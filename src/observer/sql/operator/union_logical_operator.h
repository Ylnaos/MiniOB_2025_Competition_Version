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

#include "sql/operator/logical_operator.h"

/**
 * @brief UNION/UNION ALL 逻辑算子
 * @ingroup LogicalOperator
 * @details 负责在逻辑计划中串联多个子查询的集合操作
 */
class UnionLogicalOperator : public LogicalOperator
{
public:
  explicit UnionLogicalOperator(bool distinct) : distinct_(distinct) {}
  ~UnionLogicalOperator() override = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::UNION; }
  bool                distinct() const { return distinct_; }

private:
  bool distinct_ = true;
};
