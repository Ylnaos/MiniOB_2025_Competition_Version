/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "sql/operator/physical_operator.h"
#include "sql/expr/expression.h"

/**
 * @brief ORDER BY 物理算子
 */
class OrderByPhysicalOperator : public PhysicalOperator
{
public:
  using OrderItem = pair<unique_ptr<Expression>, bool>; // bool: true for ASC, false for DESC

  OrderByPhysicalOperator(vector<OrderItem> &&order_by_items) : order_by_items_(std::move(order_by_items)) {}

  virtual ~OrderByPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::ORDER_BY; }
  OpType               get_op_type() const override { return OpType::ORDERBY; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override;

private:
  // 用于存储每一行及其预计算的排序键值
  struct RowWithKeys {
    ValueListTuple row;
    vector<Value>  sort_keys; // 预计算的排序键值
  };

  int compare_rows(const RowWithKeys &lhs, const RowWithKeys &rhs) const;

private:
  vector<OrderItem>       order_by_items_;
  vector<RowWithKeys>     rows_;
  size_t                  current_index_ = 0;
  bool                    opened_        = false;
};

