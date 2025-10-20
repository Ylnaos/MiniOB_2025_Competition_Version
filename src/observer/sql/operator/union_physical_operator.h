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

#include <string>
#include <unordered_set>
#include <vector>

#include "common/sys/rc.h"
#include "sql/operator/physical_operator.h"

/**
 * @brief UNION/UNION ALL 物理算子
 * @ingroup PhysicalOperator
 * @details 顺序读取各子计划结果，必要时进行去重
 */
class UnionPhysicalOperator : public PhysicalOperator
{
public:
  explicit UnionPhysicalOperator(bool distinct);
  ~UnionPhysicalOperator() override = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::UNION_OP; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override { return current_tuple_; }
  RC     tuple_schema(TupleSchema &schema) const override;

private:
  RC switch_to_child(size_t index);
  RC build_signature(const Tuple &tuple, std::string &signature) const;

private:
  bool                           distinct_           = true;
  size_t                         current_child_idx_  = 0;
  Tuple                         *current_tuple_      = nullptr;
  Trx                           *trx_                = nullptr;
  std::vector<bool>              child_opened_;
  std::unordered_set<std::string> seen_signatures_;
};
