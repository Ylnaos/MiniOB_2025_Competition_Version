/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/executor/drop_view_executor.h"

#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/stmt/drop_view_stmt.h"
#include "storage/db/db.h"
#include "storage/view/view.h"

RC DropViewExecutor::execute(SQLStageEvent *sql_event)
{
  Stmt    *stmt    = sql_event->stmt();
  Session *session = sql_event->session_event()->session();
  ASSERT(stmt->type() == StmtType::DROP_VIEW,
      "drop view executor can not run this command: %d",
      static_cast<int>(stmt->type()));

  DropViewStmt *drop_view_stmt = static_cast<DropViewStmt *>(stmt);

  const char *view_name = drop_view_stmt->view_name().c_str();

  // 检查视图是否存在（如果 stmt 中已经标记了 if_exists，说明 create 阶段视图不存在）
  // 在这种情况下，跳过删除并返回成功
  if (drop_view_stmt->if_exists()) {
    // 再次检查视图是否存在
    View *view = session->get_current_db()->find_view(view_name);
    if (view == nullptr) {
      // 视图不存在，返回成功
      LOG_INFO("skip dropping view since it does not exist. view=%s", view_name);
      return RC::SUCCESS;
    }
  }

  RC rc = session->get_current_db()->drop_view(view_name);

  return rc;
}
