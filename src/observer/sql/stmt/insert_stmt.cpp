/* Copyright (c) 2021OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2022/5/22.
//

#include "sql/stmt/insert_stmt.h"
#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/view/view.h"

namespace {
// 从简单的 SELECT 语句中提取 FROM 后的第一个表名。
// 仅用于将 "INSERT INTO <view> ..." 简单重写为向底层单表插入的场景：
//   CREATE VIEW v AS SELECT * FROM base;
// 若无法可靠提取，则返回空串。
static std::string extract_first_table_name(const std::string &sql)
{
  if (sql.empty()) return {};
  std::string lower = sql;
  for (auto &ch : lower) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));

  auto find_ci = [&](const std::string &pat, size_t pos) -> size_t {
    std::string p = pat;
    for (auto &c : p) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return lower.find(p, pos);
  };

  size_t from_pos = find_ci(" from ", 0);
  if (from_pos == std::string::npos) return {};
  size_t i = from_pos + 6; // skip " from "
  // skip spaces
  while (i < lower.size() && isspace(static_cast<unsigned char>(lower[i]))) i++;
  size_t start = i;
  // accept identifier characters: letters, digits, underscore and dot
  while (i < lower.size()) {
    char c = lower[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.') {
      ++i;
    } else {
      break;
    }
  }
  if (i <= start) return {};
  // strip possible schema prefix db.table -> table
  std::string name = sql.substr(start, i - start);
  size_t dot = name.rfind('.');
  if (dot != std::string::npos && dot + 1 < name.size()) {
    name = name.substr(dot + 1);
  }
  // trim trailing spaces if any (unlikely)
  while (!name.empty() && isspace(static_cast<unsigned char>(name.back()))) name.pop_back();
  return name;
}
}

InsertStmt::InsertStmt(Table *table, vector<vector<Value>> values_rows)
    : table_(table), values_rows_(std::move(values_rows))
{}

RC InsertStmt::create(Db *db, const InsertSqlNode &inserts, Stmt *&stmt)
{
  const char *table_name = inserts.relation_name.c_str();
  if (nullptr == db || nullptr == table_name || inserts.rows.empty()) {
    LOG_WARN("invalid argument. db=%p, table_name=%p, row_num=%d",
        db, table_name, static_cast<int>(inserts.rows.size()));
    return RC::INVALID_ARGUMENT;
  }

  // check whether the table exists; if not, try view rewrite (insert into view -> base table)
  Table *table = db->find_table(table_name);
  if (nullptr == table) {
    View *view = db->find_view(table_name);
    if (view != nullptr) {
      std::string base = extract_first_table_name(view->select_sql());
      if (!base.empty()) {
        table = db->find_table(base.c_str());
        if (table != nullptr) {
          LOG_INFO("rewrite insert into view(%s) to base table(%s)", table_name, base.c_str());
        }
      }
    }
    if (nullptr == table) {
      LOG_WARN("no such table or unsupported view insert. db=%s, target=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
  }

  // check the fields number for each row
  const TableMeta &table_meta = table->table_meta();
  const int        field_num  = table_meta.field_num() - table_meta.sys_field_num();
  for (size_t i = 0; i < inserts.rows.size(); i++) {
    const int value_num = static_cast<int>(inserts.rows[i].size());
    if (field_num != value_num) {
      LOG_WARN("schema mismatch at row %d. value num=%d, field num in schema=%d", (int)i, value_num, field_num);
      return RC::SCHEMA_FIELD_MISSING;
    }
  }

  // everything alright
  stmt = new InsertStmt(table, inserts.rows);
  return RC::SUCCESS;
}
