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

/**
 * @brief LIMIT 物理算子
 * @ingroup PhysicalOperator
 * @details 限制输出的行数
 */
class LimitPhysicalOperator : public PhysicalOperator
{
public:
  LimitPhysicalOperator(int limit) : limit_(limit), count_(0) {}

  virtual ~LimitPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::LIMIT; }
  OpType               get_op_type() const override { return OpType::LIMIT; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override;

private:
  int limit_;  // 最大返回行数,-1表示无限制
  int count_;  // 已返回的行数
};
