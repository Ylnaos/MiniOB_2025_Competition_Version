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

#include <string.h>

#include "parse_stage.h"

#include "common/conf/ini.h"
#include "common/io/io.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "sql/parser/parse.h"

using namespace common;

RC ParseStage::handle_request(SQLStageEvent *sql_event)
{
  RC rc = RC::SUCCESS;

  SqlResult         *sql_result = sql_event->session_event()->sql_result();
  const string &sql        = sql_event->sql();

  ParsedSqlResult parsed_sql_result;

  parse(sql.c_str(), &parsed_sql_result);
  if (parsed_sql_result.sql_nodes().empty()) {
    sql_result->set_return_code(RC::SUCCESS);
    sql_result->set_state_string("");
    return RC::INTERNAL;
  }

  // 统计有效(非错误)语句数量，并记录第一条有效语句位置
  int valid_count = 0;
  int first_valid_idx = -1;
  for (int i = 0; i < static_cast<int>(parsed_sql_result.sql_nodes().size()); i++) {
    auto &node_up = parsed_sql_result.sql_nodes()[i];
    if (node_up && node_up->flag != SCF_ERROR) {
      if (first_valid_idx < 0) first_valid_idx = i;
      ++valid_count;
    }
  }

  // 优先取第一条非错误节点；若全是错误节点，仍按原始第一个返回
  unique_ptr<ParsedSqlNode> sql_node;
  if (first_valid_idx >= 0) {
    sql_node = std::move(parsed_sql_result.sql_nodes()[first_valid_idx]);
  } else {
    sql_node = std::move(parsed_sql_result.sql_nodes().front());
  }

  // 如确有多条有效语句，仅处理第一条并提示
  if (valid_count > 1) {
    LOG_WARN("got multi sql commands but only 1 will be handled");
  }

  if (sql_node->flag == SCF_ERROR) {
    // set error information to event
    rc = RC::SQL_SYNTAX;
    sql_result->set_return_code(rc);
    sql_result->set_state_string("Failed to parse sql");
    return rc;
  }

  sql_event->set_sql_node(std::move(sql_node));

  return RC::SUCCESS;
}
