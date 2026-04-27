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
#include "sql/expr/expression_iterator.h"
#include "sql/expr/tuple.h"
#include "common/lang/string.h"
#include "storage/common/chunk.h"
#include "storage/common/meta_util.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

#include <strings.h>
#include <unistd.h>

namespace {

bool file_exists(const string &path)
{
  return access(path.c_str(), F_OK) == 0;
}

bool materialized_view_temp_name_available(Db *db, const string &temp_name)
{
  if (db->find_table(temp_name.c_str()) != nullptr || db->find_view(temp_name.c_str()) != nullptr) {
    return false;
  }

  const string db_path = db->path();
  return !file_exists(table_meta_file(db_path.c_str(), temp_name.c_str())) &&
         !file_exists(table_data_file(db_path.c_str(), temp_name.c_str())) &&
         !file_exists(table_lob_file(db_path.c_str(), temp_name.c_str())) &&
         !file_exists(view_meta_file(db_path.c_str(), temp_name.c_str()));
}

string make_temp_materialized_view_name(Db *db, const char *view_name)
{
  for (int i = 0; i < 1024; ++i) {
    string temp_name = "__mv_tmp_" + string(view_name) + "_" + to_string(i);
    if (materialized_view_temp_name_available(db, temp_name)) {
      return temp_name;
    }
  }
  return "";
}

bool same_name(const string &lhs, const char *rhs)
{
  return rhs != nullptr && 0 == strcasecmp(lhs.c_str(), rhs);
}

bool select_uses_table_name(const SelectStmt *select_stmt, const char *table_name)
{
  if (select_stmt == nullptr || table_name == nullptr || table_name[0] == '\0') {
    return false;
  }

  for (Table *table : select_stmt->tables()) {
    if (table != nullptr && same_name(table->name(), table_name)) {
      return true;
    }
  }

  for (const SelectStmt::FromItem &item : select_stmt->from_items()) {
    if (item.type == SelectStmt::FromItem::Type::DERIVED && select_uses_table_name(item.derived, table_name)) {
      return true;
    }
  }

  if (select_uses_table_name(select_stmt->inner_view_stmt(), table_name)) {
    return true;
  }

  for (const SelectStmt::SetOperation &set_op : select_stmt->set_operations()) {
    if (select_uses_table_name(set_op.stmt.get(), table_name)) {
      return true;
    }
  }

  return false;
}

bool expression_contains_subquery(Expression &expr)
{
  if (expr.type() == ExprType::SUBQUERY || expr.type() == ExprType::EXISTS) {
    return true;
  }

  bool found = false;
  (void)ExpressionIterator::iterate_child_expr(expr, [&](std::unique_ptr<Expression> &child) -> RC {
    if (child != nullptr && expression_contains_subquery(*child)) {
      found = true;
    }
    return RC::SUCCESS;
  });
  return found;
}

bool logical_plan_can_create_vec(LogicalOperator &oper)
{
  switch (oper.type()) {
    case LogicalOperatorType::TABLE_GET:
    case LogicalOperatorType::PREDICATE:
    case LogicalOperatorType::PROJECTION:
    case LogicalOperatorType::GROUP_BY:
    case LogicalOperatorType::ORDER_BY:
      break;
    default:
      return false;
  }

  for (const auto &expr : oper.expressions()) {
    if (expr != nullptr && expression_contains_subquery(*expr)) {
      return false;
    }
  }

  for (const auto &child : oper.children()) {
    if (child == nullptr || !logical_plan_can_create_vec(*child)) {
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
  if (select_uses_table_name(stmt->select_stmt(), view_name)) {
    LOG_WARN("materialized view definition references itself. view=%s", view_name);
    return RC::SCHEMA_TABLE_EXIST;
  }

  RC rc = RC::SUCCESS;
  if (db->find_table(view_name) != nullptr) {
    rc = db->drop_table(view_name);
    if (OB_FAIL(rc)) {
      return rc;
    }
  } else if (db->find_view(view_name) != nullptr) {
    rc = db->drop_view(view_name);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  string      temp_name = make_temp_materialized_view_name(db, view_name);
  if (temp_name.empty()) {
    LOG_WARN("failed to generate temporary materialized view name. view=%s", view_name);
    return RC::EXIST;
  }

  rc = db->create_materialized_view(temp_name.c_str(), stmt->select_stmt());
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
  bool use_chunk_iterator = session->get_execution_mode() == ExecutionMode::CHUNK_ITERATOR &&
                            logical_plan_can_create_vec(*logical_oper);
  if (use_chunk_iterator) {
    rc = physical_gen.create_vec(*logical_oper, physical_oper, session);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to create vectorized physical plan for materialized view. rc=%s", strrc(rc));
      cleanup_temp_object();
      return rc;
    }
  } else {
    rc = physical_gen.create(*logical_oper, physical_oper, session);
  }
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

  if (use_chunk_iterator) {
    Chunk chunk;
    while (RC::SUCCESS == (rc = physical_oper->next(chunk))) {
      const int cols = chunk.column_num();
      vector<Value> values(cols);
      for (int row = 0; row < chunk.rows(); ++row) {
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
      chunk.reset();
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
