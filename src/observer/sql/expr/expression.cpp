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
// Created by Wangyunlai on 2022/07/05.
//

#include "sql/expr/expression.h"
#include "common/type/attr_type.h"
#include "sql/expr/tuple.h"
#include "sql/expr/expression_iterator.h"
#include "sql/expr/arithmetic_operator.hpp"
#include "event/sql_debug.h"
#include "sql/parser/parse_defs.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/stmt.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "session/session.h"
#include <cmath>

using namespace std;

RC FieldExpr::get_value(const Tuple &tuple, Value &value) const
{
  return tuple.find_cell(TupleCellSpec(table_name(), field_name()), value);
}

bool FieldExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != ExprType::FIELD) {
    return false;
  }
  const auto &other_field_expr = static_cast<const FieldExpr &>(other);
  return table_name() == other_field_expr.table_name() && field_name() == other_field_expr.field_name();
}

// TODO: 鍦ㄨ繘琛岃〃杈惧紡璁＄畻鏃讹紝`chunk` 鍖呭惈浜嗘墍鏈夊垪锛屽洜姝ゅ彲浠ラ€氳繃 `field_id` 鑾峰彇鍒板搴斿垪銆?// 鍚庣画鍙互浼樺寲鎴愬湪 `FieldExpr` 涓瓨鍌?`chunk` 涓煇鍒楃殑浣嶇疆淇℃伅銆?
RC FieldExpr::get_column(Chunk &chunk, Column &column)
{
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
  } else {
    column.reference(chunk.column(field().meta()->field_id()));
  }
  return RC::SUCCESS;
}

bool ValueExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != ExprType::VALUE) {
    return false;
  }
  const auto &other_value_expr = static_cast<const ValueExpr &>(other);
  return value_.compare(other_value_expr.get_value()) == 0;
}

RC ValueExpr::get_value(const Tuple &tuple, Value &value) const
{
  value = value_;
  return RC::SUCCESS;
}

