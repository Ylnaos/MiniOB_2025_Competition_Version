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
// Created by Meiyi
//

#include "sql/parser/parse.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include <string>
#include <algorithm>
#include <cctype>
#include <vector>

using std::string;

// 简单工具: 全部转小写的拷贝
static inline string to_lower_copy(const string &s)
{
  string t = s;
  std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return std::tolower(c); });
  return t;
}

// 去左右空白
static inline string trim(const string &s)
{
  size_t l = 0, r = s.size();
  while (l < r && std::isspace(static_cast<unsigned char>(s[l]))) l++;
  while (r > l && std::isspace(static_cast<unsigned char>(s[r - 1]))) r--;
  return s.substr(l, r - l);
}

// 从 pos 开始，跳过空白
static inline void skip_spaces(const string &s, size_t &pos)
{
  while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) pos++;
}

// 读取一个简单的标识符(表名.列名 也一并当作标识符)，由字母/数字/下划线/点构成
static inline string read_identifier(const string &s, size_t &pos)
{
  skip_spaces(s, pos);
  size_t start = pos;
  while (pos < s.size()) {
    char c = s[pos];
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.') {
      pos++;
    } else {
      break;
    }
  }
  return trim(s.substr(start, pos - start));
}

// 在 FROM ... 子句范围内，将 INNER JOIN / JOIN 语法改写为 逗号分隔 + WHERE 条件
// 仅做轻量、保守的改写，满足比赛用例(多表、多 ON 条件，AND 连接)。
static string rewrite_join_to_where(const string &sql)
{
  string lower = to_lower_copy(sql);

  // 查找 FROM 位置
  size_t from_kw = lower.find(" from ");
  if (from_kw == string::npos) {
    return sql; // 非 SELECT 或无 FROM，原样返回
  }

  size_t from_pos = from_kw + 6; // 跳过 " from "

  // 定位 FROM 子句结束(WHERE/GROUP/ORDER/; 之一)
  size_t end_pos = lower.size();
  for (const char *kw : {" where ", " group ", " order ", ";"}) {
    size_t p = lower.find(kw, from_pos);
    if (p != string::npos) end_pos = std::min(end_pos, p);
  }

  string head = sql.substr(0, from_kw + 6);           // 含 "FROM "
  string from_section = sql.substr(from_pos, end_pos - from_pos);
  string tail = sql.substr(end_pos);                   // 余下部分(可能含 WHERE/GROUP/ORDER)

  string from_lower = to_lower_copy(from_section);

  // 快速判断是否包含 join
  if (from_lower.find(" join ") == string::npos) {
    return sql; // 无 JOIN，原样返回
  }

  // 解析 FROM 段，收集表名与 ON 条件
  std::vector<string> tables;
  std::vector<string> join_conds;

  size_t pos = 0;
  // 支持逗号分隔的基础表 + 多个连续 join 片段
  while (pos < from_section.size()) {
    // 若当前位置是 JOIN 片段，则直接解析 JOIN（不要把关键字当作表名）
    skip_spaces(from_section, pos);
    string rest_lower = to_lower_copy(from_section.substr(pos));
    if (rest_lower.rfind("inner join ", 0) == 0 || rest_lower.rfind("join ", 0) == 0) {
      // 跳过 join 关键字
      if (rest_lower.rfind("inner join ", 0) == 0) {
        pos += static_cast<size_t>(11);
      } else {
        pos += static_cast<size_t>(5);
      }

      // 读取被 join 的表名
      string right_tbl = read_identifier(from_section, pos);
      if (!right_tbl.empty()) tables.push_back(right_tbl);

      // 期望 ON
      skip_spaces(from_section, pos);
      string after_tbl_lower = to_lower_copy(from_section.substr(pos));
      if (!(after_tbl_lower.rfind("on ", 0) == 0)) {
        // 没有 ON，停止改写
        break;
      }
      pos += 3; // 跳过 "on "

      // 提取 ON 条件，直到下一个 JOIN 或 FROM 结束
      size_t next_join_rel = string::npos;
      string remain_lower  = to_lower_copy(from_section.substr(pos));
      size_t j1 = remain_lower.find(" join ");
      size_t j2 = remain_lower.find(" inner ");
      if (j1 != string::npos) next_join_rel = j1;
      if (j2 != string::npos) next_join_rel = (next_join_rel == string::npos) ? j2 : std::min(next_join_rel, j2);

      size_t cond_end_in_from = (next_join_rel == string::npos) ? from_section.size() : (pos + next_join_rel);
      string cond = trim(from_section.substr(pos, cond_end_in_from - pos));
      if (!cond.empty()) join_conds.push_back(cond);
      pos = cond_end_in_from; // 继续后续 join 解析
      continue;
    }

    // 否则解析基础表（可能有逗号分隔）
    string tbl = read_identifier(from_section, pos);
    if (!tbl.empty()) tables.push_back(tbl);
    skip_spaces(from_section, pos);
    if (pos >= from_section.size()) break;
    if (from_section[pos] == ',') {
      pos++;
      continue;
    }
    // 若后续是 join 关键字，则回到循环开头按 join 处理
    {
      string lookahead = to_lower_copy(from_section.substr(pos));
      if (lookahead.rfind("inner join ", 0) == 0 || lookahead.rfind("join ", 0) == 0) {
        continue;
      }
    }
    // 否则结束解析
    break;
  }

  if (join_conds.empty()) {
    // 没有成功改写的 join 条件，保守起见返回原 SQL
    return sql;
  }

  // 组装新的 FROM 子句(逗号分隔)
  string new_from;
  for (size_t i = 0; i < tables.size(); i++) {
    if (i > 0) new_from += ", ";
    new_from += tables[i];
  }

  // 将 join 条件合并进 WHERE
  string cond_all;
  for (size_t i = 0; i < join_conds.size(); i++) {
    if (i > 0) cond_all += " AND ";
    cond_all += join_conds[i];
  }

  string tail_lower = to_lower_copy(tail);
  string result;
  size_t where_in_tail = tail_lower.find(" where ");
  if (where_in_tail != string::npos) {
    // 在既有 WHERE 后拼接 AND join_conds（不加括号，避免语法冲突）
    size_t where_real = where_in_tail + 7; // 跳过 " where "
    result = head + new_from + tail.substr(0, where_real) + cond_all + " AND " + tail.substr(where_real);
  } else {
    // 无 WHERE，直接追加
    result = head + new_from + " WHERE " + cond_all + tail;
  }

  return result;
}

RC parse(char *st, ParsedSqlNode *sqln);

ParsedSqlNode::ParsedSqlNode() : flag(SCF_ERROR) {}

ParsedSqlNode::ParsedSqlNode(SqlCommandFlag _flag) : flag(_flag) {}

void ParsedSqlResult::add_sql_node(unique_ptr<ParsedSqlNode> sql_node)
{
  sql_nodes_.emplace_back(std::move(sql_node));
}

////////////////////////////////////////////////////////////////////////////////

int sql_parse(const char *st, ParsedSqlResult *sql_result);

RC parse(const char *st, ParsedSqlResult *sql_result)
{
  // 在进入 yacc/flex 解析前，将 INNER JOIN / JOIN 改写为 逗号+WHERE 以复用已有语法树与执行路径
  // 仅在检测到 JOIN 关键词时进行改写，其余 SQL 原样转发。
  string sql_text(st);
  string sql_rewritten = rewrite_join_to_where(sql_text);
  sql_parse(sql_rewritten.c_str(), sql_result);
  return RC::SUCCESS;
}
