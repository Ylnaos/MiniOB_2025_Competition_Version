/* Copyright (c) 2021 OceanBase and/or its affiliates.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Codex Agent on 2025/2/15.
//

#include "sql/executor/drop_index_executor.h"

#include "common/log/log.h"
#include "event/sql_event.h"
#include "sql/stmt/drop_index_stmt.h"
#include "storage/table/table.h"

RC DropIndexExecutor::execute(SQLStageEvent *sql_event)
{
  Stmt *stmt = sql_event->stmt();
  ASSERT(stmt->type() == StmtType::DROP_INDEX,
      "drop index executor can not run this command: %d",
      static_cast<int>(stmt->type()));

  DropIndexStmt *drop_index_stmt = static_cast<DropIndexStmt *>(stmt);

  Table *table = drop_index_stmt->table();

  // 处理 IF EXISTS 情况：table 为 nullptr 或索引不存在
  if (drop_index_stmt->if_exists()) {
    if (table == nullptr) {
      // 表不存在，返回成功
      LOG_INFO("skip dropping index since table does not exist. index=%s",
               drop_index_stmt->index_name().c_str());
      return RC::SUCCESS;
    }

    // 检查索引是否存在
    Index *index = table->find_index(drop_index_stmt->index_name().c_str());
    if (index == nullptr) {
      // 索引不存在，返回成功
      LOG_INFO("skip dropping index since it does not exist. table=%s, index=%s",
               table->name(), drop_index_stmt->index_name().c_str());
      return RC::SUCCESS;
    }
  }

  return table->drop_index(drop_index_stmt->index_name().c_str());
}