RC ValueExpr::get_column(Chunk &chunk, Column &column)
{
  column.init(value_, chunk.rows());
  return RC::SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////
CastExpr::CastExpr(unique_ptr<Expression> child, AttrType cast_type) : child_(std::move(child)), cast_type_(cast_type)
{}

CastExpr::~CastExpr() {}

RC CastExpr::cast(const Value &value, Value &cast_value) const
{
  RC rc = RC::SUCCESS;
  if (this->value_type() == value.attr_type()) {
    cast_value = value;
    return rc;
  }
  rc = Value::cast_to(value, cast_type_, cast_value);
  return rc;
}

RC CastExpr::get_value(const Tuple &tuple, Value &result) const
{
  Value value;
  RC rc = child_->get_value(tuple, value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(value, result);
}

RC CastExpr::get_column(Chunk &chunk, Column &column)
{
  Column child_column;
  RC rc = child_->get_column(chunk, child_column);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  column.init(cast_type_, child_column.attr_len());
  for (int i = 0; i < child_column.count(); ++i) {
    Value value = child_column.get_value(i);
    Value cast_value;
    rc = cast(value, cast_value);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    column.append_value(cast_value);
  }
  return rc;
}

RC CastExpr::try_get_value(Value &result) const
{
  Value value;
  RC rc = child_->try_get_value(value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(value, result);
}

////////////////////////////////////////////////////////////////////////////////

ComparisonExpr::ComparisonExpr(CompOp comp, unique_ptr<Expression> left, unique_ptr<Expression> right)
    : comp_(comp), left_(std::move(left)), right_(std::move(right))
{
}

ComparisonExpr::~ComparisonExpr() {}

RC ComparisonExpr::compare_value(const Value &left, const Value &right, bool &result) const
{
  RC  rc         = RC::SUCCESS;
  result         = false;

  // IS NULL / IS NOT NULL: 仅依据左值是否为 NULL 判断
  if (comp_ == IS_NULL) {
    result = left.is_null();
    return RC::SUCCESS;
  }
  if (comp_ == IS_NOT_NULL) {
    result = !left.is_null();
    return RC::SUCCESS;
  }

  // 澶勭悊NULL鍊兼瘮杈冿細NULL涓庝换浣曞€兼瘮杈冮兘杩斿洖false锛堝寘鎷琋ULL = NULL锛?
if (left.is_null() || right.is_null()) {
    result = false;
    return RC::SUCCESS;
  }

  if (comp_ == LIKE_OP) {
    // LIKE 浠呮敮鎸佸瓧绗︿覆绫诲瀷锛堝寘鍚?CHAR/TEXT锛?
if (!is_string_type(left.attr_type()) || !is_string_type(right.attr_type())) {
      LOG_WARN("LIKE operator only supports string type");
      return RC::INVALID_ARGUMENT;
    }
    result = like_match(left.get_string().c_str(), right.get_string().c_str());
    return RC::SUCCESS;
  }

  int cmp_result = left.compare(right);
  switch (comp_) {
    case EQUAL_TO: {
      result = (0 == cmp_result);
    } break;
    case LESS_EQUAL: {
      result = (cmp_result <= 0);
    } break;
    case NOT_EQUAL: {
      result = (cmp_result != 0);
    } break;
    case LESS_THAN: {
      result = (cmp_result < 0);
    } break;
    case GREAT_EQUAL: {
      result = (cmp_result >= 0);
    } break;
    case GREAT_THAN: {
      result = (cmp_result > 0);
    } break;
    default: {
      LOG_WARN("unsupported comparison. %d", comp_);
      rc = RC::INTERNAL;
    } break;
  }

  return rc;
}

bool ComparisonExpr::like_match(const char *text, const char *pattern) const
{
  // Handle NULL or empty cases
  if (!text || !pattern) {
    return false;
  }

  const char *t = text;
  const char *p = pattern;

  while (*p) {
    if (*p == '%') {
      // Skip consecutive % characters
      while (*p == '%') {
        p++;
      }
      // If % is at the end, match everything
      if (!*p) {
        return true;
      }
      // Try to match the rest of the pattern with any suffix of the text
      while (*t) {
        if (like_match(t, p)) {
          return true;
        }
        // Single quotes cannot be matched by wildcards
        if (*t == '\'') {
          return false;
        }
        t++;
      }
      return like_match(t, p); // Handle case where text is exhausted
    } else if (*p == '_') {
      // _ matches any single character except single quote
      if (!*t || *t == '\'') {
        return false;  // No character to match or single quote
      }
      p++;
      t++;
    } else {
      // Regular character must match exactly
      if (!*t || *p != *t) {
        return false;
      }
      p++;
      t++;
    }
  }

  // Pattern exhausted, text should also be exhausted
  return !*t;
}

RC ComparisonExpr::try_get_value(Value &cell) const
{
  if (left_->type() == ExprType::VALUE && right_->type() == ExprType::VALUE) {
    ValueExpr *  left_value_expr  = static_cast<ValueExpr *>(left_.get());
    ValueExpr *  right_value_expr = static_cast<ValueExpr *>(right_.get());
    const Value &left_cell        = left_value_expr->get_value();
    const Value &right_cell       = right_value_expr->get_value();

    bool value = false;
    RC   rc    = compare_value(left_cell, right_cell, value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
    } else {
      cell.set_boolean(value);
    }
    return rc;
  }

  return RC::INVALID_ARGUMENT;
}

RC ComparisonExpr::get_value(const Tuple &tuple, Value &value) const
{
  // IS NULL / IS NOT NULL：无需读取右操作数
  if (comp_ == IS_NULL || comp_ == IS_NOT_NULL) {
    Value left_value;
    RC rc = left_->get_value(tuple, left_value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
      return rc;
    }
    bool bool_value = (comp_ == IS_NULL) ? left_value.is_null() : !left_value.is_null();
    value.set_boolean(bool_value);
    return RC::SUCCESS;
  }

  // 鐗瑰寲澶勭悊锛氬綋涓€渚т负瀛愭煡璇㈡椂锛屾敮鎸侊細
  // 1) 鏍囬噺瀛愭煡璇紙0鎴?琛岋級鐩存帴姣旇緝锛?  // 2) 澶氳鍗曞垪 + EQUAL/NOT_EQUAL锛氭寜 IN/NOT IN 璇箟姣旇緝锛?  // 鍏跺畠姣旇緝绗﹀彿 + 澶氳锛氭姤閿欍€?
if (left_->type() == ExprType::SUBQUERY || right_->type() == ExprType::SUBQUERY) {
    const bool left_is_subq  = left_->type() == ExprType::SUBQUERY;
    const bool right_is_subq = right_->type() == ExprType::SUBQUERY;

    RC rc = RC::SUCCESS;

    // 涓や晶鍧囦负瀛愭煡璇細鎸夋爣閲忔瘮杈冩墽琛?
if (left_is_subq && right_is_subq) {
      auto *lsubq = static_cast<SubqueryExpr *>(left_.get());
      auto *rsubq = static_cast<SubqueryExpr *>(right_.get());

      rc = lsubq->execute_with_context(&tuple);
      if (OB_FAIL(rc)) return rc;
      rc = rsubq->execute_with_context(&tuple);
      if (OB_FAIL(rc)) return rc;

      const auto &lvals = lsubq->results();
      const auto &rvals = rsubq->results();

      if (lvals.empty() || rvals.empty()) {
        // 绌洪泦鍚堣涓轰笉鍙瘮杈冿紝杩斿洖 false锛堢畝鍖栫殑 NULL 姣旇緝琛屼负锛?
value.set_boolean(false);
        return RC::SUCCESS;
      }
      if (lvals.size() > 1 || rvals.size() > 1) {
        LOG_WARN("scalar subquery returned more than one row: L=%zu R=%zu", lvals.size(), rvals.size());
        sql_debug("scalar subquery returned more than one row: L=%zu R=%zu", lvals.size(), rvals.size());
        return RC::INVALID_ARGUMENT;
      }

      bool bool_value = false;
      rc              = compare_value(lvals[0], rvals[0], bool_value);
      if (OB_SUCC(rc)) {
        value.set_boolean(bool_value);
      }
      return rc;
    }

    // 鍙湁涓€渚т负瀛愭煡璇細鎸夊師閫昏緫澶勭悊
    SubqueryExpr *subq = static_cast<SubqueryExpr *>(left_is_subq ? left_.get() : right_.get());
    rc                 = subq->execute_with_context(&tuple);
    if (OB_FAIL(rc)) {
      return rc;
    }
    const auto &vals = subq->results();

    // 鑾峰彇鍙︿竴渚у€?
Value other_val;
    rc = (left_is_subq ? right_->get_value(tuple, other_val) : left_->get_value(tuple, other_val));
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to get value of non-subquery expression. rc=%s", strrc(rc));
      return rc;
    }

    // 绌洪泦鍚堬細姣旇緝缁撴灉鎭掍负 false
    if (vals.empty()) {
      value.set_boolean(false);
      return RC::SUCCESS;
    }

    // 鍗曡锛氭爣閲忔瘮杈?
if (vals.size() == 1) {
      bool bool_value = false;
      rc              = left_is_subq ? compare_value(vals[0], other_val, bool_value)
                                     : compare_value(other_val, vals[0], bool_value);
      if (OB_SUCC(rc)) {
        value.set_boolean(bool_value);
      }
      return rc;
    }

    // 澶氳锛氫笉鍏佽鐢ㄤ簬鏍囬噺姣旇緝
    LOG_WARN("scalar subquery returned more than one row: %zu", vals.size());
    sql_debug("scalar subquery returned more than one row: %zu", vals.size());
    return RC::INVALID_ARGUMENT;
  }

  // 闈炲瓙鏌ヨ璺緞锛氭寜鏍囬噺姣旇緝
  Value left_value;
  Value right_value;

  RC rc = left_->get_value(tuple, left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_value(tuple, right_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }

  bool bool_value = false;

  rc = compare_value(left_value, right_value, bool_value);
  if (rc == RC::SUCCESS) {
    value.set_boolean(bool_value);
  }
  return rc;
}

RC ComparisonExpr::eval(Chunk &chunk, vector<uint8_t> &select)
{
  RC     rc = RC::SUCCESS;
  Column left_column;
  Column right_column;

  rc = left_->get_column(chunk, left_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }

  // IS NULL / IS NOT NULL：仅基于左列的空值位进行判断
  if (comp_ == IS_NULL || comp_ == IS_NOT_NULL) {
    int rows = (left_column.column_type() == Column::Type::CONSTANT_COLUMN) ? select.size() : left_column.count();
    for (int i = 0; i < rows; ++i) {
      Value lval = left_column.get_value(i);
      bool res = (comp_ == IS_NULL) ? lval.is_null() : !lval.is_null();
      select[i] &= res ? 1 : 0;
    }
    return RC::SUCCESS;
  }

  rc = right_->get_column(chunk, right_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }

  // If either side is a NULL constant, any comparison (except IS [NOT] NULL handled above)
  // should yield UNKNOWN -> treated as false for filtering. Do not report type error.
  if (left_column.attr_type() == AttrType::NULLS || right_column.attr_type() == AttrType::NULLS) {
    int rows = 0;
    if (left_column.column_type() == Column::Type::CONSTANT_COLUMN) {
      rows = right_column.count();
    } else {
      rows = left_column.count();
    }
    for (int i = 0; i < rows; ++i) {
      select[i] &= 0; // UNKNOWN in WHERE -> filtered out
    }
    return RC::SUCCESS;
  }
  if (left_column.attr_type() != right_column.attr_type()) {
    // 支持 VECTORS 与 字符串(CHARS/TEXTS) 的比较：逐行 compare_value
    bool vec_str_ok =
        (left_column.attr_type() == AttrType::VECTORS && (right_column.attr_type() == AttrType::CHARS || right_column.attr_type() == AttrType::TEXTS)) ||
        (right_column.attr_type() == AttrType::VECTORS && (left_column.attr_type() == AttrType::CHARS || left_column.attr_type() == AttrType::TEXTS));
    if (vec_str_ok) {
      int rows = (left_column.column_type() == Column::Type::CONSTANT_COLUMN) ? right_column.count() : left_column.count();
      for (int i = 0; i < rows; ++i) {
        Value left_val  = left_column.get_value(i);
        Value right_val = right_column.get_value(i);
        bool  res       = false;
        rc              = compare_value(left_val, right_val, res);
        if (OB_FAIL(rc)) return rc;
        select[i] &= res ? 1 : 0;
      }
      return RC::SUCCESS;
    }
    LOG_WARN("cannot compare columns with different types");
    return RC::INTERNAL;
  }
  if (left_column.attr_type() == AttrType::INTS) {
    rc = compare_column<int>(left_column, right_column, select);
  } else if (left_column.attr_type() == AttrType::FLOATS) {
    rc = compare_column<float>(left_column, right_column, select);
  } else if (left_column.attr_type() == AttrType::CHARS || left_column.attr_type() == AttrType::TEXTS) {
    int rows = 0;
    if (left_column.column_type() == Column::Type::CONSTANT_COLUMN) {
      rows = right_column.count();
    } else {
      rows = left_column.count();
    }
    for (int i = 0; i < rows; ++i) {
      Value left_val = left_column.get_value(i);
      Value right_val = right_column.get_value(i);
      bool        result   = false;
      rc                   = compare_value(left_val, right_val, result);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
        return rc;
      }
      select[i] &= result ? 1 : 0;
    }

  } else if (left_column.attr_type() == AttrType::VECTORS) {
    int rows = 0;
    if (left_column.column_type() == Column::Type::CONSTANT_COLUMN) {
      rows = right_column.count();
    } else {
      rows = left_column.count();
    }
    for (int i = 0; i < rows; ++i) {
      Value left_val = left_column.get_value(i);
      Value right_val = right_column.get_value(i);
      bool        result   = false;
      rc                   = compare_value(left_val, right_val, result);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
        return rc;
      }
      select[i] &= result ? 1 : 0;
    }
  } else {
    LOG_WARN("unsupported data type %d", left_column.attr_type());
    return RC::INTERNAL;
  }
  return rc;
}

template <typename T>
RC ComparisonExpr::compare_column(const Column &left, const Column &right, vector<uint8_t> &result) const
{
  RC rc = RC::SUCCESS;

  bool left_const  = left.column_type() == Column::Type::CONSTANT_COLUMN;
  bool right_const = right.column_type() == Column::Type::CONSTANT_COLUMN;
  if (left_const && right_const) {
    compare_result<T, true, true>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  } else if (left_const && !right_const) {
    compare_result<T, true, false>((T *)left.data(), (T *)right.data(), right.count(), result, comp_);
  } else if (!left_const && right_const) {
    compare_result<T, false, true>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  } else {
    compare_result<T, false, false>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  }
  return rc;
}

////////////////////////////////////////////////////////////////////////////////
ConjunctionExpr::ConjunctionExpr(Type type, vector<unique_ptr<Expression>> &children)
    : conjunction_type_(type), children_(std::move(children))
{}

RC ConjunctionExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;
  if (children_.empty()) {
    value.set_boolean(true);
    return rc;
  }

  Value tmp_value;
  for (const unique_ptr<Expression> &expr : children_) {
    rc = expr->get_value(tuple, tmp_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value by child expression. rc=%s", strrc(rc));
      return rc;
    }
    bool bool_value = tmp_value.get_boolean();
    if ((conjunction_type_ == Type::AND && !bool_value) || (conjunction_type_ == Type::OR && bool_value)) {
      value.set_boolean(bool_value);
      return rc;
    }
  }

  bool default_value = (conjunction_type_ == Type::AND);
  value.set_boolean(default_value);
  return rc;
}

