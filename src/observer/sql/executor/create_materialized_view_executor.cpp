/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/executor/create_materialized_view_executor.h"
#include "common/log/log.h"
#include "event/sql_event.h"
#include "event/session_event.h"
#include "session/session.h"
#include "sql/stmt/create_materialized_view_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/physical_operator.h"
#include "sql/expr/tuple.h"
#include "common/lang/string.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

namespace {

string make_temp_materialized_view_name(Db *db, const char *view_name)
{
  for (int i = 0; i < 1024; ++i) {
    string temp_name = "__mv_tmp_" + string(view_name) + "_" + to_string(i);
    if (db->find_table(temp_name.c_str()) == nullptr && db->find_view(temp_name.c_str()) == nullptr) {
      return temp_name;
    }
  }
  return "";
}

} // namespace

RC CreateMaterializedViewExecutor::execute(SQLStageEvent *sql_event)
{
  Session *session = sql_event->session_event()->session();
  Db      *db      = session->get_current_db();

  auto *stmt = static_cast<CreateMaterializedViewStmt *>(sql_event->stmt());
  if (nullptr == stmt) {
    return RC::INVALID_ARGUMENT;
  }

  const char *view_name = stmt->view_name().c_str();
  string      temp_name = make_temp_materialized_view_name(db, view_name);
  if (temp_name.empty()) {
    LOG_WARN("failed to generate temporary materialized view name. view=%s", view_name);
    return RC::EXIST;
  }

  RC rc = db->create_materialized_view(temp_name.c_str(), stmt->select_stmt());
  if (OB_FAIL(rc)) {
    return rc;
  }

  auto cleanup_temp_object = [&]() {
    RC drop_rc = RC::SUCCESS;
    if (db->find_table(temp_name.c_str()) != nullptr) {
      drop_rc = db->drop_table(temp_name.c_str());
    } else if (db->find_view(temp_name.c_str()) != nullptr) {
      drop_rc = db->drop_view(temp_name.c_str());
    }
    if (OB_FAIL(drop_rc)) {
      LOG_WARN("failed to drop temporary materialized view object. name=%s rc=%s", temp_name.c_str(), strrc(drop_rc));
    }
  };

  Table *table = db->find_table(temp_name.c_str());
  if (table == nullptr) {
    LOG_WARN("created materialized view table not found. view=%s", temp_name.c_str());
    cleanup_temp_object();
    return RC::INTERNAL;
  }

  std::unique_ptr<LogicalOperator> logical_oper;
  LogicalPlanGenerator             logical_gen;
  rc = logical_gen.create(stmt->select_stmt(), logical_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create logical plan for materialized view. rc=%s", strrc(rc));
    cleanup_temp_object();
    return rc;
  }

  std::unique_ptr<PhysicalOperator> physical_oper;
  PhysicalPlanGenerator             physical_gen;
  rc = physical_gen.create(*logical_oper, physical_oper, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create physical plan for materialized view. rc=%s", strrc(rc));
    cleanup_temp_object();
    return rc;
  }

  Trx *trx = session->current_trx();
  trx->start_if_need();

  rc = physical_oper->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open materialized view plan. rc=%s", strrc(rc));
    trx->rollback();
    cleanup_temp_object();
    return rc;
  }

  vector<RID> inserted_rids;
  auto insert_values = [&](int cols, Value *values) -> RC {
    Record record;
    RC rc = table->make_record(cols, values, record);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to make materialized view record. rc=%s", strrc(rc));
      return rc;
    }

    rc = trx->insert_record(table, record);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to insert materialized view record. rc=%s", strrc(rc));
      return rc;
    }
    inserted_rids.push_back(record.rid());
    return RC::SUCCESS;
  };

  while (RC::SUCCESS == (rc = physical_oper->next())) {
    Tuple *tuple = physical_oper->current_tuple();
    if (tuple == nullptr) {
      rc = RC::INTERNAL;
      break;
    }

    const int cols = tuple->cell_num();
    vector<Value> values(cols);
    for (int i = 0; i < cols; ++i) {
      rc = tuple->cell_at(i, values[i]);
      if (OB_FAIL(rc)) {
        break;
      }
    }
    if (OB_FAIL(rc)) {
      break;
    }

    rc = insert_values(cols, values.data());
    if (OB_FAIL(rc)) {
      break;
    }
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }

  RC close_rc = physical_oper->close();
  if (OB_SUCC(rc) && OB_FAIL(close_rc)) {
    rc = close_rc;
  }

  if (OB_FAIL(rc)) {
    for (auto it = inserted_rids.rbegin(); it != inserted_rids.rend(); ++it) {
      Record record;
      RC     get_rc = table->get_record(*it, record);
      if (OB_FAIL(get_rc)) {
        LOG_WARN("failed to get materialized view record for rollback. rc=%s", strrc(get_rc));
        continue;
      }
      RC delete_rc = trx->delete_record(table, record);
      if (OB_FAIL(delete_rc)) {
        LOG_WARN("failed to delete materialized view record for rollback. rc=%s", strrc(delete_rc));
      }
    }
    trx->rollback();
    cleanup_temp_object();
    return rc;
  }

  rc = trx->commit();
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to commit materialized view records. rc=%s", strrc(rc));
    trx->rollback();
    cleanup_temp_object();
    return rc;
  }

  rc = db->finalize_materialized_view(temp_name.c_str(), view_name);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to finalize materialized view. temp=%s view=%s rc=%s", temp_name.c_str(), view_name, strrc(rc));
    cleanup_temp_object();
    return rc;
  }

  return RC::SUCCESS;
}
