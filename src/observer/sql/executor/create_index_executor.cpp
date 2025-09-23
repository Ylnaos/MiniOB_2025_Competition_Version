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
// Created by Wangyunlai on 2023/4/25.
//

#include "sql/executor/create_index_executor.h"
#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/stmt/create_index_stmt.h"
#include "storage/table/table.h"
#include <vector>

RC CreateIndexExecutor::execute(SQLStageEvent *sql_event)
{
  Stmt    *stmt    = sql_event->stmt();
  Session *session = sql_event->session_event()->session();
  ASSERT(stmt->type() == StmtType::CREATE_INDEX,
      "create index executor can not run this command: %d",
      static_cast<int>(stmt->type()));

  CreateIndexStmt *create_index_stmt = static_cast<CreateIndexStmt *>(stmt);

  Trx   *trx   = session->current_trx();
  Table *table = create_index_stmt->table();
  // 将字段指针列表拷贝为连续的 FieldMeta 数组，便于构造 span<const FieldMeta>
  const auto &fms = create_index_stmt->field_metas();
  std::vector<FieldMeta> field_metas;
  field_metas.reserve(fms.size());
  for (const FieldMeta *fm : fms) {
    field_metas.push_back(*fm);
  }
  return table->create_index(trx,
                             span<const FieldMeta>(field_metas.data(), field_metas.size()),
                             create_index_stmt->index_name().c_str(),
                             create_index_stmt->unique());
}
