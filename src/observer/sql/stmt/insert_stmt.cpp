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
#include <unordered_map>
#include "common/lang/string.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/parser/parse.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/view/view.h"

namespace {
static std::string to_lower_copy(const std::string &input)
{
  std::string tmp = input;
  common::str_to_lower(tmp);
  return tmp;
}

struct FieldKey
{
  size_t relation_index = 0;
  int    field_index    = -1;

  bool operator==(const FieldKey &other) const noexcept
  {
    return relation_index == other.relation_index && field_index == other.field_index;
  }
};

struct FieldKeyHash
{
  size_t operator()(const FieldKey &key) const noexcept
  {
    size_t h1 = std::hash<size_t>{}(key.relation_index);
    size_t h2 = std::hash<int>{}(key.field_index);
    return (h1 << 1) ^ h2;
  }
};

struct RelationInfo
{
  const RelationSqlNode              *rel_node      = nullptr;
  Table                              *table         = nullptr;
  std::string                         name_lower;
  std::string                         alias_lower;
  std::unordered_map<std::string,int> field_to_index;
  int                                 sys_field_num = 0;
};

struct ViewColumnInfo
{
  FieldKey    field;
  std::string column_name;
};

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
    def_val.set_null();
    defaults[i] = std::move(def_val);
  }

  return RC::SUCCESS;
}

