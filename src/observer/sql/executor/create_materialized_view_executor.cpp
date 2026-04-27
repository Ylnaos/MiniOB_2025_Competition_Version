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
#include "storage/common/chunk.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

namespace {

bool can_materialize_with_chunk(LogicalOperator &oper)
{
  switch (oper.type()) {
    case LogicalOperatorType::TABLE_GET:
    case LogicalOperatorType::PREDICATE:
    case LogicalOperatorType::PROJECTION:
      break;
    default:
      return false;
  }

  for (const auto &child : oper.children()) {
    if (child == nullptr || !can_materialize_with_chunk(*child)) {
      return false;
    }
  }
  return true;
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
  RC          rc        = db->create_materialized_view(view_name, stmt->select_stmt());
  if (OB_FAIL(rc)) {
    return rc;
  }

  Table *table = db->find_table(view_name);
  if (table == nullptr) {
    LOG_WARN("created materialized view table not found. view=%s", view_name);
    return RC::INTERNAL;
  }

  std::unique_ptr<LogicalOperator> logical_oper;
  LogicalPlanGenerator             logical_gen;
  rc = logical_gen.create(stmt->select_stmt(), logical_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create logical plan for materialized view. rc=%s", strrc(rc));
    db->drop_table(view_name);
    return rc;
  }

  std::unique_ptr<PhysicalOperator> physical_oper;
  PhysicalPlanGenerator             physical_gen;
  const bool use_chunk = session->get_execution_mode() == ExecutionMode::CHUNK_ITERATOR &&
                         can_materialize_with_chunk(*logical_oper);
  rc = use_chunk ? physical_gen.create_vec(*logical_oper, physical_oper, session)
                 : physical_gen.create(*logical_oper, physical_oper, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create physical plan for materialized view. rc=%s", strrc(rc));
    db->drop_table(view_name);
    return rc;
  }

  Trx *trx = session->current_trx();
  trx->start_if_need();

  rc = physical_oper->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open materialized view plan. rc=%s", strrc(rc));
    db->drop_table(view_name);
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

  if (use_chunk) {
    Chunk chunk;
    while (RC::SUCCESS == (rc = physical_oper->next(chunk))) {
      const int rows = chunk.rows();
      const int cols = chunk.column_num();
      vector<Value> values(cols);
      for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < cols; ++col) {
          values[col] = chunk.get_value(col, row);
        }
        rc = insert_values(cols, values.data());
        if (OB_FAIL(rc)) {
          break;
        }
      }
      if (OB_FAIL(rc)) {
        break;
      }
    }
  } else {
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
    db->drop_table(view_name);
    return rc;
  }

  rc = trx->commit();
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to commit materialized view records. rc=%s", strrc(rc));
    return rc;
  }

  return RC::SUCCESS;
}
