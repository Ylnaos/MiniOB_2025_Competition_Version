/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

// Created by Wangyunlai on 2022/5/22.

#include "sql/stmt/update_stmt.h"
#include <cctype>
#include <unordered_map>
#include <vector>
#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/stmt/filter_stmt.h"
#include "sql/expr/expression.h"
#include "sql/parser/expression_binder.h"
#include "sql/parser/parse.h"
#include "common/value.h"
#include "storage/view/view.h"

namespace {

struct ViewColumnMapping
{
  Table           *table      = nullptr;
  const FieldMeta *field_meta = nullptr;
};

// 返回输入字符串的小写副本
static std::string to_lower_copy(const std::string &input)
{
  std::string result = input;
  for (char &ch : result) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return result;
}

ConditionSqlNode clone_condition(const ConditionSqlNode &condition)
{
  ConditionSqlNode cloned;
  cloned.left_is_attr  = condition.left_is_attr;
  cloned.left_value    = condition.left_value;
  cloned.left_attr     = condition.left_attr;
  cloned.comp          = condition.comp;
  cloned.right_is_attr = condition.right_is_attr;
  cloned.right_attr    = condition.right_attr;
  cloned.right_value   = condition.right_value;
  if (condition.left_expr) {
    cloned.left_expr = condition.left_expr->copy();
  }
  if (condition.right_expr) {
    cloned.right_expr = condition.right_expr->copy();
  }
  return cloned;
}

RC append_filter_conditions(Expression &expr, vector<ConditionSqlNode> &conditions)
{
  if (expr.type() == ExprType::COMPARISON) {
    auto *cmp = static_cast<ComparisonExpr *>(&expr);
    ConditionSqlNode condition;
    condition.left_expr     = cmp->left()->copy();
    condition.right_expr    = cmp->right()->copy();
    condition.left_is_attr  = -1;
    condition.right_is_attr = -1;
    condition.comp          = cmp->comp();
    conditions.emplace_back(std::move(condition));
    return RC::SUCCESS;
  }

  if (expr.type() == ExprType::CONJUNCTION) {
    auto *conj = static_cast<ConjunctionExpr *>(&expr);
    if (conj->conjunction_type() != ConjunctionExpr::Type::AND) {
      return RC::UNSUPPORTED;
    }
    for (const auto &child : conj->children()) {
      RC rc = append_filter_conditions(*child, conditions);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
    return RC::SUCCESS;
  }

  return RC::UNSUPPORTED;
}

static RC collect_single_table_view_filter(Db *db, View *view, Table *target_table,
    std::string &relation_name, std::string &relation_alias, unique_ptr<Expression> &filter_expr)
{
  relation_name.clear();
  relation_alias.clear();
  filter_expr.reset();

  if (view == nullptr || target_table == nullptr) {
    return RC::SUCCESS;
  }

  ParsedSqlResult parsed;
  RC parse_rc = parse(view->select_sql(), &parsed);
  if (OB_FAIL(parse_rc) || parsed.sql_nodes().empty()) {
    LOG_WARN("failed to parse view definition for update filter. view=%s rc=%s", view->name(), strrc(parse_rc));
    return RC::SQL_SYNTAX;
  }

  ParsedSqlNode *node = parsed.sql_nodes()[0].get();
  if (node->flag != SCF_SELECT) {
    return RC::SUCCESS;
  }

  SelectSqlNode &select_node = node->selection;
  if (select_node.relations.size() != 1) {
    return RC::SUCCESS;
  }

  Table *base_table = db->find_table(select_node.relations[0].relation_name.c_str());
  if (base_table != target_table) {
    return RC::SUCCESS;
  }

  relation_name  = select_node.relations[0].relation_name;
  relation_alias = select_node.relations[0].alias;
  if (select_node.where_expr) {
    filter_expr = select_node.where_expr->copy();
  }
  return RC::SUCCESS;
}

static RC analyze_view_for_update(Db *db, View *view,
    std::unordered_map<std::string, ViewColumnMapping> &view_columns)
{
  view_columns.clear();

  if (view == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  ParsedSqlResult parsed;
  RC parse_rc = parse(view->select_sql(), &parsed);
  if (OB_FAIL(parse_rc) || parsed.sql_nodes().empty()) {
    LOG_WARN("failed to parse view definition for update. view=%s rc=%s", view->name(), strrc(parse_rc));
    return RC::SQL_SYNTAX;
  }

  ParsedSqlNode *node = parsed.sql_nodes()[0].get();
  if (node->flag != SCF_SELECT) {
    LOG_WARN("view definition is not simple select for update. view=%s", view->name());
    return RC::UNSUPPORTED;
  }

  SelectSqlNode &select_node = node->selection;

  struct RelationInfo
  {
    Table *table = nullptr;
    std::string name_lower;
    std::string alias_lower;
    std::unordered_map<std::string, const FieldMeta *> field_map;
  };

  std::vector<RelationInfo> relations;
  relations.reserve(select_node.relations.size());

  for (const RelationSqlNode &relation : select_node.relations) {
    Table *table = db->find_table(relation.relation_name.c_str());
    if (table == nullptr) {
      LOG_WARN("base table not found when analyzing view update. view=%s table=%s",
          view->name(), relation.relation_name.c_str());
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    RelationInfo info;
    info.table       = table;
    info.name_lower  = to_lower_copy(relation.relation_name);
    info.alias_lower = relation.alias.empty() ? info.name_lower : to_lower_copy(relation.alias);

    const TableMeta &meta    = table->table_meta();
    const int        sys_num = meta.sys_field_num();
    const int        visible_num = meta.visible_field_num();
    for (int idx = 0; idx < visible_num; ++idx) {
      const FieldMeta *field_meta = meta.field(idx + sys_num);
      if (field_meta != nullptr && field_meta->visible()) {
        info.field_map.emplace(to_lower_copy(field_meta->name()), field_meta);
      }
    }

    relations.emplace_back(std::move(info));
  }

  const std::vector<std::string> &view_fields = view->view_fields();
  size_t                           view_field_cursor = 0;

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

  auto find_relation_by_name = [&](const std::string &name_lower) -> const RelationInfo * {
    for (const auto &rel : relations) {
      if (name_lower == rel.name_lower || name_lower == rel.alias_lower) {
        return &rel;
      }
    }
    return nullptr;
  };

  for (const auto &expr : select_node.expressions) {
    if (!expr) {
      LOG_WARN("null expression encountered in view definition when analyzing update. view=%s",
          view->name());
      return RC::INVALID_ARGUMENT;
    }

    if (expr->type() == ExprType::UNBOUND_FIELD) {
      auto *uf = static_cast<UnboundFieldExpr *>(expr.get());

      const char *field_name = uf->field_name();
      if (field_name == nullptr || field_name[0] == '\0') {
        LOG_WARN("invalid field name in view definition for update. view=%s", view->name());
        return RC::INVALID_ARGUMENT;
      }

      std::string field_lower = to_lower_copy(field_name);
      const RelationInfo *owner_rel = nullptr;
      const FieldMeta    *field_meta = nullptr;

      if (uf->table_name() != nullptr && uf->table_name()[0] != '\0') {
        std::string tbl_lower = to_lower_copy(uf->table_name());
        owner_rel = find_relation_by_name(tbl_lower);
        if (owner_rel == nullptr) {
          LOG_WARN("referenced relation %s not found in view definition", uf->table_name());
          return RC::SCHEMA_TABLE_NOT_EXIST;
        }
        auto iter = owner_rel->field_map.find(field_lower);
        if (iter == owner_rel->field_map.end()) {
          LOG_WARN("field %s not found in relation %s when analyzing view update", field_name,
              uf->table_name());
          return RC::SCHEMA_FIELD_NOT_EXIST;
        }
        field_meta = iter->second;
      } else {
        for (const auto &rel : relations) {
          auto iter = rel.field_map.find(field_lower);
          if (iter != rel.field_map.end()) {
            if (owner_rel != nullptr) {
              LOG_WARN("ambiguous column %s in multi-table view", field_name);
              return RC::UNSUPPORTED;
            }
            owner_rel = &rel;
            field_meta = iter->second;
          }
        }
        if (owner_rel == nullptr) {
          LOG_WARN("field %s not found in any relation when analyzing view update", field_name);
          return RC::SCHEMA_FIELD_NOT_EXIST;
        }
      }

      std::string default_label;
      if (expr->alias() != nullptr && expr->alias()[0] != '\0') {
        default_label = expr->alias();
      } else if (field_meta != nullptr) {
        default_label = field_meta->name();
      }

      std::string label = consume_label(default_label);
      if (label.empty() && field_meta != nullptr) {
        label = field_meta->name();
      }
      view_columns.emplace(to_lower_copy(label), ViewColumnMapping {owner_rel->table, field_meta});
    } else if (expr->type() == ExprType::STAR) {
      auto *star = static_cast<const StarExpr *>(expr.get());
      const RelationInfo *target_rel = nullptr;
      if (star->table_name() != nullptr && star->table_name()[0] != '\0') {
        std::string tbl_lower = to_lower_copy(star->table_name());
        target_rel = find_relation_by_name(tbl_lower);
        if (target_rel == nullptr) {
          LOG_WARN("star references unknown relation %s in view update", star->table_name());
          return RC::SCHEMA_TABLE_NOT_EXIST;
        }
      } else {
        if (relations.size() != 1) {
          LOG_WARN("ambiguous star expression in multi-table view for update. view=%s", view->name());
          return RC::UNSUPPORTED;
        }
        target_rel = &relations[0];
      }

      for (const auto &entry : target_rel->field_map) {
        const FieldMeta *field_meta = entry.second;
        std::string default_label   = field_meta->name();
        std::string label           = consume_label(default_label);
        if (label.empty()) {
          label = field_meta->name();
        }
        view_columns.emplace(to_lower_copy(label), ViewColumnMapping {target_rel->table, field_meta});
      }
    } else {
      std::string default_label;
      if (expr->alias() != nullptr && expr->alias()[0] != '\0') {
        default_label = expr->alias();
      }
      std::string label = consume_label(default_label);
      if (!label.empty()) {
        view_columns.emplace(to_lower_copy(label), ViewColumnMapping {nullptr, nullptr});
      }
    }
  }

  if (view_columns.empty()) {
    LOG_WARN("view contains no updatable columns. view=%s", view->name());
    return RC::UNSUPPORTED;
  }

  return RC::SUCCESS;
}
}

UpdateStmt::UpdateStmt(Table *table, const vector<const FieldMeta *> &field_metas,
                       const vector<Value> &values, const vector<Expression *> &value_expressions,
                       FilterStmt *filter_stmt)
    : table_(table), field_metas_(field_metas), values_(values),
      value_expressions_(value_expressions), filter_stmt_(filter_stmt)
{}

UpdateStmt::~UpdateStmt()
{
  if (filter_stmt_ != nullptr) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
  for (auto expr : value_expressions_) {
    delete expr;
  }
}

RC UpdateStmt::create(Db *db, const UpdateSqlNode &update, Stmt *&stmt)
{
  const char *table_name = update.relation_name.c_str();
  if (db == nullptr || table_name == nullptr) {
    LOG_WARN("invalid argument. db=%p, table_name=%p", db, table_name);
    return RC::INVALID_ARGUMENT;
  }

  // find table
  Table *table = db->find_table(table_name);
  std::string view_alias; // 当从视图改写时，保存视图名用于别名映射
  std::unordered_map<std::string, ViewColumnMapping> view_updatable_columns;
  bool updating_view = false;
  View *target_view = nullptr;
  if (table == nullptr) {
    // 支持：UPDATE <view> ...
    View *view = db->find_view(table_name);
    if (view != nullptr) {
      target_view = view;
      RC view_rc = analyze_view_for_update(db, view, view_updatable_columns);
      if (OB_FAIL(view_rc)) {
        LOG_WARN("view not updatable for update statement. view=%s rc=%s", table_name, strrc(view_rc));
        return view_rc;
      }
      updating_view = true;
      view_alias    = table_name;
      LOG_INFO("rewrite update on view(%s) to base table via view mapping", table_name);
    }
    if (!updating_view) {
      LOG_WARN("no such table or unsupported view update. db=%s, target=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
  }

  // empty update is invalid (for compatibility)
  if (update.attribute_names.empty()) {
    LOG_WARN("no fields to update");
    return RC::INVALID_ARGUMENT;
  }

  if (updating_view) {
    Table *target_table = nullptr;
    for (const std::string &attr_name : update.attribute_names) {
      std::string lower = to_lower_copy(attr_name);
      auto        iter  = view_updatable_columns.find(lower);
      if (iter == view_updatable_columns.end()) {
        LOG_WARN("field not found in view for update. view=%s field=%s", table_name, attr_name.c_str());
        return RC::UNSUPPORTED;
      }
      Table *col_table = iter->second.table;
      if (col_table == nullptr) {
        LOG_WARN("field %s is not updatable (expression column). view=%s", attr_name.c_str(), table_name);
        return RC::UNSUPPORTED;
      }
      if (target_table == nullptr) {
        target_table = col_table;
      } else if (target_table != col_table) {
        LOG_WARN("update touches multiple base tables via view. view=%s", table_name);
        return RC::UNSUPPORTED;
      }
    }
    if (target_table == nullptr) {
      LOG_WARN("no base table resolved for view update. view=%s", table_name);
      return RC::UNSUPPORTED;
    }
    table = target_table;
    LOG_INFO("rewrite update on view(%s) to base table(%s)", table_name, table->name());
  }

  string view_relation_name;
  string view_relation_alias;
  unique_ptr<Expression> view_filter_expr;
  if (updating_view) {
    RC rc = collect_single_table_view_filter(
        db, target_view, table, view_relation_name, view_relation_alias, view_filter_expr);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  vector<const FieldMeta *> field_metas;
  vector<Value> values;
  vector<Expression *> value_expressions;

  // handle each field-expression pair
  for (size_t i = 0; i < update.attribute_names.size(); ++i) {
    // find field meta
    const FieldMeta *field_meta = nullptr;
    if (updating_view) {
      std::string attr_lower = to_lower_copy(update.attribute_names[i]);
      auto        iter       = view_updatable_columns.find(attr_lower);
      if (iter == view_updatable_columns.end()) {
        LOG_WARN("field not updatable via view. view=%s field=%s", table_name, update.attribute_names[i].c_str());
        return RC::UNSUPPORTED;
      }
      field_meta = iter->second.field_meta;
      if (field_meta == nullptr) {
        LOG_WARN("field %s is an expression column and cannot be updated via view %s",
            update.attribute_names[i].c_str(), table_name);
        return RC::UNSUPPORTED;
      }
    } else {
      field_meta = table->table_meta().field(update.attribute_names[i].c_str());
    }
    if (field_meta == nullptr) {
      LOG_WARN("no such field. field=%s.%s", table_name, update.attribute_names[i].c_str());
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }

    field_metas.push_back(field_meta);

    // prefer expression if provided
    if (i < update.value_expressions.size() && update.value_expressions[i] != nullptr) {
      Expression *expr = update.value_expressions[i];

      // try constant folding + simple type cast for literals
      Value const_val;
      if (expr->try_get_value(const_val) == RC::SUCCESS) {
        if (!const_val.is_null() && field_meta->type() != const_val.attr_type()) {
          Value casted_val;
          RC rc = Value::cast_to(const_val, field_meta->type(), casted_val);
          if (rc != RC::SUCCESS) {
            LOG_WARN("field type mismatch in constant assign. table=%s, field=%s, field type=%d, value type=%d",
                     table_name, field_meta->name(), field_meta->type(), const_val.attr_type());
            return RC::SCHEMA_FIELD_TYPE_MISMATCH;
          }
          // replace with casted constant
          delete expr;
          expr = new ValueExpr(casted_val);
        }
      }

      value_expressions.push_back(expr);
      // placeholder in values when expression is used
      values.push_back(Value());
    } else if (i < update.values.size()) {
      // legacy value list path
      Value value = update.values[i];
      if (field_meta->type() != value.attr_type()) {
        Value real_value;
        RC rc = Value::cast_to(value, field_meta->type(), real_value);
        if (rc != RC::SUCCESS) {
          LOG_WARN("field type mismatch. table=%s, field=%s, field type=%d, value type=%d",
                   table_name, field_meta->name(), field_meta->type(), value.attr_type());
          return RC::SCHEMA_FIELD_TYPE_MISMATCH;
        }
        value = real_value;
      }
      values.push_back(value);
      value_expressions.push_back(nullptr);
    } else {
      LOG_WARN("no value or expression for field. field=%s", field_meta->name());
      return RC::INVALID_ARGUMENT;
    }
  }

  // bind expressions in SET list
  {
    BinderContext binder_context;
    binder_context.add_table(table);
    // 若来自视图改写，允许在表达式中使用视图名限定列（如 view.col）
    if (!view_alias.empty()) {
      binder_context.add_alias(view_alias, table);
    }
    ExpressionBinder binder(binder_context);
    for (size_t i = 0; i < value_expressions.size(); ++i) {
      if (value_expressions[i] == nullptr) {
        continue;
      }
      std::unique_ptr<Expression> expr_guard(value_expressions[i]);
      std::vector<std::unique_ptr<Expression>> bound_list;
      RC rc = binder.bind_expression(expr_guard, bound_list);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to bind update set expression for field %s. rc=%s", field_metas[i]->name(), strrc(rc));
        return rc;
      }
      if (bound_list.size() != 1) {
        LOG_WARN("invalid bound expression size for field %s: %zu", field_metas[i]->name(), bound_list.size());
        return RC::INVALID_ARGUMENT;
      }
      value_expressions[i] = bound_list[0].release();
    }
  }

  // build filter stmt (even if WHERE is empty)
  FilterStmt *filter_stmt = nullptr;
  {
    unordered_map<string, Table *> table_map;
    table_map[table->name()] = table;
    if (!view_alias.empty()) {
      table_map[view_alias] = table;
    }
    if (!view_relation_name.empty()) {
      table_map[view_relation_name] = table;
    }
    if (!view_relation_alias.empty()) {
      table_map[view_relation_alias] = table;
    }

    vector<ConditionSqlNode> conditions;
    conditions.reserve(update.conditions.size() + (view_filter_expr ? 1 : 0));
    for (const ConditionSqlNode &condition : update.conditions) {
      conditions.emplace_back(clone_condition(condition));
    }
    if (view_filter_expr) {
      RC append_rc = append_filter_conditions(*view_filter_expr, conditions);
      if (OB_FAIL(append_rc)) {
        return append_rc;
      }
    }

    RC rc = FilterStmt::create(db, table, &table_map, conditions.data(),
                               conditions.size(), filter_stmt);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create filter statement. rc=%s", strrc(rc));
      return rc;
    }
  }

  // create final stmt
  stmt = new UpdateStmt(table, field_metas, values, value_expressions, filter_stmt);
  return RC::SUCCESS;
}