////////////////////////////////////////////////////////////////////////////////

ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type, Expression *left, Expression *right)
    : arithmetic_type_(type), left_(left), right_(right)
{}
ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type, unique_ptr<Expression> left, unique_ptr<Expression> right)
    : arithmetic_type_(type), left_(std::move(left)), right_(std::move(right))
{}

bool ArithmeticExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (type() != other.type()) {
    return false;
  }
  auto &other_arith_expr = static_cast<const ArithmeticExpr &>(other);
  // 比较左右子树时需要考虑一元运算（右子树为空）的情况
  if (arithmetic_type_ != other_arith_expr.arithmetic_type()) {
    return false;
  }
  if (!left_ || !other_arith_expr.left_) {
    return left_ == nullptr && other_arith_expr.left_ == nullptr;
  }
  bool left_eq = left_->equal(*other_arith_expr.left_);
  bool right_eq = true;
  if (right_ || other_arith_expr.right_) {
    if (!right_ || !other_arith_expr.right_) {
      right_eq = false;
    } else {
      right_eq = right_->equal(*other_arith_expr.right_);
    }
  }
  return left_eq && right_eq;
}
AttrType ArithmeticExpr::value_type() const
{
  if (!right_) {
    // 一元负号：结果类型与子表达式一致
    return left_->value_type();
  }

  // 向量运算：任一侧为 VECTORS，则结果为 VECTORS（仅对 +, -, * 支持）
  if (left_->value_type() == AttrType::VECTORS || right_->value_type() == AttrType::VECTORS) {
    if (arithmetic_type_ == Type::ADD || arithmetic_type_ == Type::SUB || arithmetic_type_ == Type::MUL) {
      return AttrType::VECTORS;
    }
  }

  // 除法：即使左右都是 INT，也返回 FLOATS，避免整数截断
  if (arithmetic_type_ == Type::DIV) {
    return AttrType::FLOATS;
  }

  // 其余运算：双 INT 产出 INT，否则 FLOAT
  if ((left_->value_type() == AttrType::INTS) && (right_->value_type() == AttrType::INTS)) {
    return AttrType::INTS;
  }

  return AttrType::FLOATS;
}

RC ArithmeticExpr::calc_value(const Value &left_value, const Value &right_value, Value &value) const
{
  RC rc = RC::SUCCESS;

  // 澶勭悊NULL鍊间紶鎾?
if (left_value.is_null() || (arithmetic_type_ != Type::NEGATIVE && right_value.is_null())) {
    value.set_null();
    return RC::SUCCESS;
  }

  // 鐗规畩澶勭悊闄ら浂锛氳繑鍥濶ULL
  if (arithmetic_type_ == Type::DIV) {
    float divisor = right_value.get_float();
    if (divisor > -0.000001 && divisor < 0.000001) {
      value.set_null();
      return RC::SUCCESS;
    }
  }

  const AttrType target_type = value_type();
  value.set_type(target_type);

  switch (arithmetic_type_) {
    case Type::ADD: {
      Value::add(left_value, right_value, value);
    } break;

    case Type::SUB: {
      Value::subtract(left_value, right_value, value);
    } break;

    case Type::MUL: {
      Value::multiply(left_value, right_value, value);
    } break;

    case Type::DIV: {
      Value::divide(left_value, right_value, value);
    } break;

    case Type::NEGATIVE: {
      Value::negative(left_value, value);
    } break;

    default: {
      rc = RC::INTERNAL;
      LOG_WARN("unsupported arithmetic type. %d", arithmetic_type_);
    } break;
  }
  return rc;
}

