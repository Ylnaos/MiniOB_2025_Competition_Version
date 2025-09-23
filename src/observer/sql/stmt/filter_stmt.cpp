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
// Created by Wangyunlai on 2022/5/22.
//

#include "sql/stmt/filter_stmt.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "common/sys/rc.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/parser/expression_binder.h"
#include "sql/expr/expression_iterator.h"

FilterStmt::~FilterStmt()
{
  for (FilterUnit *unit : filter_units_) {
    delete unit;
  }
  filter_units_.clear();
}

RC FilterStmt::create(Db *db, Table *default_table, unordered_map<string, Table *> *tables,
    const ConditionSqlNode *conditions, int condition_num, FilterStmt *&stmt)
{
  RC rc = RC::SUCCESS;
  stmt  = nullptr;

  FilterStmt *tmp_stmt = new FilterStmt();
  for (int i = 0; i < condition_num; i++) {
    FilterUnit *filter_unit = nullptr;

    rc = create_filter_unit(db, default_table, tables, conditions[i], filter_unit);
    if (rc != RC::SUCCESS) {
      delete tmp_stmt;
      LOG_WARN("failed to create filter unit. condition index=%d", i);
      return rc;
    }
    tmp_stmt->filter_units_.push_back(filter_unit);
  }

  stmt = tmp_stmt;
  return rc;
}

