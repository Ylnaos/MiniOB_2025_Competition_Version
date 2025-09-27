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

  // 提前获取去首尾空白的 SQL 文本
  auto trim_copy = [&](const string &s) -> string {
    string t = s;
    common::strip(t);
    return t;
  };
  string sql_trimmed = trim_copy(sql);

  // quick path 1: handle "CREATE TABLE <name> (<cols...>) SELECT ..."
  // 解析出表名 + 列定义 + SELECT 子句，并构造一个携带 as_select 的 CREATE TABLE 语法树
  {
    string lowered = sql_trimmed;
    common::str_to_lower(lowered);
    const string prefix = "create table ";
    size_t pos = lowered.find(prefix);
    if (pos == 0) {
      size_t p = prefix.size();
      // 读取表名
      while (p < lowered.size() && isspace(static_cast<unsigned char>(lowered[p]))) ++p;
      size_t name_start = p;
      while (p < lowered.size()) {
        char c = lowered[p];
        if (isalnum(static_cast<unsigned char>(c)) || c == '_') ++p; else break;
      }
      if (name_start < p) {
        string table_name = sql_trimmed.substr(name_start, p - name_start);
        // 跳过空白
        while (p < lowered.size() && isspace(static_cast<unsigned char>(lowered[p]))) ++p;
        if (p < lowered.size() && lowered[p] == '(') {
          // 捕获括号内的列定义（支持嵌套括号计数）
          size_t lp = p; // 指向 '('
          int    depth = 0;
          size_t q = p;
          bool   ok = false;
          while (q < lowered.size()) {
            char c = lowered[q];
            if (c == '(') { ++depth; }
            else if (c == ')') { --depth; if (depth == 0) { ok = true; ++q; break; } }
            ++q;
          }
          if (ok) {
            // q 指向右括号之后第一个字符
            size_t after_paren = q;
            // 跳过空白
            while (after_paren < lowered.size() && isspace(static_cast<unsigned char>(lowered[after_paren]))) ++after_paren;
            // 期待出现 select
            if (after_paren < lowered.size() && lowered.compare(after_paren, 6, "select") == 0) {
              string col_defs   = sql_trimmed.substr(lp, q - lp);     // 包含括号
              string select_sql = sql_trimmed.substr(after_paren);     // 余下全部

              // 1) 解析列定义：构造一个独立的 CREATE TABLE 语句，仅用于提取 attr_infos
              string create_header = "create table ";
              create_header += table_name;
              create_header += " ";
              string create_cols = create_header + col_defs;

              ParsedSqlResult cols_result;
              parse(create_cols.c_str(), &cols_result);
              vector<AttrInfoSqlNode> attr_infos;
              vector<string>          pks;
              string                  storage_fmt;
              if (!cols_result.sql_nodes().empty()) {
                ParsedSqlNode *n = cols_result.sql_nodes()[0].get();
                if (n->flag == SCF_CREATE_TABLE) {
                  attr_infos = n->create_table.attr_infos;
                  pks        = n->create_table.primary_keys;
                  storage_fmt= n->create_table.storage_format;
                }
              }

              // 2) 解析 SELECT 子句
              ParsedSqlResult sub_result;
              parse(select_sql.c_str(), &sub_result);
              int idx = -1;
              for (int i = 0; i < static_cast<int>(sub_result.sql_nodes().size()); i++) {
                auto &node_up = sub_result.sql_nodes()[i];
                if (node_up && node_up->flag != SCF_ERROR) { idx = i; break; }
              }
              if (idx >= 0 && sub_result.sql_nodes()[idx]->flag == SCF_SELECT) {
                // 构造最终 CREATE TABLE 节点，附带 as_select
                unique_ptr<ParsedSqlNode> node = make_unique<ParsedSqlNode>(SCF_CREATE_TABLE);
                node->create_table.relation_name = table_name;
                node->create_table.attr_infos    = std::move(attr_infos);  // 如有显式列定义，保留
                node->create_table.primary_keys  = std::move(pks);
                node->create_table.storage_format= std::move(storage_fmt);
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

  // quick path 2: handle CREATE TABLE ... AS SELECT ... without relying on yacc rule
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
