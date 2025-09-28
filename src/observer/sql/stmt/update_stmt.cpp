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
#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/view/view.h"
#include "sql/parser/parse.h"
#include "sql/stmt/filter_stmt.h"
#include "sql/expr/expression.h"
#include "sql/parser/expression_binder.h"
#include "common/value.h"

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

  // find table; 若不存在则尝试将视图改写为底层单表
  Table *table = db->find_table(table_name);
  if (table == nullptr) {
    View *view = db->find_view(table_name);
    if (view != nullptr) {
      // 解析视图定义，提取第一个关系名
      ParsedSqlResult parsed;
      RC prc = parse(view->select_sql(), &parsed);
      if (prc == RC::SUCCESS && !parsed.sql_nodes().empty()) {
        ParsedSqlNode *node = parsed.sql_nodes()[0].get();
        if (node->flag == SCF_SELECT && !node->selection.relations.empty()) {
          const auto &rel = node->selection.relations[0];
          if (!rel.relation_name.empty()) {
            table = db->find_table(rel.relation_name.c_str());
            if (table != nullptr) {
              LOG_INFO("rewrite update on view(%s) to base table(%s)", table_name, rel.relation_name.c_str());
            }
          }
        }
      }
    }
    if (table == nullptr) {
      LOG_WARN("no such table or unsupported view update. db=%s, target=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
  }

  // empty update is invalid (for compatibility)
  if (update.attribute_names.empty()) {
    LOG_WARN("no fields to update");
    return RC::INVALID_ARGUMENT;
  }

  vector<const FieldMeta *> field_metas;
  vector<Value> values;
  vector<Expression *> value_expressions;

  // handle each field-expression pair
  for (size_t i = 0; i < update.attribute_names.size(); ++i) {
    // find field meta
    const FieldMeta *field_meta = table->table_meta().field(update.attribute_names[i].c_str());
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
    RC rc = FilterStmt::create(db, table, &table_map, update.conditions.data(),
                               update.conditions.size(), filter_stmt);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create filter statement. rc=%s", strrc(rc));
      return rc;
    }
  }

  // create final stmt
  stmt = new UpdateStmt(table, field_metas, values, value_expressions, filter_stmt);
  return RC::SUCCESS;
}
