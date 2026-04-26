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