template <bool LEFT_CONSTANT, bool RIGHT_CONSTANT>
RC ArithmeticExpr::execute_calc(
    const Column &left, const Column &right, Column &result, Type type, AttrType attr_type) const
{
  RC rc = RC::SUCCESS;
  switch (type) {
    case Type::NEGATIVE: {
      // 一元负号，仅使用左列
      if (attr_type == AttrType::INTS) {
        unary_operator<LEFT_CONSTANT, int, NegateOperator>((int *)left.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        // 保证输入视图为 float
        std::vector<float> left_buf;
        const float *      lptr = nullptr;
        if (left.attr_type() == AttrType::FLOATS) {
          lptr = reinterpret_cast<const float *>(left.data());
        } else {
          left_buf.resize(LEFT_CONSTANT ? 1 : left.count());
          if (LEFT_CONSTANT) {
            left_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(left.data()));
          } else {
            auto src = reinterpret_cast<const int *>(left.data());
            for (int i = 0; i < left.count(); ++i) left_buf[i] = static_cast<float>(src[i]);
          }
          lptr = left_buf.data();
        }
        unary_operator<LEFT_CONSTANT, float, NegateOperator>(const_cast<float *>(lptr), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
    } break;
    case Type::ADD: {
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, AddOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        // 准备浮点视图：当输入列为 INT 时，先转成对应的 FLOAT 缓冲区
        std::vector<float> left_buf;
        std::vector<float> right_buf;
        const float *      lptr = nullptr;
        const float *      rptr = nullptr;

        if (left.attr_type() == AttrType::FLOATS) {
          lptr = reinterpret_cast<const float *>(left.data());
        } else {
          left_buf.resize(LEFT_CONSTANT ? 1 : left.count());
          if (LEFT_CONSTANT) {
            left_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(left.data()));
          } else {
            auto src = reinterpret_cast<const int *>(left.data());
            for (int i = 0; i < left.count(); ++i) left_buf[i] = static_cast<float>(src[i]);
          }
          lptr = left_buf.data();
        }

        if (right.attr_type() == AttrType::FLOATS) {
          rptr = reinterpret_cast<const float *>(right.data());
        } else {
          right_buf.resize(RIGHT_CONSTANT ? 1 : right.count());
          if (RIGHT_CONSTANT) {
            right_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(right.data()));
          } else {
            auto src = reinterpret_cast<const int *>(right.data());
            for (int i = 0; i < right.count(); ++i) right_buf[i] = static_cast<float>(src[i]);
          }
          rptr = right_buf.data();
        }

        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, AddOperator>(
            const_cast<float *>(lptr), const_cast<float *>(rptr), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
    } break;
    case Type::SUB:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, SubtractOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        std::vector<float> left_buf;
        std::vector<float> right_buf;
        const float *      lptr = nullptr;
        const float *      rptr = nullptr;
        if (left.attr_type() == AttrType::FLOATS) {
          lptr = reinterpret_cast<const float *>(left.data());
        } else {
          left_buf.resize(LEFT_CONSTANT ? 1 : left.count());
          if (LEFT_CONSTANT) {
            left_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(left.data()));
          } else {
            auto src = reinterpret_cast<const int *>(left.data());
            for (int i = 0; i < left.count(); ++i) left_buf[i] = static_cast<float>(src[i]);
          }
          lptr = left_buf.data();
        }
        if (right.attr_type() == AttrType::FLOATS) {
          rptr = reinterpret_cast<const float *>(right.data());
        } else {
          right_buf.resize(RIGHT_CONSTANT ? 1 : right.count());
          if (RIGHT_CONSTANT) {
            right_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(right.data()));
          } else {
            auto src = reinterpret_cast<const int *>(right.data());
            for (int i = 0; i < right.count(); ++i) right_buf[i] = static_cast<float>(src[i]);
          }
          rptr = right_buf.data();
        }
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, SubtractOperator>(
            const_cast<float *>(lptr), const_cast<float *>(rptr), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::MUL:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, MultiplyOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        std::vector<float> left_buf;
        std::vector<float> right_buf;
        const float *      lptr = nullptr;
        const float *      rptr = nullptr;
        if (left.attr_type() == AttrType::FLOATS) {
          lptr = reinterpret_cast<const float *>(left.data());
        } else {
          left_buf.resize(LEFT_CONSTANT ? 1 : left.count());
          if (LEFT_CONSTANT) {
            left_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(left.data()));
          } else {
            auto src = reinterpret_cast<const int *>(left.data());
            for (int i = 0; i < left.count(); ++i) left_buf[i] = static_cast<float>(src[i]);
          }
          lptr = left_buf.data();
        }
        if (right.attr_type() == AttrType::FLOATS) {
          rptr = reinterpret_cast<const float *>(right.data());
        } else {
          right_buf.resize(RIGHT_CONSTANT ? 1 : right.count());
          if (RIGHT_CONSTANT) {
            right_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(right.data()));
          } else {
            auto src = reinterpret_cast<const int *>(right.data());
            for (int i = 0; i < right.count(); ++i) right_buf[i] = static_cast<float>(src[i]);
          }
          rptr = right_buf.data();
        }
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, MultiplyOperator>(
            const_cast<float *>(lptr), const_cast<float *>(rptr), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::DIV:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, DivideOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        std::vector<float> left_buf;
        std::vector<float> right_buf;
        const float *      lptr = nullptr;
        const float *      rptr = nullptr;
        if (left.attr_type() == AttrType::FLOATS) {
          lptr = reinterpret_cast<const float *>(left.data());
        } else {
          left_buf.resize(LEFT_CONSTANT ? 1 : left.count());
          if (LEFT_CONSTANT) {
            left_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(left.data()));
          } else {
            auto src = reinterpret_cast<const int *>(left.data());
            for (int i = 0; i < left.count(); ++i) left_buf[i] = static_cast<float>(src[i]);
          }
          lptr = left_buf.data();
        }
        if (right.attr_type() == AttrType::FLOATS) {
          rptr = reinterpret_cast<const float *>(right.data());
        } else {
          right_buf.resize(RIGHT_CONSTANT ? 1 : right.count());
          if (RIGHT_CONSTANT) {
            right_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(right.data()));
          } else {
            auto src = reinterpret_cast<const int *>(right.data());
            for (int i = 0; i < right.count(); ++i) right_buf[i] = static_cast<float>(src[i]);
          }
          rptr = right_buf.data();
        }
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, DivideOperator>(
            const_cast<float *>(lptr), const_cast<float *>(rptr), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    default: rc = RC::UNIMPLEMENTED; break;
  }
  return rc;
}

