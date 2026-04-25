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

#include "common/lang/unordered_map.h"
#include "common/lang/vector.h"
#include "common/lang/memory.h"
#include "sql/operator/physical_operator.h"
#include "sql/expr/expression.h"
#include "sql/parser/parse.h"

/**
 * @brief Hash Join 算子
 * @ingroup PhysicalOperator
 */
class HashJoinPhysicalOperator
    : public PhysicalOperator
{
public:
  HashJoinPhysicalOperator(unique_ptr<Expression> left_expr, unique_ptr<Expression> right_expr);
  ~HashJoinPhysicalOperator() override = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::HASH_JOIN; }

  RC     open(Trx *trx) override;
  RC     next() override;
  RC     close() override;
  Tuple *current_tuple() override { return &joined_tuple_; }

  void set_predicates(vector<unique_ptr<Expression>> &&predicates) { predicates_ = std::move(predicates); }

private:
  RC     build_right_table(Trx *trx);
  RC     orient_key_exprs(const Tuple &right_tuple);
  RC     predicate_satisfied(bool &matched);
  string hash_key(const Value &value) const;

private:
  unique_ptr<Expression> left_expr_;
  unique_ptr<Expression> right_expr_;
  Expression            *probe_expr_ = nullptr;
  Expression            *build_expr_ = nullptr;

  PhysicalOperator *left_  = nullptr;
  PhysicalOperator *right_ = nullptr;
  Trx              *trx_   = nullptr;

  vector<ValueListTuple>              right_tuples_;
  unordered_map<string, vector<int>>   hash_table_;
  const vector<int>                   *matched_right_indexes_ = nullptr;
  size_t                               matched_pos_           = 0;
  Tuple                               *left_tuple_            = nullptr;
  JoinedTuple                          joined_tuple_;
  vector<unique_ptr<Expression>>        predicates_;
  bool                                 left_opened_ = false;
  bool                                 right_opened_ = false;
};