RC get_table_and_field(Db *db, Table *default_table, unordered_map<string, Table *> *tables,
    const RelAttrSqlNode &attr, Table *&table, const FieldMeta *&field)
{
  if (common::is_blank(attr.relation_name.c_str())) {
    table = default_table;
  } else if (nullptr != tables) {
    auto iter = tables->find(attr.relation_name);
    if (iter != tables->end()) {
      table = iter->second;
    }
  } else {
    table = db->find_table(attr.relation_name.c_str());
  }
  if (nullptr == table) {
    LOG_WARN("No such table: attr.relation_name: %s", attr.relation_name.c_str());
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  field = table->table_meta().field(attr.attribute_name.c_str());
  if (nullptr == field) {
    LOG_WARN("no such field in table: table %s, field %s", table->name(), attr.attribute_name.c_str());
    table = nullptr;
    return RC::SCHEMA_FIELD_NOT_EXIST;
  }

  return RC::SUCCESS;
}

// 辅助函数：递归绑定表达式中的UnboundFieldExpr
static RC bind_expression_fields(unique_ptr<Expression> &expr, Db *db, Table *default_table,
                                 unordered_map<string, Table *> *tables)
{
  if (!expr) {
    return RC::SUCCESS;
  }

  RC rc = RC::SUCCESS;

  // 如果是UnboundFieldExpr，将其转换为FieldExpr
  if (expr->type() == ExprType::UNBOUND_FIELD) {
    auto* unbound_expr = static_cast<UnboundFieldExpr*>(expr.get());

    const char *table_name = unbound_expr->table_name();
    const char *field_name = unbound_expr->field_name();

    Table *table = nullptr;
    if (table_name && strlen(table_name) > 0) {
      if (tables) {
        auto iter = tables->find(table_name);
        if (iter != tables->end()) {
          table = iter->second;
        }
      }
      if (!table) {
        table = db->find_table(table_name);
      }
    } else {
      table = default_table;
    }

    if (!table) {
      LOG_WARN("table not found: %s", table_name ? table_name : "<default>");
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    const FieldMeta *field_meta = table->table_meta().field(field_name);
    if (!field_meta) {
      LOG_WARN("field not found: %s.%s", table->name(), field_name);
      return RC::SCHEMA_FIELD_MISSING;
    }

    Field field(table, field_meta);
    auto field_expr = make_unique<FieldExpr>(field);
    field_expr->set_name(expr->name());
    expr = std::move(field_expr);
  }
  // 如果是算术表达式，递归处理子表达式
  else if (expr->type() == ExprType::ARITHMETIC) {
    auto* arith_expr = static_cast<ArithmeticExpr*>(expr.get());
    rc = bind_expression_fields(arith_expr->left(), db, default_table, tables);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    if (arith_expr->right()) {
      rc = bind_expression_fields(arith_expr->right(), db, default_table, tables);
      if (rc != RC::SUCCESS) {
        return rc;
      }
    }
  }
  // 如果是比较表达式，递归处理子表达式
  else if (expr->type() == ExprType::COMPARISON) {
    auto* comp_expr = static_cast<ComparisonExpr*>(expr.get());
    rc = bind_expression_fields(comp_expr->left(), db, default_table, tables);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    rc = bind_expression_fields(comp_expr->right(), db, default_table, tables);
    if (rc != RC::SUCCESS) {
      return rc;
    }
  }

  return RC::SUCCESS;
}

RC FilterStmt::create_filter_unit(Db *db, Table *default_table, unordered_map<string, Table *> *tables,
    const ConditionSqlNode &condition, FilterUnit *&filter_unit)
{
  RC rc = RC::SUCCESS;

  CompOp comp = condition.comp;
  if (comp < EQUAL_TO || comp >= NO_OP) {
    LOG_WARN("invalid compare operator : %d", comp);
    return RC::INVALID_ARGUMENT;
  }

  // LIKE operator only works with CHAR type fields
  if (comp == LIKE_OP) {
    // Check if at least one side is an attribute (field)
    if (!condition.left_is_attr && !condition.right_is_attr) {
      LOG_WARN("LIKE operator requires at least one field operand");
      return RC::INVALID_ARGUMENT;
    }
  }

  filter_unit = new FilterUnit;

  // 处理左侧操作数
  if (condition.left_is_attr == -1 && condition.left_expr) {
    // 使用表达式，需要递归绑定其中的UnboundFieldExpr
    auto expr_copy = condition.left_expr->copy();
    rc = bind_expression_fields(expr_copy, db, default_table, tables);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to bind expression fields in left expression");
      return rc;
    }
    FilterObj filter_obj;
    filter_obj.init_expr(std::move(expr_copy));
    filter_unit->set_left(std::move(filter_obj));
  } else if (condition.left_is_attr == 1) {
    Table           *table = nullptr;
    const FieldMeta *field = nullptr;
    rc                     = get_table_and_field(db, default_table, tables, condition.left_attr, table, field);
    if (rc != RC::SUCCESS) {
      LOG_WARN("cannot find attr");
      return rc;
    }
    FilterObj filter_obj;
    filter_obj.init_attr(Field(table, field));
    filter_unit->set_left(std::move(filter_obj));
  } else {
    FilterObj filter_obj;
    filter_obj.init_value(condition.left_value);
    filter_unit->set_left(std::move(filter_obj));
  }

  // 处理右侧操作数
  if (condition.right_is_attr == -1 && condition.right_expr) {
    // 使用表达式，需要递归绑定其中的UnboundFieldExpr
    auto expr_copy = condition.right_expr->copy();
    rc = bind_expression_fields(expr_copy, db, default_table, tables);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to bind expression fields in right expression");
      return rc;
    }
    FilterObj filter_obj;
    filter_obj.init_expr(std::move(expr_copy));
    filter_unit->set_right(std::move(filter_obj));
  } else if (condition.right_is_attr == 1) {
    Table           *table = nullptr;
    const FieldMeta *field = nullptr;
    rc                     = get_table_and_field(db, default_table, tables, condition.right_attr, table, field);
    if (rc != RC::SUCCESS) {
      LOG_WARN("cannot find attr");
      return rc;
    }
    FilterObj filter_obj;
    filter_obj.init_attr(Field(table, field));
    filter_unit->set_right(std::move(filter_obj));
  } else {
    FilterObj filter_obj;
    filter_obj.init_value(condition.right_value);
    filter_unit->set_right(std::move(filter_obj));
  }

  filter_unit->set_comp(comp);

  // 检查两个类型是否能够比较
  return rc;
}
