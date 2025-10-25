/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/log/log.h"
#include "common/lang/ranges.h"
#include "common/type/attr_type.h"
#include "common/value.h"
#include "event/sql_debug.h"
#include "sql/operator/aggregate_vec_physical_operator.h"
#include "sql/expr/aggregate_state.h"
#include "sql/expr/expression_tuple.h"
#include "sql/expr/composite_tuple.h"

#include <algorithm>
#include <sstream>
#include <utility>

using namespace common;

namespace {

template <typename... Args>
void exec_trace(const char *fmt, Args &&... args)
{
  LOG_INFO(fmt, std::forward<Args>(args)...);
  sql_debug(fmt, std::forward<Args>(args)...);
}

const char *aggregate_type_to_string(AggregateExpr::Type type)
{
  switch (type) {
    case AggregateExpr::Type::COUNT:
      return "COUNT";
    case AggregateExpr::Type::SUM:
      return "SUM";
    case AggregateExpr::Type::AVG:
      return "AVG";
    case AggregateExpr::Type::MAX:
      return "MAX";
    case AggregateExpr::Type::MIN:
      return "MIN";
    default:
      return "UNKNOWN";
  }
}

std::string bool_to_string(bool value)
{
  return value ? "true" : "false";
}

std::string dump_aggregate_state(AggregateExpr::Type aggr_type, AttrType child_type, void *state)
{
  if (state == nullptr) {
    return "state=null";
  }

  std::ostringstream oss;
  switch (aggr_type) {
    case AggregateExpr::Type::COUNT: {
      auto *st = reinterpret_cast<CountState<int> *>(state);
      oss << "count=" << st->value;
      break;
    }
    case AggregateExpr::Type::SUM: {
      if (child_type == AttrType::INTS) {
        auto *st = reinterpret_cast<SumState<int> *>(state);
        oss << "sum=" << st->value << ", has_value=" << bool_to_string(st->has_value);
      } else if (child_type == AttrType::FLOATS) {
        auto *st = reinterpret_cast<SumState<float> *>(state);
        oss << "sum=" << st->value << ", has_value=" << bool_to_string(st->has_value);
      } else {
        oss << "sum=unsupported(child_type=" << attr_type_to_string(child_type) << ")";
      }
      break;
    }
    case AggregateExpr::Type::AVG: {
      if (child_type == AttrType::INTS) {
        auto *st = reinterpret_cast<AvgState<int> *>(state);
        oss << "accumulate=" << st->value << ", count=" << st->count;
      } else if (child_type == AttrType::FLOATS) {
        auto *st = reinterpret_cast<AvgState<float> *>(state);
        oss << "accumulate=" << st->value << ", count=" << st->count;
      } else {
        oss << "avg=unsupported(child_type=" << attr_type_to_string(child_type) << ")";
      }
      break;
    }
    case AggregateExpr::Type::MAX:
    case AggregateExpr::Type::MIN: {
      auto *st = reinterpret_cast<MinMaxState *>(state);
      oss << "has_value=" << bool_to_string(st->has_value);
      if (st->has_value) {
        oss << ", current=" << st->value.to_string();
      }
      break;
    }
    default:
      oss << "state=unknown";
      break;
  }
  return oss.str();
}

std::string dump_column_samples(const Column &column, int sample_limit = 5)
{
  std::ostringstream oss;
  const int row_count = column.count();
  oss << "rows=" << row_count;
  if (row_count == 0) {
    return oss.str();
  }

  const int limit = std::min(row_count, sample_limit);
  oss << ", samples=[";
  for (int i = 0; i < limit; ++i) {
    Value v = column.get_value(i);
    oss << v.to_string();
    if (i + 1 < limit) {
      oss << ", ";
    }
  }
  if (row_count > limit) {
    oss << ", ...";
  }
  oss << "]";
  return oss.str();
}

std::string expression_debug_name(const Expression &expr)
{
  const char *alias = expr.alias();
  const char *name  = expr.name();
  if (alias != nullptr && alias[0] != '\0') {
    return std::string(alias);
  }
  if (name != nullptr && name[0] != '\0') {
    return std::string(name);
  }
  return std::string("<anonymous>");
}

}  // namespace

AggregateVecPhysicalOperator::AggregateVecPhysicalOperator(vector<Expression *> &&expressions)
{
  aggregate_expressions_ = std::move(expressions);
  value_expressions_.reserve(aggregate_expressions_.size());

  ranges::for_each(aggregate_expressions_, [this](Expression *expr) {
    auto *      aggregate_expr = static_cast<AggregateExpr *>(expr);
    Expression *child_expr     = aggregate_expr->child().get();
    ASSERT(child_expr != nullptr, "aggregation expression must have a child expression");
    value_expressions_.emplace_back(child_expr);
  });

  for (size_t i = 0; i < aggregate_expressions_.size(); i++) {
    auto &expr = aggregate_expressions_[i];
    ASSERT(expr->type() == ExprType::AGGREGATION, "expected an aggregation expression");
    auto *aggregate_expr = static_cast<AggregateExpr *>(expr);
    void *state_ptr = create_aggregate_state(aggregate_expr->aggregate_type(), aggregate_expr->child()->value_type());
    ASSERT(state_ptr != nullptr, "failed to create aggregate state");
    aggr_values_.insert(state_ptr);
    output_chunk_.add_column(make_unique<Column>(aggregate_expr->value_type(), aggregate_expr->value_length()), i);
  }
}

