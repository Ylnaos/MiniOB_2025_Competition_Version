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
  LOG_INFO("parse stage receive sql: %s", sql.c_str());

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
  string sql_to_parse = sql;
  bool   mark_index_if_not_exists = false;
  {
    string lowered = sql_to_parse;
    common::str_to_lower(lowered);
    const string create_kw = "create";
    const string index_kw  = " index";
    size_t search_pos = 0;
    while (true) {
      size_t create_pos = lowered.find(create_kw, search_pos);
      if (create_pos == string::npos) {
        break;
      }
      size_t index_pos = lowered.find(index_kw, create_pos + create_kw.size());
      if (index_pos == string::npos) {
        search_pos = create_pos + create_kw.size();
        continue;
      }
      size_t scan_pos = index_pos + index_kw.size();
      size_t ws_pos = scan_pos;
      while (ws_pos < lowered.size() && isspace(static_cast<unsigned char>(lowered[ws_pos]))) {
        ++ws_pos;
      }
      size_t cursor = ws_pos;
      if (cursor + 2 > lowered.size() || lowered.compare(cursor, 2, "if") != 0) {
        search_pos = index_pos + index_kw.size();
        continue;
      }
      cursor += 2;
      while (cursor < lowered.size() && isspace(static_cast<unsigned char>(lowered[cursor]))) {
        ++cursor;
      }
      if (cursor + 3 > lowered.size() || lowered.compare(cursor, 3, "not") != 0) {
        search_pos = index_pos + index_kw.size();
        continue;
      }
      cursor += 3;
      while (cursor < lowered.size() && isspace(static_cast<unsigned char>(lowered[cursor]))) {
        ++cursor;
      }
      if (cursor + 6 > lowered.size() || lowered.compare(cursor, 6, "exists") != 0) {
        search_pos = index_pos + index_kw.size();
        continue;
      }
      cursor += 6;

      size_t remove_start = scan_pos;
      size_t remove_end   = cursor;

      mark_index_if_not_exists = true;
      lowered.erase(remove_start, remove_end - remove_start);
      sql_to_parse.erase(remove_start, remove_end - remove_start);
      lowered.insert(remove_start, " ");
      sql_to_parse.insert(remove_start, " ");
      LOG_INFO("normalize create index: removed IF NOT EXISTS");
      search_pos = remove_start + 1;
    }
  }

  parse(sql_to_parse.c_str(), &parsed_sql_result);
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

  if (mark_index_if_not_exists && sql_node && sql_node->flag == SCF_CREATE_INDEX) {
    sql_node->create_index.if_not_exists = true;
  }

  sql_event->set_sql_node(std::move(sql_node));

  return RC::SUCCESS;
}
