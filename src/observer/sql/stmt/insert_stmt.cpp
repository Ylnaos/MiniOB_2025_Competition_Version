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
#include "common/lang/string.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/parser/parse.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/view/view.h"

namespace {
// 从简单的 SELECT 语句中提取 FROM 后的第一个表名。
// 仅用于将 "INSERT INTO <view> ..." 简单重写为向底层单表插入的场景：
//   CREATE VIEW v AS SELECT * FROM base;
// 若无法可靠提取，则返回空串。
static std::string to_lower_copy(const std::string &input)
{
  std::string tmp = input;
  common::str_to_lower(tmp);
  return tmp;
}

static bool equals_ignore_case(const std::string &lhs, const std::string &rhs)
{
  return to_lower_copy(lhs) == to_lower_copy(rhs);
}

static bool match_table_name(const std::string &name, const RelationSqlNode &rel)
{
  if (name.empty()) return true;
  if (equals_ignore_case(name, rel.relation_name)) return true;
  if (!rel.alias.empty() && equals_ignore_case(name, rel.alias)) return true;
  return false;
}

static RC build_default_row(const TableMeta &table_meta, std::vector<Value> &defaults)
{
  const int sys_num    = table_meta.sys_field_num();
  const int field_num  = table_meta.field_num();
  const int normal_num = field_num - sys_num;

  defaults.clear();
  defaults.resize(normal_num);

  for (int i = 0; i < normal_num; ++i) {
    const FieldMeta *field = table_meta.field(i + sys_num);
    if (field == nullptr) {
      LOG_WARN("unexpected null field meta when preparing default row. table=%s index=%d",
          table_meta.name(), i + sys_num);
      return RC::INTERNAL;
    }

    Value def_val;
    switch (field->type()) {
      case AttrType::INTS: {
        def_val.set_int(0);
      } break;
      case AttrType::FLOATS: {
        def_val.set_float(0.0f);
      } break;
      case AttrType::CHARS: {
        def_val.set_empty_string(field->len());
      } break;
      case AttrType::TEXTS: {
        def_val.set_string("", 0);
      } break;
      case AttrType::BOOLEANS: {
        def_val.set_boolean(false);
      } break;
      case AttrType::DATES: {
        def_val.set_date(19700101);  // 选取合法的日期作为缺省填充值
      } break;
      case AttrType::NULLS: {
        def_val.set_null();
      } break;
      default: {
        LOG_WARN("unsupported field type for default fill. table=%s field=%s type=%d",
            table_meta.name(), field->name(), static_cast<int>(field->type()));
        return RC::UNIMPLEMENTED;
      }
    }

    defaults[i] = std::move(def_val);
  }

  return RC::SUCCESS;
}

static RC build_view_insert_mapping(
    const SelectSqlNode &view_select,
    const TableMeta       &table_meta,
    std::vector<int>      &value_to_field)
{
  if (view_select.relations.size() != 1) {
    LOG_WARN("view insert only supports single base table. relation_count=%zu",
        view_select.relations.size());
    return RC::UNIMPLEMENTED;
  }

  const RelationSqlNode &rel = view_select.relations[0];
  const int              sys_num = table_meta.sys_field_num();
  const int              field_num = table_meta.field_num();
  const int              normal_num = field_num - sys_num;

  value_to_field.clear();
  value_to_field.reserve(view_select.expressions.size());

  auto append_field = [&](const std::string &field_name) -> RC {
    for (int i = 0; i < normal_num; ++i) {
      const FieldMeta *field = table_meta.field(i + sys_num);
      if (field != nullptr && equals_ignore_case(field_name, field->name())) {
        value_to_field.push_back(i);
        return RC::SUCCESS;
      }
    }
    LOG_WARN("view insert field not found in base table. table=%s field=%s", table_meta.name(), field_name.c_str());
    return RC::SCHEMA_FIELD_MISSING;
  };

  for (const auto &expr : view_select.expressions) {
    if (!expr) {
      LOG_WARN("nullptr expression in view definition when rewriting insert. view_relation=%s",
          rel.relation_name.c_str());
      return RC::INVALID_ARGUMENT;
    }

    switch (expr->type()) {
      case ExprType::STAR: {
        const char *tbl_name = static_cast<const StarExpr *>(expr.get())->table_name();
        if (!match_table_name(tbl_name != nullptr ? std::string(tbl_name) : std::string(), rel)) {
          LOG_WARN("view insert star expression references unsupported table. ref=%s base=%s",
              tbl_name != nullptr ? tbl_name : "", rel.relation_name.c_str());
          return RC::UNIMPLEMENTED;
        }
        for (int i = 0; i < normal_num; ++i) {
          value_to_field.push_back(i);
        }
      } break;
      case ExprType::UNBOUND_FIELD: {
        auto *uf = static_cast<UnboundFieldExpr *>(expr.get());
        const char *tbl_name = uf->table_name();
        if (!match_table_name(tbl_name != nullptr ? std::string(tbl_name) : std::string(), rel)) {
          LOG_WARN("view insert field references unsupported table. ref=%s base=%s",
              tbl_name != nullptr ? tbl_name : "", rel.relation_name.c_str());
          return RC::UNIMPLEMENTED;
        }
        const char *field_name = uf->field_name();
        if (field_name == nullptr || field_name[0] == '\0') {
          LOG_WARN("view insert encounters empty field name");
          return RC::INVALID_ARGUMENT;
        }
        RC rc = append_field(field_name);
        if (OB_FAIL(rc)) {
          return rc;
        }
      } break;
      default: {
        LOG_WARN("unsupported expression in view definition for insert. type=%d", static_cast<int>(expr->type()));
        return RC::UNIMPLEMENTED;
      }
    }
  }

  if (value_to_field.empty()) {
    LOG_WARN("view insert mapping is empty");
    return RC::INVALID_ARGUMENT;
  }

  return RC::SUCCESS;
}

static RC rewrite_insert_for_view(
    Db *db,
    const char *view_name,
    const InsertSqlNode &inserts,
    Table               *&base_table,
    std::vector<std::vector<Value>> &normalized_rows)
{
  if (view_name == nullptr) return RC::INVALID_ARGUMENT;

  View *view = db->find_view(view_name);
  if (view == nullptr) {
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  ParsedSqlResult parsed;
  RC parse_rc = parse(view->select_sql(), &parsed);
  if (OB_FAIL(parse_rc) || parsed.sql_nodes().empty()) {
    LOG_WARN("parse view select failed when rewriting insert. view=%s rc=%s", view->name(), strrc(parse_rc));
    return RC::SQL_SYNTAX;
  }

  ParsedSqlNode *node = parsed.sql_nodes()[0].get();
  if (node->flag != SCF_SELECT) {
    LOG_WARN("view definition is not select. view=%s", view->name());
    return RC::UNIMPLEMENTED;
  }

  SelectSqlNode &view_select = node->selection;
  if (view_select.relations.empty()) {
    LOG_WARN("view definition without from clause is unsupported for insert. view=%s", view->name());
    return RC::UNIMPLEMENTED;
  }

  const RelationSqlNode &rel = view_select.relations[0];
  Table *table = db->find_table(rel.relation_name.c_str());
  if (table == nullptr) {
    LOG_WARN("base table not found when rewriting view insert. view=%s base=%s", view->name(), rel.relation_name.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  std::vector<int> mapping;
  RC rc = build_view_insert_mapping(view_select, table->table_meta(), mapping);
  if (OB_FAIL(rc)) {
    return rc;
  }

  std::vector<Value> defaults;
  rc = build_default_row(table->table_meta(), defaults);
  if (OB_FAIL(rc)) {
    return rc;
  }

  normalized_rows.clear();
  normalized_rows.reserve(inserts.rows.size());
  for (size_t row_idx = 0; row_idx < inserts.rows.size(); ++row_idx) {
    const std::vector<Value> &src_row = inserts.rows[row_idx];
    if (src_row.size() != mapping.size()) {
      LOG_WARN("insert values mismatch view column count. view=%s row=%zu values=%zu expected=%zu",
          view->name(), row_idx, src_row.size(), mapping.size());
      return RC::SCHEMA_FIELD_MISSING;
    }

    std::vector<Value> dst_row = defaults;
    for (size_t i = 0; i < mapping.size(); ++i) {
      dst_row[mapping[i]] = src_row[i];
    }
    normalized_rows.emplace_back(std::move(dst_row));
  }

  base_table = table;
  LOG_INFO("rewrite insert into view(%s) to base table(%s)", view->name(), table->name());
  return RC::SUCCESS;
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
  vector<vector<Value>> normalized_rows;

  if (nullptr == table) {
    RC rewrite_rc = rewrite_insert_for_view(db, table_name, inserts, table, normalized_rows);
    if (OB_FAIL(rewrite_rc)) {
      LOG_WARN("failed to rewrite insert into view. db=%s, view=%s, rc=%s", db->name(), table_name, strrc(rewrite_rc));
      return rewrite_rc;
    }
  } else {
    normalized_rows = inserts.rows;
  }

  if (nullptr == table) {
    LOG_WARN("no such table or unsupported view insert. db=%s, target=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  // check the fields number for each row
  const TableMeta &table_meta = table->table_meta();
  const int        field_num  = table_meta.field_num() - table_meta.sys_field_num();
  for (size_t i = 0; i < normalized_rows.size(); i++) {
    const int value_num = static_cast<int>(normalized_rows[i].size());
    if (field_num != value_num) {
      LOG_WARN("schema mismatch at row %d. value num=%d, field num in schema=%d", (int)i, value_num, field_num);
      return RC::SCHEMA_FIELD_MISSING;
    }
  }

  // everything alright
  stmt = new InsertStmt(table, std::move(normalized_rows));
  return RC::SUCCESS;
}
