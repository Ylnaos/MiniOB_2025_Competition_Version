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
#include "sql/expr/arithmetic_operator.hpp"
#include "event/sql_debug.h"
#include "sql/parser/parse_defs.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/stmt.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "session/session.h"

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

// TODO: 在进行表达式计算时，`chunk` 包含了所有列，因此可以通过 `field_id` 获取到对应列。
// 后续可以优化成在 `FieldExpr` 中存储 `chunk` 中某列的位置信息。
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

  // 处理NULL值比较：NULL与任何值比较都返回false（包括NULL = NULL）
  if (left.is_null() || right.is_null()) {
    result = false;
    return RC::SUCCESS;
  }

  if (comp_ == LIKE_OP) {
    // LIKE 仅支持字符串类型（包含 CHAR/TEXT）
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
  // 特化处理：当一侧为子查询时，支持：
  // 1) 标量子查询（0或1行）直接比较；
  // 2) 多行单列 + EQUAL/NOT_EQUAL：按 IN/NOT IN 语义比较；
  // 其它比较符号 + 多行：报错。
  if (left_->type() == ExprType::SUBQUERY || right_->type() == ExprType::SUBQUERY) {
    const bool left_is_subq  = left_->type() == ExprType::SUBQUERY;
    const bool right_is_subq = right_->type() == ExprType::SUBQUERY;
    if (left_is_subq && right_is_subq) {
      LOG_WARN("comparison between two subqueries is not supported");
      return RC::INVALID_ARGUMENT;
    }

    RC rc = RC::SUCCESS;
    SubqueryExpr *subq = static_cast<SubqueryExpr *>(left_is_subq ? left_.get() : right_.get());
    rc                 = subq->execute_once();
    if (OB_FAIL(rc)) {
      return rc;
    }
    const auto &vals = subq->results();

    // 获取另一侧值
    Value other_val;
    rc = (left_is_subq ? right_->get_value(tuple, other_val) : left_->get_value(tuple, other_val));
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to get value of non-subquery expression. rc=%s", strrc(rc));
      return rc;
    }

    // 空集合：比较结果恒为 false
    if (vals.empty()) {
      value.set_boolean(false);
      return RC::SUCCESS;
    }

    // 单行：标量比较
    if (vals.size() == 1) {
      bool bool_value = false;
      rc              = left_is_subq ? compare_value(vals[0], other_val, bool_value)
                                     : compare_value(other_val, vals[0], bool_value);
      if (OB_SUCC(rc)) {
        value.set_boolean(bool_value);
      }
      return rc;
    }

    // 多行：不允许用于标量比较（题目要求：不能把多行标量子查询当成多个值比较）
    LOG_WARN("scalar subquery returned more than one row: %zu", vals.size());
    sql_debug("scalar subquery returned more than one row: %zu", vals.size());
    return RC::INVALID_ARGUMENT;
  }

  // 非子查询路径：按标量比较
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
  rc = right_->get_column(chunk, right_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }
  if (left_column.attr_type() != right_column.attr_type()) {
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
  return arithmetic_type_ == other_arith_expr.arithmetic_type() && left_->equal(*other_arith_expr.left_) &&
         right_->equal(*other_arith_expr.right_);
}
AttrType ArithmeticExpr::value_type() const
{
  if (!right_) {
    return left_->value_type();
  }

  if ((left_->value_type() == AttrType::INTS) &&
   (right_->value_type() == AttrType::INTS)) {
    return AttrType::INTS;
  }

  return AttrType::FLOATS;
}

RC ArithmeticExpr::calc_value(const Value &left_value, const Value &right_value, Value &value) const
{
  RC rc = RC::SUCCESS;

  // 处理NULL值传播
  if (left_value.is_null() || (arithmetic_type_ != Type::NEGATIVE && right_value.is_null())) {
    value.set_null();
    return RC::SUCCESS;
  }

  // 特殊处理除零：返回NULL
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
    case Type::ADD: {
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, AddOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, AddOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
    } break;
    case Type::SUB:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, SubtractOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, SubtractOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::MUL:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, MultiplyOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, MultiplyOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::DIV:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, DivideOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, DivideOperator>(
            (float *)left.data(), (float *)right.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::NEGATIVE:
      if (attr_type == AttrType::INTS) {
        unary_operator<LEFT_CONSTANT, int, NegateOperator>((int *)left.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        unary_operator<LEFT_CONSTANT, float, NegateOperator>(
            (float *)left.data(), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    default: rc = RC::UNIMPLEMENTED; break;
  }
  if (rc == RC::SUCCESS) {
    result.set_count(result.capacity());
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

  if (arithmetic_type_ == Type::NEGATIVE) {
    column.init(target_type, left_column.attr_len(), left_column.count());
    bool left_const = left_column.column_type() == Column::Type::CONSTANT_COLUMN;
    column.set_column_type(left_const ? Column::Type::CONSTANT_COLUMN : Column::Type::NORMAL_COLUMN);
    rc = execute_calc<false, false>(left_column, right_column, column, arithmetic_type_, target_type);
  } else {
    column.init(target_type, left_column.attr_len(), max(left_column.count(), right_column.count()));
    bool left_const  = left_column.column_type() == Column::Type::CONSTANT_COLUMN;
    bool right_const = right_column.column_type() == Column::Type::CONSTANT_COLUMN;
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
  // 深拷贝解析节点
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

  // 创建 SelectStmt
  Stmt *stmt = nullptr;
  RC rc = Stmt::create_stmt(db, *subquery_node_, stmt);
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

  // 生成逻辑/物理计划
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

  // 打开并执行
  rc = physical_oper->open(session->current_trx());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open physical operator for subquery. rc=%s", strrc(rc));
    return rc;
  }

  // 校验仅一列输出
  TupleSchema schema;
  rc = physical_oper->tuple_schema(schema);
  if (OB_FAIL(rc)) {
    // 有些物理算子可能未实现 tuple_schema，这种情况下通过首行推断
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

  // 记录结果类型
  if (!results_.empty()) {
    result_type_ = results_.front().attr_type();
    result_len_  = results_.front().length();
  } else {
    // 没有结果，默认类型沿用 UNDEFINED
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
    // 标量上下文中，空结果当做 NULL
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

std::unique_ptr<ParsedSqlNode> SubqueryExpr::deep_copy_parsed_node(const ParsedSqlNode &node) const
{
  auto copied = std::make_unique<ParsedSqlNode>(node.flag);
  if (node.flag == SCF_SELECT) {
    // 深拷贝 SelectSqlNode
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

////////////////////////////////////////////////////////////////////////////////
// InExpr

RC InExpr::get_value(const Tuple &tuple, Value &value) const
{
  // 求左值
  Value left_val;
  RC rc = test_expr_->get_value(tuple, left_val);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (left_val.is_null()) {
    // 简化处理：NULL 与集合比较为 false（与标准 SQL 的三值逻辑可能不同）
    value.set_boolean(false);
    return RC::SUCCESS;
  }

  // 右值应为子查询表达式
  if (set_expr_->type() != ExprType::SUBQUERY) {
    LOG_WARN("IN operator's right expr should be a subquery");
    return RC::INVALID_ARGUMENT;
  }
  auto *subq = static_cast<SubqueryExpr *>(set_expr_.get());
  rc = subq->execute_once();
  if (OB_FAIL(rc)) {
    return rc;
  }

  bool found = false;
  const auto &vals = subq->results();
  for (const auto &rv : vals) {
    if (rv.is_null()) {
      continue; // 忽略 NULL
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