RC AggregateVecPhysicalOperator::open(Trx *trx)
{
  // Reset internal states so the operator can be reopened safely (join RHS, subquery, view, etc.)
  outputed_ = false;
  output_chunk_.reset_data();
  for (size_t i = 0; i < aggregate_expressions_.size(); i++) {
    ASSERT(aggregate_expressions_[i]->type() == ExprType::AGGREGATION, "expect aggregation expression");
    auto *aggregate_expr = static_cast<AggregateExpr *>(aggregate_expressions_[i]);
    reset_aggregate_state(
        aggr_values_.at(i), aggregate_expr->aggregate_type(), aggregate_expr->child()->value_type());
    exec_trace("[AggVec][Reset] index=%zu expr=%s type=%s child_type=%s state=%s", i,
        expression_debug_name(*aggregate_expr).c_str(),
        aggregate_type_to_string(aggregate_expr->aggregate_type()),
        attr_type_to_string(aggregate_expr->child()->value_type()),
        dump_aggregate_state(aggregate_expr->aggregate_type(),
            aggregate_expr->child()->value_type(), aggr_values_.at(i)).c_str());
  }

  ASSERT(children_.size() == 1, "group by operator only support one child, but got %d", children_.size());

  PhysicalOperator &child = *children_[0];
  RC                rc    = child.open(trx);
  if (OB_FAIL(rc)) {
    LOG_INFO("failed to open child operator. rc=%s", strrc(rc));
    return rc;
  }

  size_t  chunk_index       = 0;
  int64_t total_input_rows  = 0;

  while (OB_SUCC(rc = child.next(chunk_))) {
    const int current_rows = chunk_.rows();
    total_input_rows += current_rows;
    exec_trace("[AggVec][Chunk] index=%zu rows=%d column_num=%d", chunk_index, current_rows, chunk_.column_num());
    for (size_t aggr_idx = 0; aggr_idx < aggregate_expressions_.size(); aggr_idx++) {
      Column column;
      value_expressions_[aggr_idx]->get_column(chunk_, column);
      ASSERT(aggregate_expressions_[aggr_idx]->type() == ExprType::AGGREGATION, "expect aggregate expression");
      auto *aggregate_expr = static_cast<AggregateExpr *>(aggregate_expressions_[aggr_idx]);
      exec_trace("[AggVec][BeforeUpdate] index=%zu expr=%s type=%s child_type=%s column=%s state=%s",
          aggr_idx,
          expression_debug_name(*aggregate_expr).c_str(),
          aggregate_type_to_string(aggregate_expr->aggregate_type()),
          attr_type_to_string(aggregate_expr->child()->value_type()),
          dump_column_samples(column).c_str(),
          dump_aggregate_state(aggregate_expr->aggregate_type(),
              aggregate_expr->child()->value_type(), aggr_values_.at(aggr_idx)).c_str());
      rc = aggregate_state_update_by_column(aggr_values_.at(aggr_idx), aggregate_expr->aggregate_type(), aggregate_expr->child()->value_type(), column);
      if (OB_FAIL(rc)) {
        LOG_INFO("failed to update aggregate state. rc=%s", strrc(rc));
        return rc;
      }
      exec_trace("[AggVec][AfterUpdate] index=%zu expr=%s state=%s", aggr_idx,
          expression_debug_name(*aggregate_expr).c_str(),
          dump_aggregate_state(aggregate_expr->aggregate_type(),
              aggregate_expr->child()->value_type(), aggr_values_.at(aggr_idx)).c_str());
    }
    chunk_index++;
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
    exec_trace("[AggVec][Summary] chunk_count=%zu total_rows=%ld", chunk_index, total_input_rows);
  }

  if (OB_FAIL(rc)) {
    return rc;
  }

  // 无论输入是否为空，聚合查询都必须返回一行结果：
  // COUNT -> 0，其它聚合(SUM/AVG/MIN/MAX)在无输入时返回 NULL
  // 在 open() 完成后立即 finalize 所有聚合状态到 output_chunk_
  for (size_t i = 0; i < aggr_values_.size(); i++) {
    ASSERT(aggregate_expressions_[i]->type() == ExprType::AGGREGATION, "expect aggregation expression");
    auto *aggregate_expr = static_cast<AggregateExpr *>(aggregate_expressions_[i]);
    rc = finialize_aggregate_state(aggr_values_.at(i), aggregate_expr->aggregate_type(),
                                   aggregate_expr->child()->value_type(), output_chunk_.column(i));
    if (OB_FAIL(rc)) {
      LOG_INFO("failed to finialize aggregate state. rc=%s", strrc(rc));
      return rc;
    }
    exec_trace("[AggVec][Finalize] index=%zu expr=%s result=%s", i,
        expression_debug_name(*aggregate_expr).c_str(),
        dump_column_samples(output_chunk_.column(i), 1).c_str());
  }

  return rc;
}

template <class STATE, typename T>
void AggregateVecPhysicalOperator::update_aggregate_state(void *state, const Column &column)
{
  STATE *state_ptr = reinterpret_cast<STATE *>(state);
  T *    data      = (T *)column.data();
  state_ptr->update(data, column.count());
}

RC AggregateVecPhysicalOperator::next(Chunk &chunk)
{
  if (outputed_) {
    return RC::RECORD_EOF;
  }

  // 聚合状态已在 open() 中完成 finalize，这里只需引用结果
  chunk.reference(output_chunk_);
  outputed_ = true;

  return RC::SUCCESS;
}

RC AggregateVecPhysicalOperator::close()
{
  children_[0]->close();
  LOG_INFO("close group by operator");
  return RC::SUCCESS;
}
