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
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/operator/physical_operator.h"
#include "sql/expr/tuple.h"
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
  RC rc = session->get_current_db()->create_table(
      table_name,
      create_table_stmt->attr_infos(),
      create_table_stmt->primary_keys(),
      create_table_stmt->storage_format());

  if (OB_FAIL(rc)) {
    return rc;
  }

  // 若为 CTAS，则执行 SELECT 并将结果插入新表
  if (create_table_stmt->is_ctas()) {
    // 获取新表对象
    Table *new_table = session->get_current_db()->find_table(table_name);
    if (nullptr == new_table) {
      return RC::INTERNAL;
    }

    // 基于 select_stmt 生成并执行物理计划（使用行迭代模式以简化导入逻辑）
    LogicalPlanGenerator  lg;
    PhysicalPlanGenerator pg;
    std::unique_ptr<LogicalOperator>  logical_op;
    std::unique_ptr<PhysicalOperator> phys_op;

    rc = lg.create(static_cast<Stmt *>(create_table_stmt->select_stmt()), logical_op);
    if (OB_FAIL(rc)) {
      return rc;
    }
    // 统一生成子节点
    logical_op->generate_general_child();
    rc = pg.create(*logical_op, phys_op, session);
    if (OB_FAIL(rc)) {
      return rc;
    }

    // 打开并逐行读取
    Trx *trx = session->current_trx();
    rc       = phys_op->open(trx);
    if (OB_FAIL(rc)) {
      return rc;
    }

    vector<RID> inserted_rids;
    int         col_num = static_cast<int>(create_table_stmt->attr_infos().size());
    for (;;) {
      RC rn = phys_op->next();
      if (rn == RC::RECORD_EOF) break;
      if (rn != RC::SUCCESS) {
        rc = rn;
        break;
      }
      Tuple *tuple = phys_op->current_tuple();
      if (nullptr == tuple) {
        rc = RC::INTERNAL;
        break;
      }
      vector<Value> row_values;
      row_values.reserve(col_num);
      for (int i = 0; i < col_num; i++) {
        Value v;
        RC rv = tuple->cell_at(i, v);
        if (OB_FAIL(rv)) {
          rc = rv;
          break;
        }
        row_values.emplace_back(v);
      }
      if (OB_FAIL(rc)) break;

      Record record;
      rc = new_table->make_record(col_num, row_values.data(), record);
      if (OB_FAIL(rc)) {
        break;
      }
      rc = trx->insert_record(new_table, record);
      if (OB_FAIL(rc)) {
        break;
      }
      inserted_rids.push_back(record.rid());
    }

    // 关闭算子
    phys_op->close();

    if (OB_FAIL(rc)) {
      // 回滚本语句已写入的新表数据，尽力而为
      for (auto it = inserted_rids.rbegin(); it != inserted_rids.rend(); ++it) {
        const RID &rid = *it;
        Record     rec;
        RC         grc = new_table->get_record(rid, rec);
        if (OB_SUCC(grc)) {
          RC drc = trx->delete_record(new_table, rec);
          (void)drc;
        }
      }
      return rc;
    }
  }

  return rc;
}
