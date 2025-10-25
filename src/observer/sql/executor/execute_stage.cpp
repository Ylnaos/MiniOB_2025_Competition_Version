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
// Created by Longda on 2021/4/13.
//

#include "sql/executor/execute_stage.h"

#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "sql/executor/command_executor.h"
#include "sql/operator/calc_physical_operator.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/stmt.h"
#include "storage/default/default_handler.h"

using namespace common;

// 全局计数器，追踪查询执行
static int execute_stage_query_count = 0;

RC ExecuteStage::handle_request(SQLStageEvent *sql_event)
{
  execute_stage_query_count++;

  // 强制输出到控制台，确保能看到调试信息
  printf("=== FORCE OUTPUT: ExecuteStage::handle_request开始 [%d] ===\n", execute_stage_query_count);
  fflush(stdout);

  LOG_INFO("DATA_FLOW: ExecuteStage::handle_request开始 - 执行次数=%d", execute_stage_query_count);

  RC rc = RC::SUCCESS;

  const unique_ptr<PhysicalOperator> &physical_operator = sql_event->physical_operator();
  if (physical_operator != nullptr) {
    LOG_INFO("DATA_FLOW: ExecuteStage使用物理操作符执行 - 操作符=%p, 类型=%d",
              physical_operator.get(), static_cast<int>(physical_operator->type()));
    return handle_request_with_physical_operator(sql_event);
  }

  SessionEvent *session_event = sql_event->session_event();

  Stmt *stmt = sql_event->stmt();
  if (stmt != nullptr) {
    LOG_INFO("DATA_FLOW: ExecuteStage使用命令执行器 - stmt=%p, 类型=%d",
              stmt, static_cast<int>(stmt->type()));
    CommandExecutor command_executor;
    rc = command_executor.execute(sql_event);
    session_event->sql_result()->set_return_code(rc);
    LOG_INFO("DATA_FLOW: ExecuteStage命令执行完成 - RC=%s", strrc(rc));
  } else {
    LOG_INFO("DATA_FLOW: ExecuteStage执行失败 - stmt为空");
    return RC::INTERNAL;
  }
  return rc;
}

RC ExecuteStage::handle_request_with_physical_operator(SQLStageEvent *sql_event)
{
  RC rc = RC::SUCCESS;

  unique_ptr<PhysicalOperator> &physical_operator = sql_event->physical_operator();
  ASSERT(physical_operator != nullptr, "physical operator should not be null");

  LOG_INFO("DATA_FLOW: ExecuteStage设置物理操作符到结果集 - 操作符=%p, 类型=%d",
            physical_operator.get(), static_cast<int>(physical_operator->type()));

  SqlResult *sql_result = sql_event->session_event()->sql_result();
  sql_result->set_operator(std::move(physical_operator));

  LOG_INFO("DATA_FLOW: ExecuteStage物理操作符设置完成");
  return rc;
}
