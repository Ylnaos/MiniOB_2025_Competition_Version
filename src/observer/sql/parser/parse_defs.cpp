/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/parser/parse_defs.h"
#include "sql/expr/expression.h"

namespace {

static void copy_select_sql_node(const SelectSqlNode &src, SelectSqlNode &dst);

static unique_ptr<Expression> copy_expression_impl(const unique_ptr<Expression> &expr)
{
  if (!expr) {
    return nullptr;
  }

  unique_ptr<Expression> copied = expr->copy();
  if (expr->name() != nullptr && expr->name()[0] != '\0') {
    copied->set_name(expr->name());
  }
  if (expr->alias() != nullptr && expr->alias()[0] != '\0') {
    copied->set_alias(expr->alias());
  }
  return copied;
}

static ConditionSqlNode copy_condition_sql_node(const ConditionSqlNode &src)
{
  ConditionSqlNode dst;
  dst.left_is_attr  = src.left_is_attr;
  dst.right_is_attr = src.right_is_attr;
  dst.left_value    = src.left_value;
  dst.right_value   = src.right_value;
  dst.left_attr     = src.left_attr;
  dst.right_attr    = src.right_attr;
  dst.comp          = src.comp;
  dst.left_expr     = copy_expression_impl(src.left_expr);
  dst.right_expr    = copy_expression_impl(src.right_expr);
  return dst;
}

static OrderBySqlNode copy_order_by_sql_node(const OrderBySqlNode &src)
{
  OrderBySqlNode dst;
  dst.asc        = src.asc;
  dst.expression = copy_expression_impl(src.expression);
  return dst;
}

static void copy_select_sql_node(const SelectSqlNode &src, SelectSqlNode &dst)
{
  for (const auto &expr : src.expressions) {
    dst.expressions.emplace_back(copy_expression_impl(expr));
  }

  dst.relations = src.relations;

  dst.conditions.reserve(src.conditions.size());
  for (const auto &condition : src.conditions) {
    dst.conditions.emplace_back(copy_condition_sql_node(condition));
  }

  dst.where_expr = copy_expression_impl(src.where_expr);

  for (const auto &expr : src.group_by) {
    dst.group_by.emplace_back(copy_expression_impl(expr));
  }

  dst.having.reserve(src.having.size());
  for (const auto &condition : src.having) {
    dst.having.emplace_back(copy_condition_sql_node(condition));
  }

  dst.order_by.reserve(src.order_by.size());
  for (const auto &item : src.order_by) {
    dst.order_by.emplace_back(copy_order_by_sql_node(item));
  }

  dst.limit = src.limit;

  dst.set_operations.reserve(src.set_operations.size());
  for (const auto &set_op : src.set_operations) {
    SelectSetOperationSqlNode copied_set_op;
    copied_set_op.type = set_op.type;
    if (set_op.select) {
      copied_set_op.select = make_unique<SelectSqlNode>();
      copy_select_sql_node(*set_op.select, *copied_set_op.select);
    }
    dst.set_operations.emplace_back(std::move(copied_set_op));
  }
}

} // namespace

// ConditionSqlNode 的析构函数定义
ConditionSqlNode::~ConditionSqlNode() = default;

// ConditionSqlNode 的移动构造函数定义
ConditionSqlNode::ConditionSqlNode(ConditionSqlNode&& other) noexcept
  : left_is_attr(other.left_is_attr),
    left_value(std::move(other.left_value)),
    left_attr(std::move(other.left_attr)),
    comp(other.comp),
    right_is_attr(other.right_is_attr),
    right_attr(std::move(other.right_attr)),
    right_value(std::move(other.right_value)),
    left_expr(std::move(other.left_expr)),
    right_expr(std::move(other.right_expr))
{}

// ConditionSqlNode 的移动赋值运算符定义
ConditionSqlNode& ConditionSqlNode::operator=(ConditionSqlNode&& other) noexcept
{
  if (this != &other) {
    left_is_attr = other.left_is_attr;
    left_value = std::move(other.left_value);
    left_attr = std::move(other.left_attr);
    comp = other.comp;
    right_is_attr = other.right_is_attr;
    right_attr = std::move(other.right_attr);
    right_value = std::move(other.right_value);
    left_expr = std::move(other.left_expr);
    right_expr = std::move(other.right_expr);
  }
  return *this;
}

// SelectSqlNode 的析构函数定义
SelectSqlNode::~SelectSqlNode() = default;

// CalcSqlNode 的析构函数定义
CalcSqlNode::~CalcSqlNode() = default;

unique_ptr<Expression> copy_expression_with_metadata(const unique_ptr<Expression> &expr)
{
  return copy_expression_impl(expr);
}

unique_ptr<ParsedSqlNode> copy_parsed_sql_node(const ParsedSqlNode &node)
{
  auto copied = make_unique<ParsedSqlNode>(node.flag);
  switch (node.flag) {
    case SCF_SELECT:
      copy_select_sql_node(node.selection, copied->selection);
      break;
    default:
      break;
  }
  return copied;
}
