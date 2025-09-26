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

#include "sql/expr/expression.h"

/**
 * @brief Vector distance/scoring functions: l2_distance, cosine_distance, inner_product
 */
class VectorFuncExpr : public Expression
{
public:
  enum class Func
  {
    L2,
    COSINE,
    INNER_PRODUCT
  };

  VectorFuncExpr(Func f, Expression *left, Expression *right);
  VectorFuncExpr(Func f, std::unique_ptr<Expression> left, std::unique_ptr<Expression> right);
  virtual ~VectorFuncExpr() = default;

  unique_ptr<Expression> copy() const override
  {
    return std::make_unique<VectorFuncExpr>(func_, left_->copy(), right_->copy());
  }

  ExprType type() const override { return ExprType::ARITHMETIC; }
  AttrType value_type() const override { return AttrType::FLOATS; }

  RC get_value(const Tuple &tuple, Value &value) const override;
  RC get_column(Chunk &chunk, Column &column) override;

  Func func() const { return func_; }
  unique_ptr<Expression> &left() { return left_; }
  unique_ptr<Expression> &right() { return right_; }

private:
  // parse/resolve any Value (vector or string) to vector<float>
  RC resolve_to_vector(const Value &v, std::vector<float> &out) const;
  float calc_score(const std::vector<float> &a, const std::vector<float> &b) const;

private:
  Func                   func_;
  unique_ptr<Expression> left_;
  unique_ptr<Expression> right_;
};

