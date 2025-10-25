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
// Created by WangYunlai on 2024/06/11.
//

#include "common/log/log.h"
#include "common/lang/ranges.h"
#include "event/sql_debug.h"
#include "sql/operator/group_by_physical_operator.h"
#include "sql/expr/expression_tuple.h"
#include "sql/expr/composite_tuple.h"

#include <sstream>
#include <utility>

using namespace std;
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

std::string expression_debug_name(const Expression &expr)
{
  const char *alias = expr.alias();
  if (alias != nullptr && alias[0] != '\0') {
    return alias;
  }
  const char *name = expr.name();
  if (name != nullptr && name[0] != '\0') {
    return name;
  }
  return "<anonymous>";
}

std::string dump_value(const Value &value)
{
  return value.to_string();
}

}  // namespace

GroupByPhysicalOperator::GroupByPhysicalOperator(vector<Expression *> &&expressions)
{
  aggregate_expressions_ = std::move(expressions);
  value_expressions_.reserve(aggregate_expressions_.size());
  ranges::for_each(aggregate_expressions_, [this](Expression *expr) {
    auto       *aggregate_expr = static_cast<AggregateExpr *>(expr);
    Expression *child_expr     = aggregate_expr->child().get();
    ASSERT(child_expr != nullptr, "aggregate expression must have a child expression");
    value_expressions_.emplace_back(child_expr);
  });
}

void GroupByPhysicalOperator::create_aggregator_list(AggregatorList &aggregator_list)
{
  aggregator_list.clear();
  aggregator_list.reserve(aggregate_expressions_.size());
  std::ranges::for_each(aggregate_expressions_, [&aggregator_list](Expression *expr) {
    auto *aggregate_expr = static_cast<AggregateExpr *>(expr);
    LOG_DEBUG("create aggregator: expr=%s type=%d child=%s",
        aggregate_expr->name(), static_cast<int>(aggregate_expr->aggregate_type()),
        aggregate_expr->child() ? aggregate_expr->child()->name() : "(null)");
    aggregator_list.emplace_back(aggregate_expr->create_aggregator());
    Expression *child_expr = aggregate_expr->child() ? aggregate_expr->child().get() : nullptr;
    exec_trace("[GroupBy][CreateAggregator] expr=%s type=%s child=%s", expression_debug_name(*aggregate_expr).c_str(),
        aggregate_type_to_string(aggregate_expr->aggregate_type()),
        child_expr ? expression_debug_name(*child_expr).c_str() : "(null)");
  });
  LOG_DEBUG("aggregator list created, size=%zu", aggregator_list.size());
}

RC GroupByPhysicalOperator::aggregate(AggregatorList &aggregator_list, const Tuple &tuple)
{
  ASSERT(static_cast<int>(aggregator_list.size()) == tuple.cell_num(), 
         "aggregator list size must be equal to tuple size. aggregator num: %d, tuple num: %d",
         aggregator_list.size(), tuple.cell_num());

  RC        rc = RC::SUCCESS;
  Value     value;
  const int size = static_cast<int>(aggregator_list.size());
  for (int i = 0; i < size; i++) {
    Aggregator *aggregator = aggregator_list[i].get();

    rc = tuple.cell_at(i, value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to get value from expression. rc=%s", strrc(rc));
      return rc;
    }
    auto *aggregate_expr = static_cast<AggregateExpr *>(aggregate_expressions_[i]);
    exec_trace("[GroupBy][BeforeAcc] expr=%s type=%s value=%s", expression_debug_name(*aggregate_expr).c_str(),
        aggregate_type_to_string(aggregate_expr->aggregate_type()), dump_value(value).c_str());
    rc = aggregator->accumulate(value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to accumulate value. rc=%s", strrc(rc));
      return rc;
    }
  }

  return rc;
}

RC GroupByPhysicalOperator::evaluate(GroupValueType &group_value)
{
  RC rc = RC::SUCCESS;

  vector<TupleCellSpec> aggregator_names;
  for (Expression *expr : aggregate_expressions_) {
    aggregator_names.emplace_back(expr->name());
  }

  AggregatorList &aggregators           = get<0>(group_value);
  CompositeTuple &composite_value_tuple = get<1>(group_value);

  ValueListTuple evaluated_tuple;
  vector<Value>  values;
  for (unique_ptr<Aggregator> &aggregator : aggregators) {
    Value value;
    rc = aggregator->evaluate(value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to evaluate aggregator. rc=%s", strrc(rc));
      return rc;
    }
    values.emplace_back(value);
    ASSERT(values.size() <= aggregate_expressions_.size(), "aggregate result size overflow");
    auto *aggregate_expr = static_cast<AggregateExpr *>(aggregate_expressions_[values.size() - 1]);
    exec_trace("[GroupBy][FinalizeValue] expr=%s type=%s result=%s",
        expression_debug_name(*aggregate_expr).c_str(), aggregate_type_to_string(aggregate_expr->aggregate_type()),
        dump_value(value).c_str());
  }

  evaluated_tuple.set_cells(values);
  evaluated_tuple.set_names(aggregator_names);

  composite_value_tuple.add_tuple(make_unique<ValueListTuple>(std::move(evaluated_tuple)));

  return rc;
}