static RC resolve_field_key(const std::vector<RelationInfo> &relations,
    const std::unordered_map<std::string, size_t> &relation_lookup,
    const char *table_name,
    const char *field_name,
    FieldKey &field_key)
{
  if (field_name == nullptr || field_name[0] == '\0') {
    LOG_WARN("empty field name when rewriting view insert");
    return RC::INVALID_ARGUMENT;
  }

  std::string field_lower = to_lower_copy(field_name);
  int         relation_index = -1;

  if (table_name != nullptr && table_name[0] != '\0') {
    std::string rel_lower = to_lower_copy(table_name);
    auto        iter      = relation_lookup.find(rel_lower);
    if (iter == relation_lookup.end()) {
      LOG_WARN("referenced relation not found when rewriting view insert. relation=%s", table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
    relation_index = static_cast<int>(iter->second);
  } else {
    for (size_t i = 0; i < relations.size(); ++i) {
      if (relations[i].field_to_index.find(field_lower) != relations[i].field_to_index.end()) {
        if (relation_index != -1) {
          LOG_WARN("ambiguous field reference '%s' in view definition", field_name);
          return RC::SCHEMA_FIELD_MISSING;
        }
        relation_index = static_cast<int>(i);
      }
    }
    if (relation_index == -1) {
      LOG_WARN("field '%s' not found in any relation of view insert", field_name);
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }
  }

  const RelationInfo &info = relations[relation_index];
  auto                fit  = info.field_to_index.find(field_lower);
  if (fit == info.field_to_index.end()) {
    LOG_WARN("field '%s' not found in relation %s when rewriting view insert",
        field_name,
        info.rel_node != nullptr ? info.rel_node->relation_name.c_str() : "");
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  field_key.relation_index = relation_index;
  field_key.field_index    = fit->second;
  return RC::SUCCESS;
}

static RC rewrite_insert_for_view(
    Db *db,
    const char *view_name,
    const InsertSqlNode &inserts,
    std::vector<InsertTask> &tasks)
{
  tasks.clear();
  if (view_name == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

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

  std::vector<RelationInfo> relations;
  relations.reserve(view_select.relations.size());
  std::unordered_map<std::string, size_t> relation_lookup;

  for (size_t i = 0; i < view_select.relations.size(); ++i) {
    const RelationSqlNode &rel = view_select.relations[i];
    Table *table = db->find_table(rel.relation_name.c_str());
    if (table == nullptr) {
      LOG_WARN("base table not found when rewriting view insert. view=%s base=%s",
          view->name(), rel.relation_name.c_str());
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    RelationInfo info;
    info.rel_node      = &rel;
    info.table         = table;
    info.name_lower    = to_lower_copy(rel.relation_name);
    info.alias_lower   = rel.alias.empty() ? info.name_lower : to_lower_copy(rel.alias);
    info.sys_field_num = table->table_meta().sys_field_num();

    const TableMeta &meta = table->table_meta();
    const int        normal_num = meta.visible_field_num();
    for (int idx = 0; idx < normal_num; ++idx) {
      const FieldMeta *field = meta.field(idx + info.sys_field_num);
      if (field != nullptr) {
        info.field_to_index.emplace(to_lower_copy(field->name()), idx);
      }
    }

    relations.emplace_back(std::move(info));
    relation_lookup[relations.back().name_lower]  = i;
    relation_lookup[relations.back().alias_lower] = i;
  }

  std::vector<ViewColumnInfo> view_columns;
  view_columns.reserve(view_select.expressions.size());

  std::unordered_map<std::string, size_t> column_name_index;
  const std::vector<std::string>         &view_fields = view->view_fields();
  size_t                                  view_field_cursor = 0;

  auto consume_label = [&](const std::string &default_label) -> std::string {
    std::string label;
    if (view_field_cursor < view_fields.size() && !view_fields[view_field_cursor].empty()) {
      label = view_fields[view_field_cursor];
    } else {
      label = default_label;
    }
    ++view_field_cursor;
    return label;
  };

  auto resolve_condition_attr = [&](const RelAttrSqlNode &attr, FieldKey &field_key) -> RC {
    const char *tbl_name   = attr.relation_name.empty() ? nullptr : attr.relation_name.c_str();
    const char *field_name = attr.attribute_name.c_str();
    return resolve_field_key(relations, relation_lookup, tbl_name, field_name, field_key);
  };

  auto resolve_expr_field = [&](const char *tbl_name, const char *field_name, FieldKey &field_key) -> RC {
    return resolve_field_key(relations, relation_lookup, tbl_name, field_name, field_key);
  };

  for (const auto &expr : view_select.expressions) {
    if (!expr) {
      LOG_WARN("null expression in view definition when rewriting insert. view=%s", view->name());
      return RC::INVALID_ARGUMENT;
    }

    if (expr->type() == ExprType::UNBOUND_FIELD) {
      auto *uf = static_cast<UnboundFieldExpr *>(expr.get());
      FieldKey field_key;
      RC rc = resolve_expr_field(uf->table_name(), uf->field_name(), field_key);
      if (OB_FAIL(rc)) {
        return rc;
      }

      std::string default_label;
      if (expr->alias() != nullptr && expr->alias()[0] != '\0') {
        default_label = expr->alias();
      } else if (uf->field_name() != nullptr) {
        default_label = uf->field_name();
      }

      std::string label = consume_label(default_label);
      if (label.empty() && uf->field_name() != nullptr) {
        label = uf->field_name();
      }

      size_t col_idx = view_columns.size();
      view_columns.push_back({field_key, label});

      if (!label.empty()) {
        std::string lower = to_lower_copy(label);
        column_name_index.emplace(lower, col_idx);
      }
    } else if (expr->type() == ExprType::STAR) {
      auto *star = static_cast<const StarExpr *>(expr.get());
      int   relation_index = -1;
      if (star->table_name() != nullptr && star->table_name()[0] != '\0') {
        std::string lower = to_lower_copy(star->table_name());
        auto        iter  = relation_lookup.find(lower);
        if (iter == relation_lookup.end()) {
          LOG_WARN("star expression references unknown table when rewriting view insert. view=%s table=%s",
              view->name(), star->table_name());
          return RC::SCHEMA_TABLE_NOT_EXIST;
        }
        relation_index = static_cast<int>(iter->second);
      } else if (relations.size() == 1) {
        relation_index = 0;
      } else {
        LOG_WARN("ambiguous star expression in view definition for insert. view=%s", view->name());
        return RC::UNIMPLEMENTED;
      }

      const RelationInfo &info = relations[relation_index];
      const TableMeta    &meta = info.table->table_meta();
      const int           normal_num = meta.visible_field_num();

      for (int idx = 0; idx < normal_num; ++idx) {
        FieldKey field_key;
        field_key.relation_index = relation_index;
        field_key.field_index    = idx;

        const FieldMeta *field = meta.field(idx + info.sys_field_num);
        std::string      default_label = field != nullptr ? field->name() : std::string();

        std::string label = consume_label(default_label);
        if (label.empty() && field != nullptr) {
          label = field->name();
        }

        size_t col_idx = view_columns.size();
        view_columns.push_back({field_key, label});

        if (!label.empty()) {
          column_name_index.emplace(to_lower_copy(label), col_idx);
        }
      }
    } else {
      LOG_WARN("unsupported expression in view definition for insert. view=%s expr_type=%d",
          view->name(), static_cast<int>(expr->type()));
      return RC::UNIMPLEMENTED;
    }
  }

  if (view_columns.empty()) {
    LOG_WARN("view insert mapping is empty. view=%s", view->name());
    return RC::INVALID_ARGUMENT;
  }

  std::vector<std::pair<FieldKey, FieldKey>> equalities;
  for (const auto &cond : view_select.conditions) {
    if (cond.comp != EQUAL_TO) {
      continue;
    }
    if (cond.left_expr || cond.right_expr) {
      continue;
    }
    if (!cond.left_is_attr || !cond.right_is_attr) {
      continue;
    }
    FieldKey left_key;
    FieldKey right_key;
    RC       rc_left = resolve_condition_attr(cond.left_attr, left_key);
    if (OB_FAIL(rc_left)) {
      return rc_left;
    }
    RC rc_right = resolve_condition_attr(cond.right_attr, right_key);
    if (OB_FAIL(rc_right)) {
      return rc_right;
    }
    equalities.emplace_back(left_key, right_key);
  }

  const size_t view_column_count = view_columns.size();
  std::vector<std::vector<Value>> normalized_view_rows;
  normalized_view_rows.reserve(inserts.rows.size());

  std::vector<Value> view_defaults(view_column_count);
  for (auto &val : view_defaults) {
    val.set_null();
  }

  if (inserts.attribute_names.empty()) {
    for (size_t row_idx = 0; row_idx < inserts.rows.size(); ++row_idx) {
      const std::vector<Value> &src_row = inserts.rows[row_idx];
      if (src_row.size() != view_column_count) {
        LOG_WARN("insert values mismatch view column count. view=%s row=%zu values=%zu expected=%zu",
            view->name(), row_idx, src_row.size(), view_column_count);
        return RC::SCHEMA_FIELD_MISSING;
      }
      normalized_view_rows.push_back(src_row);
    }
  } else {
    std::vector<int> attr_to_index;
    attr_to_index.reserve(inserts.attribute_names.size());

    for (const auto &attr_name : inserts.attribute_names) {
      std::string lower = to_lower_copy(attr_name);
      auto        iter  = column_name_index.find(lower);
      if (iter == column_name_index.end()) {
        LOG_WARN("specified column not found in view definition. view=%s column=%s",
            view->name(), attr_name.c_str());
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }
      attr_to_index.push_back(static_cast<int>(iter->second));
    }

    for (size_t row_idx = 0; row_idx < inserts.rows.size(); ++row_idx) {
      const std::vector<Value> &src_row = inserts.rows[row_idx];
      if (src_row.size() != attr_to_index.size()) {
        LOG_WARN("value count mismatch with specified columns at row %zu. values=%zu columns=%zu",
            row_idx, src_row.size(), attr_to_index.size());
        return RC::SCHEMA_FIELD_MISSING;
      }

      std::vector<Value> view_row = view_defaults;
      for (size_t i = 0; i < attr_to_index.size(); ++i) {
        view_row[attr_to_index[i]] = src_row[i];
      }
      normalized_view_rows.emplace_back(std::move(view_row));
    }
  }

  if (normalized_view_rows.empty()) {
    LOG_WARN("no rows to insert for view %s after normalization", view->name());
    return RC::INVALID_ARGUMENT;
  }

  std::vector<std::vector<Value>> table_defaults(relations.size());
  for (size_t i = 0; i < relations.size(); ++i) {
    RC rc = build_default_row(relations[i].table->table_meta(), table_defaults[i]);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to build default row for view insert. view=%s table=%s rc=%s",
          view->name(), relations[i].rel_node->relation_name.c_str(), strrc(rc));
      return rc;
    }
  }

  std::vector<std::vector<std::vector<Value>>> per_table_rows(relations.size());
  for (auto &rows : per_table_rows) {
    rows.reserve(normalized_view_rows.size());
  }

  for (size_t row_idx = 0; row_idx < normalized_view_rows.size(); ++row_idx) {
    const std::vector<Value> &view_row = normalized_view_rows[row_idx];
    std::unordered_map<FieldKey, Value, FieldKeyHash> assignments;
    assignments.reserve(view_columns.size());

    for (size_t col_idx = 0; col_idx < view_columns.size(); ++col_idx) {
      const ViewColumnInfo &col = view_columns[col_idx];
      const Value          &val = view_row[col_idx];

      auto result = assignments.emplace(col.field, val);
      if (!result.second) {
        if (result.first->second.compare(val) != 0) {
          LOG_WARN("conflicting values for base field when inserting into view. view=%s column=%s row=%zu",
              view->name(),
              col.column_name.c_str(),
              row_idx);
          return RC::INVALID_ARGUMENT;
        }
      }
    }

    bool updated = true;
    while (updated) {
      updated = false;
      for (const auto &eq : equalities) {
        auto it_left  = assignments.find(eq.first);
        auto it_right = assignments.find(eq.second);
        if (it_left != assignments.end() && it_right != assignments.end()) {
          if (it_left->second.compare(it_right->second) != 0) {
            LOG_WARN("conflicting equality propagation when inserting view. view=%s row=%zu",
                view->name(), row_idx);
            return RC::INVALID_ARGUMENT;
          }
        } else if (it_left != assignments.end()) {
          if (assignments.find(eq.second) == assignments.end()) {
            assignments.emplace(eq.second, it_left->second);
            updated = true;
          }
        } else if (it_right != assignments.end()) {
          if (assignments.find(eq.first) == assignments.end()) {
            assignments.emplace(eq.first, it_right->second);
            updated = true;
          }
        }
      }
    }

    for (size_t rel_idx = 0; rel_idx < relations.size(); ++rel_idx) {
      std::vector<Value> row_values = table_defaults[rel_idx];
      bool               has_value  = false;

      for (size_t field_idx = 0; field_idx < row_values.size(); ++field_idx) {
        FieldKey key {rel_idx, static_cast<int>(field_idx)};
        auto     it = assignments.find(key);
        if (it != assignments.end()) {
          row_values[field_idx] = it->second;
          has_value = true;
        }
      }

      // 只有当该基础表有实际数据需要插入时，才添加到per_table_rows中
      if (has_value) {
        per_table_rows[rel_idx].emplace_back(std::move(row_values));
      }
    }
  }

  // 只为有数据需要插入的基础表生成InsertTask
  for (size_t rel_idx = 0; rel_idx < relations.size(); ++rel_idx) {
    if (!per_table_rows[rel_idx].empty()) {
      InsertTask task;
      task.table = relations[rel_idx].table;
      task.rows  = std::move(per_table_rows[rel_idx]);
      tasks.emplace_back(std::move(task));
    }
  }

  LOG_INFO("rewrite insert into view(%s) to %zu base tables", view->name(), tasks.size());
  return RC::SUCCESS;
}
}

InsertStmt::InsertStmt(vector<InsertTask> tasks)
    : insert_tasks_(std::move(tasks))
{}

InsertStmt::InsertStmt(Table *table, vector<vector<Value>> values_rows)
{
  InsertTask task;
  task.table = table;
  task.rows = std::move(values_rows);
  insert_tasks_.push_back(std::move(task));
}

RC InsertStmt::create(Db *db, const InsertSqlNode &inserts, Stmt *&stmt)
{
  const char *target_name = inserts.relation_name.c_str();
  if (db == nullptr || target_name == nullptr || inserts.rows.empty()) {
    LOG_WARN("invalid argument. db=%p, table_name=%p, row_num=%d",
        db, target_name, static_cast<int>(inserts.rows.size()));
    return RC::INVALID_ARGUMENT;
  }

  std::vector<InsertTask> tasks;
  Table *table = db->find_table(target_name);

  if (table == nullptr) {
    RC rewrite_rc = rewrite_insert_for_view(db, target_name, inserts, tasks);
    if (OB_FAIL(rewrite_rc)) {
      LOG_WARN("failed to rewrite insert into view. db=%s, view=%s, rc=%s",
          db->name(), target_name, strrc(rewrite_rc));
      return rewrite_rc;
    }
  } else {
    std::vector<std::vector<Value>> normalized_rows = inserts.rows;

    const TableMeta &table_meta = table->table_meta();
    const int        sys_num    = table_meta.sys_field_num();
    const int        field_num  = table_meta.visible_field_num();

    if (!inserts.attribute_names.empty()) {
      const int specified_num = static_cast<int>(inserts.attribute_names.size());
      std::vector<int> field_indices;
      field_indices.reserve(specified_num);

      for (const auto &attr_name : inserts.attribute_names) {
        bool found = false;
        for (int i = 0; i < field_num; ++i) {
          const FieldMeta *field = table_meta.field(i + sys_num);
          if (field != nullptr && 0 == strcasecmp(field->name(), attr_name.c_str())) {
            field_indices.push_back(i);
            found = true;
            break;
          }
        }
        if (!found) {
          LOG_WARN("specified field not found in table. table=%s field=%s", target_name, attr_name.c_str());
          return RC::SCHEMA_FIELD_MISSING;
        }
      }

      std::vector<Value> defaults;
      RC rc = build_default_row(table_meta, defaults);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to build default row for insert with column names. table=%s rc=%s",
            target_name, strrc(rc));
        return rc;
      }

      std::vector<std::vector<Value>> full_rows;
      full_rows.reserve(normalized_rows.size());
      for (size_t row_idx = 0; row_idx < normalized_rows.size(); ++row_idx) {
        const std::vector<Value> &src_row = normalized_rows[row_idx];
        if (src_row.size() != inserts.attribute_names.size()) {
          LOG_WARN("value count mismatch with specified columns at row %zu. values=%zu columns=%zu",
              row_idx, src_row.size(), inserts.attribute_names.size());
          return RC::SCHEMA_FIELD_MISSING;
        }

        std::vector<Value> full_row = defaults;
        for (size_t i = 0; i < field_indices.size(); ++i) {
          full_row[field_indices[i]] = src_row[i];
        }
        full_rows.emplace_back(std::move(full_row));
      }
      normalized_rows = std::move(full_rows);
    }

    for (size_t row_idx = 0; row_idx < normalized_rows.size(); ++row_idx) {
      const int value_num = static_cast<int>(normalized_rows[row_idx].size());
      if (value_num != field_num) {
        LOG_WARN("schema mismatch at row %zu. value num=%d, field num in schema=%d",
            row_idx, value_num, field_num);
        return RC::SCHEMA_FIELD_MISSING;
      }
    }

    InsertTask task;
    task.table = table;
    task.rows  = std::move(normalized_rows);
    tasks.emplace_back(std::move(task));
  }

  if (tasks.empty()) {
    LOG_WARN("no insert task generated for target %s", target_name);
    return RC::INTERNAL;
  }

  for (const auto &task : tasks) {
    if (task.table == nullptr) {
      LOG_WARN("insert task without target table when handling %s", target_name);
      return RC::INTERNAL;
    }
    const TableMeta &meta = task.table->table_meta();
    const int        field_num = meta.visible_field_num();
    for (size_t row_idx = 0; row_idx < task.rows.size(); ++row_idx) {
      if (static_cast<int>(task.rows[row_idx].size()) != field_num) {
        LOG_WARN("schema mismatch for table %s at row %zu. values=%zu expected=%d",
            task.table->name(), row_idx, task.rows[row_idx].size(), field_num);
        return RC::SCHEMA_FIELD_MISSING;
      }
    }
  }

  stmt = new InsertStmt(std::move(tasks));
  return RC::SUCCESS;
}