RC ArithmeticExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  rc = left_->get_value(tuple, left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }

  if (right_) {
    rc = right_->get_value(tuple, right_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  return calc_value(left_value, right_value, value);
}

RC ArithmeticExpr::get_column(Chunk &chunk, Column &column)
{
  RC rc = RC::SUCCESS;
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
    return rc;
  }
  Column left_column;
  Column right_column;

  rc = left_->get_column(chunk, left_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get column of left expression. rc=%s", strrc(rc));
    return rc;
  }

  if (right_) {
    rc = right_->get_column(chunk, right_column);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get column of right expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  return calc_column(left_column, right_column, column);
}

RC ArithmeticExpr::calc_column(const Column &left_column, const Column &right_column, Column &column) const
{
  RC rc = RC::SUCCESS;

  const AttrType target_type = value_type();

  // 特殊处理向量：逐行计算
  if (target_type == AttrType::VECTORS) {
    int attr_len = left_column.attr_type() == AttrType::VECTORS ? left_column.attr_len()
                    : (right_column.attr_type() == AttrType::VECTORS ? right_column.attr_len() : 0);
    const int rows = std::max(left_column.count(), right_column.count());
    column.init(AttrType::VECTORS, attr_len, rows);
    column.set_column_type(Column::Type::NORMAL_COLUMN);
    for (int i = 0; i < rows; ++i) {
      Value lv = left_column.get_value(left_column.column_type() == Column::Type::CONSTANT_COLUMN ? 0 : i);
      Value rv;
      if (right_) {
        rv = right_column.get_value(right_column.column_type() == Column::Type::CONSTANT_COLUMN ? 0 : i);
      }
      Value out;
      rc = calc_value(lv, rv, out);
      if (OB_FAIL(rc)) return rc;
      column.append_value(out);
    }
    column.set_count(rows);
    return RC::SUCCESS;
  }

  if (arithmetic_type_ == Type::NEGATIVE) {
    const bool left_const = left_column.column_type() == Column::Type::CONSTANT_COLUMN;
    column.init(target_type, left_column.attr_len(), left_column.count());
    column.set_column_type(left_const ? Column::Type::CONSTANT_COLUMN : Column::Type::NORMAL_COLUMN);
    if (left_const) {
      rc = execute_calc<true, false>(left_column, right_column, column, arithmetic_type_, target_type);
    } else {
      rc = execute_calc<false, false>(left_column, right_column, column, arithmetic_type_, target_type);
    }
    // 结果行数与左列一致
    column.set_count(left_column.count());
  } else {
    const bool left_const  = left_column.column_type() == Column::Type::CONSTANT_COLUMN;
    const bool right_const = right_column.column_type() == Column::Type::CONSTANT_COLUMN;
    const int  rows        = std::max(left_column.count(), right_column.count());
    column.init(target_type, left_column.attr_len(), rows);
    if (left_const && right_const) {
      column.set_column_type(Column::Type::CONSTANT_COLUMN);
      rc = execute_calc<true, true>(left_column, right_column, column, arithmetic_type_, target_type);
    } else if (left_const && !right_const) {
      column.set_column_type(Column::Type::NORMAL_COLUMN);
      rc = execute_calc<true, false>(left_column, right_column, column, arithmetic_type_, target_type);
    } else if (!left_const && right_const) {
      column.set_column_type(Column::Type::NORMAL_COLUMN);
      rc = execute_calc<false, true>(left_column, right_column, column, arithmetic_type_, target_type);
    } else {
      column.set_column_type(Column::Type::NORMAL_COLUMN);
      rc = execute_calc<false, false>(left_column, right_column, column, arithmetic_type_, target_type);
    }
    // 设置结果行数
    if (left_const && !right_const) {
      column.set_count(right_column.count());
    } else {
      column.set_count(left_column.count());
    }
  }
  return rc;
}

RC ArithmeticExpr::try_get_value(Value &value) const
{
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  rc = left_->try_get_value(left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }

  if (right_) {
    rc = right_->try_get_value(right_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  return calc_value(left_value, right_value, value);
}

////////////////////////////////////////////////////////////////////////////////

UnboundAggregateExpr::UnboundAggregateExpr(const char *aggregate_name, Expression *child)
    : aggregate_name_(aggregate_name), child_(child)
{}

UnboundAggregateExpr::UnboundAggregateExpr(const char *aggregate_name, unique_ptr<Expression> child)
    : aggregate_name_(aggregate_name), child_(std::move(child))
{}

////////////////////////////////////////////////////////////////////////////////
AggregateExpr::AggregateExpr(Type type, Expression *child) : aggregate_type_(type), child_(child) {}

AggregateExpr::AggregateExpr(Type type, unique_ptr<Expression> child) : aggregate_type_(type), child_(std::move(child))
{}

RC AggregateExpr::get_column(Chunk &chunk, Column &column)
{
  RC rc = RC::SUCCESS;
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
  } else {
    rc = RC::INTERNAL;
  }
  return rc;
}

bool AggregateExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != type()) {
    return false;
  }
  const AggregateExpr &other_aggr_expr = static_cast<const AggregateExpr &>(other);
  return aggregate_type_ == other_aggr_expr.aggregate_type() && child_->equal(*other_aggr_expr.child());
}

unique_ptr<Aggregator> AggregateExpr::create_aggregator() const
{
  unique_ptr<Aggregator> aggregator;
  switch (aggregate_type_) {
    case Type::SUM: {
      aggregator = make_unique<SumAggregator>();
      break;
    }
    case Type::COUNT: {
      aggregator = make_unique<CountAggregator>();
      break;
    }
    case Type::AVG: {
      aggregator = make_unique<AvgAggregator>();
      break;
    }
    case Type::MAX: {
      aggregator = make_unique<MaxAggregator>();
      break;
    }
    case Type::MIN: {
      aggregator = make_unique<MinAggregator>();
      break;
    }
    default: {
      ASSERT(false, "unsupported aggregate type");
      break;
    }
  }
  return aggregator;
}

RC AggregateExpr::get_value(const Tuple &tuple, Value &value) const
{
  return tuple.find_cell(TupleCellSpec(name()), value);
}

RC AggregateExpr::type_from_string(const char *type_str, AggregateExpr::Type &type)
{
  RC rc = RC::SUCCESS;
  if (0 == strcasecmp(type_str, "count")) {
    type = Type::COUNT;
  } else if (0 == strcasecmp(type_str, "sum")) {
    type = Type::SUM;
  } else if (0 == strcasecmp(type_str, "avg")) {
    type = Type::AVG;
  } else if (0 == strcasecmp(type_str, "max")) {
    type = Type::MAX;
  } else if (0 == strcasecmp(type_str, "min")) {
    type = Type::MIN;
  } else {
    rc = RC::INVALID_ARGUMENT;
  }
  return rc;
}

////////////////////////////////////////////////////////////////////////////////
// FunctionExpr

static inline float fx_round2(float x)
{
  float y = std::round(x * 100.0f) / 100.0f;
  if (std::fabs(y) < 0.0005f) y = 0.0f;
  return y;
}

static RC fx_get_vec(const Value &v, std::vector<float> &out)
{
  out.clear();
  if (v.attr_type() == AttrType::VECTORS) {
    const int len = v.length();
    if (len % static_cast<int>(sizeof(float)) != 0) return RC::INVALID_ARGUMENT;
    int dim = len / static_cast<int>(sizeof(float));
    const float *ptr = reinterpret_cast<const float *>(v.data());
    out.assign(ptr, ptr + dim);
    return RC::SUCCESS;
  }
  if (v.attr_type() == AttrType::CHARS || v.attr_type() == AttrType::TEXTS) {
    std::string s = v.get_string();
    if (s.size() < 2 || s.front() != '[' || s.back() != ']') return RC::INVALID_ARGUMENT;
    std::string inner = s.substr(1, s.size()-2);
    std::stringstream ss(inner);
    std::string token;
    while (std::getline(ss, token, ',')) {
      size_t i=0,j=token.size();
      while (i<j && std::isspace(static_cast<unsigned char>(token[i]))) i++;
      while (j>i && std::isspace(static_cast<unsigned char>(token[j-1]))) j--;
      std::string t = token.substr(i,j-i);
      if (t.empty()) continue;
      try { out.push_back(fx_round2(std::stof(t))); } catch (...) { return RC::INVALID_ARGUMENT; }
    }
    return RC::SUCCESS;
  }
  return RC::INVALID_ARGUMENT;
}

