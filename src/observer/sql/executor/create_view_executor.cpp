/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/executor/create_view_executor.h"
#include "event/sql_event.h"
#include "event/session_event.h"
#include "session/session.h"
#include "sql/stmt/create_view_stmt.h"
#include "storage/db/db.h"

RC CreateViewExecutor::execute(SQLStageEvent *sql_event)
{
  Session *session = sql_event->session_event()->session();
  Db      *db      = session->get_current_db();

  auto *stmt = static_cast<CreateViewStmt *>(sql_event->stmt());
  if (nullptr == stmt) {
    LOG_DEBUG("DEBUG_LOG: CreateViewExecutor失败 - stmt为空");
    return RC::INVALID_ARGUMENT;
  }

  LOG_DEBUG("DEBUG_LOG: CreateViewExecutor开始执行视图创建 - 视图名=%s, 查询SQL=%s, 字段数=%zu",
            stmt->view_name().c_str(), stmt->view_select_sql().c_str(), stmt->view_fields().size());

  for (size_t i = 0; i < stmt->view_fields().size(); i++) {
    LOG_DEBUG("DEBUG_LOG: CreateViewExecutor视图字段[%zu] - %s", i, stmt->view_fields()[i].c_str());
  }

  RC rc = db->create_view(stmt->view_name().c_str(), stmt->view_select_sql().c_str(), stmt->view_fields());

  LOG_DEBUG("DEBUG_LOG: CreateViewExecutor视图创建完成 - RC=%s, 视图名=%s", strrc(rc), stmt->view_name().c_str());

  return rc;
}

