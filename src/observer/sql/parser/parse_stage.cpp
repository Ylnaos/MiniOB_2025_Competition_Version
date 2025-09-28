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
#include <ctype.h>

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

  // quick path: handle CREATE TABLE ... AS SELECT ... without relying on yacc rule
  auto trim_copy = [&](const string &s) -> string {
    string t = s;
    common::strip(t);
    return t;
  };
  string sql_trimmed = trim_copy(sql);
  {
    string lowered = sql_trimmed;
    common::str_to_lower(lowered);
    // accept pattern: create table <id> as select ...
    const string prefix = "create table ";
    size_t pos = lowered.find(prefix);
    if (pos == 0) {
      size_t p = prefix.size();
      // extract table name (letters/digits/underscore)
      while (p < lowered.size() && isspace(static_cast<unsigned char>(lowered[p]))) ++p;
      size_t name_start = p;
      while (p < lowered.size()) {
        char c = lowered[p];
        if (isalnum(static_cast<unsigned char>(c)) || c == '_' ) {
          ++p;
        } else {
          break;
        }
      }
      if (name_start < p) {
        string table_name = sql_trimmed.substr(name_start, p - name_start);
        // skip spaces
        while (p < lowered.size() && isspace(static_cast<unsigned char>(lowered[p]))) ++p;
        // expect "as"
        if (p + 2 <= lowered.size() && lowered.compare(p, 2, "as") == 0) {
          p += 2;
          while (p < lowered.size() && isspace(static_cast<unsigned char>(lowered[p]))) ++p;
          // expect select
          if (p < lowered.size() && lowered.compare(p, 6, "select") == 0) {
            string select_sql = sql_trimmed.substr(p);

            ParsedSqlResult sub_result;
            parse(select_sql.c_str(), &sub_result);
            if (!sub_result.sql_nodes().empty()) {
              // pick first non-error
              int idx = -1;
              for (int i = 0; i < static_cast<int>(sub_result.sql_nodes().size()); i++) {
                auto &node_up = sub_result.sql_nodes()[i];
                if (node_up && node_up->flag != SCF_ERROR) { idx = i; break; }
              }
              if (idx >= 0 && sub_result.sql_nodes()[idx]->flag == SCF_SELECT) {
                // build CTAS node
                unique_ptr<ParsedSqlNode> node = make_unique<ParsedSqlNode>(SCF_CREATE_TABLE);
                node->create_table.relation_name = table_name;
                node->create_table.as_select.reset(sub_result.sql_nodes()[idx].release());
                sql_event->set_sql_node(std::move(node));
                return RC::SUCCESS;
              }
            }
          }
        }
      }
    }
  }

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

  // 如确有多条有效语句：仅处理“最后一条”，与常见客户端行为保持一致
  if (valid_count > 1) {
    int __last_valid_idx = -1;
    for (int i = 0; i < static_cast<int>(parsed_sql_result.sql_nodes().size()); i++) {
      auto &__node_up = parsed_sql_result.sql_nodes()[i];
      if (__node_up && __node_up->flag != SCF_ERROR) {
        __last_valid_idx = i;
      }
    }
    if (__last_valid_idx >= 0) {
      sql_node = std::move(parsed_sql_result.sql_nodes()[__last_valid_idx]);
    }
    LOG_WARN("got multi sql commands but only the last one will be handled");
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