RC FunctionExpr::get_value(const Tuple &tuple, Value &value) const
{
  Value lv, rv;
  RC rc = left_->get_value(tuple, lv);
  if (OB_FAIL(rc)) return rc;
  rc = right_->get_value(tuple, rv);
  if (OB_FAIL(rc)) return rc;

  std::vector<float> a, b;
  rc = fx_get_vec(lv, a);
  if (OB_FAIL(rc)) { value.set_null(); return rc; }
  rc = fx_get_vec(rv, b);
  if (OB_FAIL(rc)) { value.set_null(); return rc; }
  if (a.size() != b.size()) { value.set_null(); return RC::INVALID_ARGUMENT; }

  float res = 0.0f;
  switch (func_type_) {
    case FuncType::L2_DISTANCE: {
      double sum = 0.0; for (size_t i=0;i<a.size();++i){ double d = a[i]-b[i]; sum += d*d; }
      res = fx_round2(static_cast<float>(std::sqrt(sum)));
    } break;
    case FuncType::COSINE_DISTANCE: {
      double dot=0.0,na=0.0,nb=0.0; for (size_t i=0;i<a.size();++i){ dot+=a[i]*b[i]; na+=a[i]*a[i]; nb+=b[i]*b[i]; }
      double denom = (std::sqrt(na) * std::sqrt(nb));
      double cosv = (denom==0.0) ? 0.0 : (dot/denom);
      res = fx_round2(static_cast<float>(1.0 - cosv));
    } break;
    case FuncType::INNER_PRODUCT: {
      double sum=0.0; for (size_t i=0;i<a.size();++i) sum += a[i]*b[i];
      res = fx_round2(static_cast<float>(sum));
    } break;
  }
  value.set_float(res);
  return RC::SUCCESS;
}

RC FunctionExpr::get_column(Chunk &chunk, Column &column)
{
  Column lc, rc;
  RC r = left_->get_column(chunk, lc);
  if (OB_FAIL(r)) return r;
  r = right_->get_column(chunk, rc);
  if (OB_FAIL(r)) return r;
  const int rows = std::max(lc.count(), rc.count());
  column.init(AttrType::FLOATS, sizeof(float), rows);
  column.set_column_type(Column::Type::NORMAL_COLUMN);
  for (int i = 0; i < rows; ++i) {
    Value lv = lc.get_value(lc.column_type() == Column::Type::CONSTANT_COLUMN ? 0 : i);
    Value rv = rc.get_value(rc.column_type() == Column::Type::CONSTANT_COLUMN ? 0 : i);
    std::vector<float> a, b;
    RC tr = fx_get_vec(lv, a);
    Value out;
    if (OB_FAIL(tr)) { out.set_null(); }
    else {
      tr = fx_get_vec(rv, b);
      if (OB_FAIL(tr) || a.size() != b.size()) { out.set_null(); }
      else {
        float res = 0.0f;
        switch (func_type_) {
          case FuncType::L2_DISTANCE: { double sum=0.0; for (size_t k=0;k<a.size();++k){double d=a[k]-b[k]; sum+=d*d;} res = fx_round2(static_cast<float>(std::sqrt(sum))); } break;
          case FuncType::COSINE_DISTANCE: { double dot=0.0,na=0.0,nb=0.0; for (size_t k=0;k<a.size();++k){ dot+=a[k]*b[k]; na+=a[k]*a[k]; nb+=b[k]*b[k]; } double denom=(std::sqrt(na)*std::sqrt(nb)); double cosv=(denom==0.0)?0.0:(dot/denom); res=fx_round2(static_cast<float>(1.0-cosv)); } break;
          case FuncType::INNER_PRODUCT: { double sum=0.0; for (size_t k=0;k<a.size();++k) sum+=a[k]*b[k]; res = fx_round2(static_cast<float>(sum)); } break;
        }
        out.set_float(res);
      }
    }
    column.append_value(out);
  }
  column.set_count(rows);
  return RC::SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// SubqueryExpr

SubqueryExpr::SubqueryExpr(std::unique_ptr<ParsedSqlNode> subquery_node)
    : subquery_node_(std::move(subquery_node))
{
}

SubqueryExpr::SubqueryExpr(const std::vector<Value> &cached_results, AttrType result_type, int result_len)
    : executed_(true), results_(cached_results), result_type_(result_type), result_len_(result_len)
{
}

unique_ptr<Expression> SubqueryExpr::copy() const
{
  if (executed_) {
    return make_unique<SubqueryExpr>(results_, result_type_, result_len_);
  }
  // 娣辨嫹璐濊В鏋愯妭鐐?
std::unique_ptr<ParsedSqlNode> copied;
  if (subquery_node_) {
    copied = deep_copy_parsed_node(*subquery_node_);
  }
  return make_unique<SubqueryExpr>(std::move(copied));
}

RC SubqueryExpr::execute_once() const
{
  if (executed_) {
    return RC::SUCCESS;
  }
  if (!subquery_node_ || subquery_node_->flag != SCF_SELECT) {
    LOG_WARN("subquery node invalid or not select");
    return RC::INVALID_ARGUMENT;
  }

  Session *session = Session::current_session();
  if (session == nullptr) {
    LOG_WARN("no current session to execute subquery");
    return RC::INTERNAL;
  }

  Db *db = session->get_current_db();
  if (db == nullptr) {
    LOG_WARN("no current db to execute subquery");
    return RC::INTERNAL;
  }

  // 鍒涘缓 SelectStmt锛堟敞鎰忥細蹇呴』瀵?ParsedSqlNode 鍋氭繁鎷疯礉锛岄伩鍏嶅湪缁戝畾闃舵绉诲姩/淇敼鍘?AST锛?  // 褰卞搷鍚庣画(鍙兘鐨?鍐嶆鎵ц鎴栫浉鍏冲瓙鏌ヨ鏇挎崲鏃剁殑娣辨嫹璐濓級銆?
Stmt *stmt = nullptr;
  std::unique_ptr<ParsedSqlNode> sub_node_copy = deep_copy_parsed_node(*subquery_node_);
  RC rc = Stmt::create_stmt(db, *sub_node_copy, stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create stmt for subquery. rc=%s", strrc(rc));
    return rc;
  }

  std::unique_ptr<Stmt> stmt_guard(stmt);
  auto *select_stmt = dynamic_cast<SelectStmt *>(stmt);
  if (select_stmt == nullptr) {
    LOG_WARN("subquery is not select stmt");
    return RC::INVALID_ARGUMENT;
  }

  // 鐢熸垚閫昏緫/鐗╃悊璁″垝
  std::unique_ptr<LogicalOperator> logical_oper;
  LogicalPlanGenerator              logical_gen;
  rc = logical_gen.create(select_stmt, logical_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create logical plan for subquery. rc=%s", strrc(rc));
    return rc;
  }

  std::unique_ptr<PhysicalOperator> physical_oper;
  PhysicalPlanGenerator             physical_gen;
  rc = physical_gen.create(*logical_oper, physical_oper, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create physical plan for subquery. rc=%s", strrc(rc));
    return rc;
  }

  // 鎵撳紑骞舵墽琛?
rc = physical_oper->open(session->current_trx());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open physical operator for subquery. rc=%s", strrc(rc));
    return rc;
  }

  // 鏍￠獙浠呬竴鍒楄緭鍑?
TupleSchema schema;
  rc = physical_oper->tuple_schema(schema);
  if (OB_FAIL(rc)) {
    // 鏈変簺鐗╃悊绠楀瓙鍙兘鏈疄鐜?tuple_schema锛岃繖绉嶆儏鍐典笅閫氳繃棣栬鎺ㄦ柇
    LOG_TRACE("tuple_schema not provided, will infer from first row");
  }

  bool schema_checked = false;
  if (rc == RC::SUCCESS && schema.cell_num() > 0) {
    if (schema.cell_num() != 1) {
      physical_oper->close();
      LOG_WARN("subquery must return exactly one column");
      sql_debug("subquery must return exactly one column");
      return RC::INVALID_ARGUMENT;
    }
    schema_checked = true;
  }

  results_.clear();

  while ((rc = physical_oper->next()) == RC::SUCCESS) {
    Tuple *tuple = physical_oper->current_tuple();
    if (tuple == nullptr) {
      rc = RC::INTERNAL;
      LOG_WARN("null tuple from subquery operator");
      break;
    }
    if (!schema_checked && tuple->cell_num() != 1) {
      LOG_WARN("subquery must return exactly one column");
      sql_debug("subquery must return exactly one column");
      rc = RC::INVALID_ARGUMENT;
      break;
    }

    Value cell;
    rc = tuple->cell_at(0, cell);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to get cell from subquery tuple. rc=%s", strrc(rc));
      break;
    }
    results_.push_back(cell);
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }

  physical_oper->close();

  // 璁板綍缁撴灉绫诲瀷
  if (!results_.empty()) {
    result_type_ = results_.front().attr_type();
    result_len_  = results_.front().length();
  } else {
    // 娌℃湁缁撴灉锛岄粯璁ょ被鍨嬫部鐢?UNDEFINED
    result_type_ = AttrType::UNDEFINED;
    result_len_  = -1;
  }

  executed_ = true;
  return rc;
}

