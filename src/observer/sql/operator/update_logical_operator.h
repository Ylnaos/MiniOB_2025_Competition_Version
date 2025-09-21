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
// Created by WangYunlai on 2022/12/26.
//

#pragma once

#include <vector>
#include <memory>

#include "sql/operator/logical_operator.h"
#include "common/value.h"
#include "sql/expr/expression.h"

/**
 * @brief 逻辑算子，用于执行更新(UPDATE)语句
 * @ingroup LogicalOperator
 */
class UpdateLogicalOperator : public LogicalOperator
{
public:
  UpdateLogicalOperator(Table *table, const vector<const FieldMeta *> &field_metas,
                       const vector<Value> &values,
                       const vector<unique_ptr<Expression>> &value_expressions);
  virtual ~UpdateLogicalOperator() = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::UPDATE; }

  Table                           *table() const { return table_; }
  const vector<const FieldMeta *> &field_metas() const { return field_metas_; }
  const vector<Value>             &values() const { return values_; }
  const vector<unique_ptr<Expression>> &value_expressions() const { return value_expressions_; }

private:
  Table                          *table_       = nullptr;
  vector<const FieldMeta *>      field_metas_;
  vector<Value>                  values_;
  vector<unique_ptr<Expression>> value_expressions_;
};