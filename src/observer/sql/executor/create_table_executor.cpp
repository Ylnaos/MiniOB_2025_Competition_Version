/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2023/6/13.
//

#include "sql/executor/create_table_executor.h"

#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/stmt/create_table_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "storage/db/db.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/operator/physical_operator.h"
#include "sql/expr/tuple.h"
// 需要完整的 Trx 类型以调用其成员函数
#include "storage/trx/trx.h"

RC CreateTableExecutor::execute(SQLStageEvent *sql_event)
{
  Stmt    *stmt    = sql_event->stmt();
  Session *session = sql_event->session_event()->session();
  ASSERT(stmt->type() == StmtType::CREATE_TABLE,
      "create table executor can not run this command: %d",
      static_cast<int>(stmt->type()));

  CreateTableStmt *create_table_stmt = static_cast<CreateTableStmt *>(stmt);

  const char *table_name = create_table_stmt->table_name().c_str();
  RC rc = session->get_current_db()->create_table(table_name, create_table_stmt->attr_infos(), create_table_stmt->primary_keys(), create_table_stmt->storage_format());

  if (rc != RC::SUCCESS) {
    return rc;
  }

  // Handle CTAS: run the select and insert results into the new table
  if (create_table_stmt->is_ctas()) {
    Db *db = session->get_current_db();
    Table *table = db->find_table(table_name);
    if (table == nullptr) {
      LOG_WARN("newly created table not found: %s", table_name);
      return RC::INTERNAL;
    }

    SelectStmt *sel = create_table_stmt->as_select_stmt();

    // Build logical & physical plan
    std::unique_ptr<LogicalOperator>  logical_oper;
    LogicalPlanGenerator              logical_gen;
    rc = logical_gen.create(sel, logical_oper);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to create logical plan for CTAS. rc=%s", strrc(rc));
      return rc;
    }

    std::unique_ptr<PhysicalOperator> physical_oper;
    PhysicalPlanGenerator             physical_gen;
    rc = physical_gen.create(*logical_oper, physical_oper, session);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to create physical plan for CTAS. rc=%s", strrc(rc));
      return rc;
    }

    rc = physical_oper->open(session->current_trx());
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to open CTAS physical operator. rc=%s", strrc(rc));
      return rc;
    }

    vector<RID> inserted_rids;

    Tuple *out_tuple = nullptr;
    while (RC::SUCCESS == (rc = physical_oper->next())) {
      out_tuple = physical_oper->current_tuple();
      if (out_tuple == nullptr) {
        rc = RC::INTERNAL;
        break;
      }

      const int cols = out_tuple->cell_num();
      vector<Value> values(cols);
      for (int i = 0; i < cols; ++i) {
        RC rc2 = out_tuple->cell_at(i, values[i]);
        if (rc2 != RC::SUCCESS) {
          rc = rc2;
          break;
        }
      }
      if (rc != RC::SUCCESS) break;

      Record record;
      rc = table->make_record(cols, values.data(), record);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to make CTAS record. rc=%s", strrc(rc));
        break;
      }
      rc = session->current_trx()->insert_record(table, record);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to insert CTAS record. rc=%s", strrc(rc));
        break;
      }
      inserted_rids.push_back(record.rid());
    }

    if (rc == RC::RECORD_EOF) {
      rc = RC::SUCCESS;
    }

    physical_oper->close();

    if (rc != RC::SUCCESS) {
      // rollback inserted rows in this statement
      for (auto it = inserted_rids.rbegin(); it != inserted_rids.rend(); ++it) {
        const RID &rid = *it;
        Record rec;
        RC rc_get = table->get_record(rid, rec);
        if (rc_get != RC::SUCCESS) {
          LOG_WARN("failed to get record for CTAS rollback. rid=%s rc=%s", rid.to_string().c_str(), strrc(rc_get));
          continue;
        }
        RC rc_del = session->current_trx()->delete_record(table, rec);
        if (rc_del != RC::SUCCESS) {
          LOG_WARN("failed to delete record for CTAS rollback. rid=%s rc=%s", rid.to_string().c_str(), strrc(rc_del));
        }
      }
    }

    return rc;
  }

  return RC::SUCCESS;
}