RC SubqueryExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = execute_once();
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (results_.size() == 0) {
    // 鏍囬噺涓婁笅鏂囦腑锛岀┖缁撴灉褰撳仛 NULL
    value.set_null();
    return RC::SUCCESS;
  }
  if (results_.size() > 1) {
    LOG_WARN("scalar subquery returned more than one row: %zu", results_.size());
    sql_debug("scalar subquery returned more than one row: %zu", results_.size());
    return RC::INVALID_ARGUMENT;
  }
  value = results_[0];
  return RC::SUCCESS;
}

RC SubqueryExpr::execute_with_context(const Tuple *outer_tuple) const
{
  // 鏃犱笂涓嬫枃锛氭部鐢ㄦ噿鎵ц + 缂撳瓨
  if (outer_tuple == nullptr) {
    return execute_once();
  }

  if (!subquery_node_ || subquery_node_->flag != SCF_SELECT) {
    LOG_WARN("subquery node invalid or not select");
    return RC::INVALID_ARGUMENT;
  }

  // 鏋勯€犲甫甯搁噺鏇挎崲鐨勬柊 AST
  bool did_substitute = false;
  std::unique_ptr<ParsedSqlNode> copied = deep_copy_parsed_node_with_ctx(*subquery_node_, *outer_tuple, did_substitute);
  if (!copied) {
    LOG_WARN("failed to copy subquery node with ctx");
    return RC::INTERNAL;
  }

  // 鑻ヤ笉瀛樺湪鐩稿叧寮曠敤锛岃惤鍥炰竴娆℃€х紦瀛樿矾寰?
if (!did_substitute) {
    return execute_once();
  }

  // 鐩稿叧瀛愭煡璇細姣忔閲嶆柊鎵ц锛屼笉鍐欏叆 executed_ 缂撳瓨
  Session *session = Session::current_session();
  if (session == nullptr) {
    LOG_WARN("no current session to execute subquery");
    return RC::INTERNAL;
  }
  Db *db = session->get_current_db();
  if (db == nullptr) {
    LOG_WARN("no current db to execute subquery");
    return RC::INTERNAL;
  }

  Stmt *stmt = nullptr;
  RC rc = Stmt::create_stmt(db, *copied, stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create stmt for correlated subquery. rc=%s", strrc(rc));
    return rc;
  }
  std::unique_ptr<Stmt> stmt_guard(stmt);
  auto *select_stmt = dynamic_cast<SelectStmt *>(stmt);
  if (select_stmt == nullptr) {
    LOG_WARN("correlated subquery is not select stmt");
    return RC::INVALID_ARGUMENT;
  }

  std::unique_ptr<LogicalOperator> logical_oper;
  LogicalPlanGenerator              logical_gen;
  rc = logical_gen.create(select_stmt, logical_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create logical plan for correlated subquery. rc=%s", strrc(rc));
    return rc;
  }

  std::unique_ptr<PhysicalOperator> physical_oper;
  PhysicalPlanGenerator             physical_gen;
  rc = physical_gen.create(*logical_oper, physical_oper, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create physical plan for correlated subquery. rc=%s", strrc(rc));
    return rc;
  }

  rc = physical_oper->open(session->current_trx());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open physical operator for correlated subquery. rc=%s", strrc(rc));
    return rc;
  }

  // 娓呯┖骞舵敹闆嗙粨鏋?
results_.clear();
  result_type_ = AttrType::UNDEFINED;
  result_len_  = -1;

  Tuple *tuple = nullptr;
  while (RC::SUCCESS == (rc = physical_oper->next())) {
    tuple = physical_oper->current_tuple();
    if (tuple == nullptr) {
      rc = RC::INTERNAL;
      break;
    }
    Value v;
    // 璇诲彇棣栧垪
    RC rc2 = tuple->cell_at(0, v);
    if (rc2 != RC::SUCCESS) {
      rc = rc2;
      break;
    }
    results_.push_back(v);
    if (result_type_ == AttrType::UNDEFINED) {
      result_type_ = v.attr_type();
      result_len_  = v.length();
    }
  }
  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }
  physical_oper->close();
  return rc;
}

std::unique_ptr<ParsedSqlNode> SubqueryExpr::deep_copy_parsed_node(const ParsedSqlNode &node) const
{
  auto copied = std::make_unique<ParsedSqlNode>(node.flag);
  if (node.flag == SCF_SELECT) {
    // 娣辨嫹璐?SelectSqlNode
    const SelectSqlNode &src = node.selection;
    SelectSqlNode       &dst = copied->selection;

    // expressions
    for (const auto &expr_ptr : src.expressions) {
      if (expr_ptr) {
        dst.expressions.emplace_back(expr_ptr->copy());
      }
    }
    // relations
    dst.relations = src.relations;
    // conditions
    dst.conditions.reserve(src.conditions.size());
    for (const auto &cond : src.conditions) {
      ConditionSqlNode new_cond;
      new_cond.left_is_attr  = cond.left_is_attr;
      new_cond.right_is_attr = cond.right_is_attr;
      new_cond.left_value    = cond.left_value;
      new_cond.right_value   = cond.right_value;
      new_cond.left_attr     = cond.left_attr;
      new_cond.right_attr    = cond.right_attr;
      new_cond.comp          = cond.comp;

      if (cond.left_expr) {
        new_cond.left_expr.reset(cond.left_expr->copy().release());
      }
      if (cond.right_expr) {
        new_cond.right_expr.reset(cond.right_expr->copy().release());
      }
      dst.conditions.emplace_back(std::move(new_cond));
    }
    // group by
    for (const auto &grp : src.group_by) {
      if (grp) {
        dst.group_by.emplace_back(grp->copy());
      }
    }
    // order by
    for (const auto &ord : src.order_by) {
      OrderBySqlNode item;
      item.asc = ord.asc;
      if (ord.expression) {
        item.expression.reset(ord.expression->copy().release());
      }
      dst.order_by.emplace_back(std::move(item));
    }
  }
  return copied;
}

