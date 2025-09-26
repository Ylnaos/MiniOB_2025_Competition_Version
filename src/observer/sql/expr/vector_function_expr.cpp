/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/expr/vector_function_expr.h"
#include "common/type/vector_type.h"
#include "common/value.h"
#include "common/log/log.h"
#include <cmath>
#include <limits>

VectorFuncExpr::VectorFuncExpr(Func f, Expression *left, Expression *right)
    : func_(f), left_(left), right_(right)
{}

VectorFuncExpr::VectorFuncExpr(Func f, std::unique_ptr<Expression> left, std::unique_ptr<Expression> right)
    : func_(f), left_(std::move(left)), right_(std::move(right))
{}

RC VectorFuncExpr::resolve_to_vector(const Value &v, std::vector<float> &out) const
{
  if (v.attr_type() == AttrType::VECTORS) {
    return static_cast<VectorType *>(DataType::type_instance(AttrType::VECTORS))->decode_vector(v, out);
  }
  if (v.attr_type() == AttrType::CHARS || v.attr_type() == AttrType::TEXTS) {
    Value tmp;
    RC rc = DataType::type_instance(v.attr_type())->cast_to(v, AttrType::VECTORS, tmp);
    if (OB_FAIL(rc)) return rc;
    return static_cast<VectorType *>(DataType::type_instance(AttrType::VECTORS))->decode_vector(tmp, out);
  }
  return RC::INVALID_ARGUMENT;
}

float VectorFuncExpr::calc_score(const std::vector<float> &a, const std::vector<float> &b) const
{
  if (a.size() != b.size()) return std::numeric_limits<float>::quiet_NaN();
  const size_t n = a.size();
  switch (func_) {
    case Func::L2: {
      double acc = 0.0;
      for (size_t i = 0; i < n; ++i) {
        double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        acc += d * d;
      }
      return static_cast<float>(std::sqrt(acc));
    }
    case Func::INNER_PRODUCT: {
      double acc = 0.0;
      for (size_t i = 0; i < n; ++i) acc += static_cast<double>(a[i]) * static_cast<double>(b[i]);
      return static_cast<float>(acc);
    }
    case Func::COSINE: {
      double dot = 0.0, na = 0.0, nb = 0.0;
      for (size_t i = 0; i < n; ++i) {
        double x = a[i], y = b[i];
        dot += x * y;
        na += x * x;
        nb += y * y;
      }
      double denom = std::sqrt(na) * std::sqrt(nb);
      if (denom <= 1e-20) return 1.0f; // treat as maximally distant
      double sim = dot / denom;
      return static_cast<float>(1.0 - sim);
    }
  }
  return std::numeric_limits<float>::quiet_NaN();
}

RC VectorFuncExpr::get_value(const Tuple &tuple, Value &value) const
{
  Value lv, rv;
  RC rc = left_->get_value(tuple, lv);
  if (OB_FAIL(rc)) return rc;
  rc = right_->get_value(tuple, rv);
  if (OB_FAIL(rc)) return rc;
  std::vector<float> a, b;
  rc = resolve_to_vector(lv, a);
  if (OB_FAIL(rc)) return rc;
  rc = resolve_to_vector(rv, b);
  if (OB_FAIL(rc)) return rc;
  float s = calc_score(a, b);
  value.set_float(s);
  return RC::SUCCESS;
}

RC VectorFuncExpr::get_column(Chunk &chunk, Column &column)
{
  Column lcol, rcol;
  RC rc = left_->get_column(chunk, lcol);
  if (OB_FAIL(rc)) return rc;
  rc = right_->get_column(chunk, rcol);
  if (OB_FAIL(rc)) return rc;

  // compute row by row (vectors are variable-sized blobs)
  int rows = 0;
  if (lcol.column_type() == Column::Type::CONSTANT_COLUMN && rcol.column_type() == Column::Type::CONSTANT_COLUMN) {
    rows = std::max(lcol.count(), rcol.count());
  } else if (lcol.column_type() == Column::Type::CONSTANT_COLUMN) {
    rows = rcol.count();
  } else {
    rows = lcol.count();
  }

  column.init(AttrType::FLOATS, sizeof(float), rows);
  column.set_column_type(Column::Type::NORMAL_COLUMN);
  for (int i = 0; i < rows; ++i) {
    Value lv = lcol.get_value(i);
    Value rv = rcol.get_value(i);
    std::vector<float> a, b;
    RC r1 = resolve_to_vector(lv, a);
    RC r2 = resolve_to_vector(rv, b);
    if (r1 != RC::SUCCESS || r2 != RC::SUCCESS) {
      return RC::INVALID_ARGUMENT;
    }
    float s = calc_score(a, b);
    Value out(static_cast<float>(s));
    column.append_value(out);
  }
  column.set_count(rows);
  return RC::SUCCESS;
}