std::unique_ptr<ParsedSqlNode> SubqueryExpr::deep_copy_parsed_node_with_ctx(
    const ParsedSqlNode &node, const Tuple &outer_tuple, bool &did_substitute) const
{
  did_substitute = false;
  auto copied    = std::make_unique<ParsedSqlNode>(node.flag);
  if (node.flag != SCF_SELECT) {
    return copied;
  }
  const SelectSqlNode &src = node.selection;
  SelectSqlNode       &dst = copied->selection;

  // relations
  dst.relations = src.relations;

  // expressions (SELECT 鍒楄〃) 鍘熸牱澶嶅埗
  for (const auto &expr_ptr : src.expressions) {
    if (expr_ptr) {
      dst.expressions.emplace_back(expr_ptr->copy());
    }
  }

  // conditions锛圵HERE AND 閾撅級閫掑綊澶嶅埗骞舵浛鎹?
dst.conditions.reserve(src.conditions.size());
  for (const auto &cond : src.conditions) {
    ConditionSqlNode new_cond;
    new_cond.left_is_attr  = cond.left_is_attr;
    new_cond.right_is_attr = cond.right_is_attr;
    new_cond.left_value    = cond.left_value;
    new_cond.right_value   = cond.right_value;
    new_cond.left_attr     = cond.left_attr;
    new_cond.right_attr    = cond.right_attr;
    new_cond.comp          = cond.comp;

    RC rc = RC::SUCCESS;
    bool sub = false;
    std::vector<std::string> inner_names;
    inner_names.reserve(src.relations.size()*2);
    for (const auto &r : src.relations) {
      inner_names.push_back(r.relation_name);
      if (!r.alias.empty()) inner_names.push_back(r.alias);
    }
    if (cond.left_expr) {
      auto left_new = copy_and_substitute_outer_refs(*cond.left_expr, inner_names, outer_tuple, sub, rc);
      if (!left_new || rc != RC::SUCCESS) return nullptr;
      new_cond.left_expr.reset(left_new.release());
      did_substitute = did_substitute || sub;
    }
    sub = false;
    if (cond.right_expr) {
      auto right_new = copy_and_substitute_outer_refs(*cond.right_expr, inner_names, outer_tuple, sub, rc);
      if (!right_new || rc != RC::SUCCESS) return nullptr;
      new_cond.right_expr.reset(right_new.release());
      did_substitute = did_substitute || sub;
    }
    dst.conditions.emplace_back(std::move(new_cond));
  }

  // group by
  for (const auto &grp : src.group_by) {
    if (grp) {
      dst.group_by.emplace_back(grp->copy());
    }
  }
  // order by
  for (const auto &ord : src.order_by) {
    OrderBySqlNode item;
    item.asc = ord.asc;
    if (ord.expression) {
      item.expression.reset(ord.expression->copy().release());
    }
    dst.order_by.emplace_back(std::move(item));
  }
  return copied;
}

std::unique_ptr<Expression> SubqueryExpr::copy_and_substitute_outer_refs(
    const Expression &expr,
    const std::vector<std::string> &inner_relations,
    const Tuple &outer_tuple,
    bool &did_substitute,
    RC &rc) const
{
  rc             = RC::SUCCESS;
  did_substitute = false;

  auto is_inner_table = [&](const char *tname) -> bool {
    if (tname == nullptr || *tname == '\0') return false;
    for (const auto &r : inner_relations) {
      if (0 == strcasecmp(r.c_str(), tname)) return true;
    }
    return false;
  };

  if (expr.type() == ExprType::UNBOUND_FIELD) {
    const auto &u = static_cast<const UnboundFieldExpr &>(expr);
    const char *t = u.table_name();
    const char *f = u.field_name();
    if (t != nullptr && *t != '\0' && !is_inner_table(t)) {
      // 澶栧眰琛ㄥ瓧娈碉細浠?outer_tuple 鎶藉彇鎴愬父閲?
Value v;
      RC rc2 = outer_tuple.find_cell(TupleCellSpec(t, f), v);
      if (rc2 != RC::SUCCESS) {
        LOG_WARN("failed to fetch correlated value %s.%s from outer tuple", t, f);
        rc = rc2;
        return nullptr;
      }
      did_substitute = true;
      auto ve        = std::make_unique<ValueExpr>(v);
      ve->set_name(string(t) + "." + string(f));
      return ve;
    }
    // 鍐呭眰琛ㄥ瓧娈典繚鎸佸師鏍?
return expr.copy();
  }

  if (expr.type() == ExprType::SUBQUERY) {
    // 閫掑綊娣卞叆瀛愭煡璇紝缁х画瀵规洿鍐呭眰鐨勭浉鍏冲紩鐢ㄥ仛鏇挎崲
    const auto &sub_e = static_cast<const SubqueryExpr &>(expr);
    if (sub_e.subquery_node_ == nullptr) {
      // 宸茬紦瀛樼粨鏋滅殑瀛愭煡璇紝鐩存帴澶嶅埗
      return expr.copy();
    }
    bool inner_substituted = false;
    auto copied_node       = deep_copy_parsed_node_with_ctx(*sub_e.subquery_node_, outer_tuple, inner_substituted);
    if (!copied_node) {
      rc = RC::INTERNAL;
      return nullptr;
    }
    if (inner_substituted) {
      did_substitute = true;
    }
    auto new_sub = std::make_unique<SubqueryExpr>(std::move(copied_node));
    new_sub->set_name(expr.name());
    return new_sub;
  }

  // 鍏跺畠琛ㄨ揪寮忥細澶嶅埗鍚庨€掑綊澶勭悊瀛愯妭鐐?
auto copied = expr.copy();
  RC   tmp_rc = ExpressionIterator::iterate_child_expr(*copied, [&](std::unique_ptr<Expression> &child) -> RC {
    bool sub_flag = false;
    RC   inner_rc = RC::SUCCESS;
    auto new_ch   = copy_and_substitute_outer_refs(*child, inner_relations, outer_tuple, sub_flag, inner_rc);
    if (!new_ch || inner_rc != RC::SUCCESS) {
      return inner_rc;
    }
    if (sub_flag) did_substitute = true;
    child.reset(new_ch.release());
    return RC::SUCCESS;
  });
  if (tmp_rc != RC::SUCCESS) {
    rc = tmp_rc;
    return nullptr;
  }
  return copied;
}

////////////////////////////////////////////////////////////////////////////////
// InExpr

RC InExpr::get_value(const Tuple &tuple, Value &value) const
{
  // 姹傚乏鍊?
Value left_val;
  RC rc = test_expr_->get_value(tuple, left_val);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (left_val.is_null()) {
    // 绠€鍖栧鐞嗭細NULL 涓庨泦鍚堟瘮杈冧负 false锛堜笌鏍囧噯 SQL 鐨勪笁鍊奸€昏緫鍙兘涓嶅悓锛?
value.set_boolean(false);
    return RC::SUCCESS;
  }

  // 鍙冲€煎簲涓哄瓙鏌ヨ琛ㄨ揪寮?
if (set_expr_->type() != ExprType::SUBQUERY) {
    LOG_WARN("IN operator's right expr should be a subquery");
    return RC::INVALID_ARGUMENT;
  }
  auto *subq = static_cast<SubqueryExpr *>(set_expr_.get());
  rc = subq->execute_with_context(&tuple);
  if (OB_FAIL(rc)) {
    return rc;
  }

  bool found = false;
  const auto &vals = subq->results();
  for (const auto &rv : vals) {
    if (rv.is_null()) {
      continue; // 蹇界暐 NULL
    }
    if (left_val.compare(rv) == 0) {
      found = true;
      break;
    }
  }

  bool result = not_in_ ? !found : found;
  value.set_boolean(result);
  return RC::SUCCESS;
}
