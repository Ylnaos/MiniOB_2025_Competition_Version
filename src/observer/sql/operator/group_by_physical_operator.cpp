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
#include "sql/operator/group_by_physical_operator.h"
#include "sql/expr/expression_tuple.h"
#include "sql/expr/composite_tuple.h"
#include <unordered_set>

using namespace std;

// 全局计数器，用于追踪聚合操作和检测重复
static int group_by_create_aggregator_list_calls = 0;
static int group_by_aggregate_calls = 0;
static std::unordered_set<uintptr_t> processed_pointers; // 用于检测重复的指针地址
using namespace common;

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
  group_by_create_aggregator_list_calls++;
  aggregator_list.clear();
  aggregator_list.reserve(aggregate_expressions_.size());

  LOG_INFO("DATA_FLOW: GroupByPhysicalOperator::create_aggregator_list[%d] - 操作符=%p, 聚合表达式数量=%zu",
            group_by_create_aggregator_list_calls, this, aggregate_expressions_.size());

  int index = 0;
  std::ranges::for_each(aggregate_expressions_, [&aggregator_list, &index, this](Expression *expr) {
    auto *aggregate_expr = static_cast<AggregateExpr *>(expr);

    const char* type_name = "";
    switch (aggregate_expr->aggregate_type()) {
      case AggregateExpr::Type::SUM: type_name = "SUM"; break;
      case AggregateExpr::Type::COUNT: type_name = "COUNT"; break;
      case AggregateExpr::Type::AVG: type_name = "AVG"; break;
      case AggregateExpr::Type::MAX: type_name = "MAX"; break;
      case AggregateExpr::Type::MIN: type_name = "MIN"; break;
      default: type_name = "UNKNOWN"; break;
    }

    LOG_INFO("DATA_FLOW: 创建聚合器[%d][%d] - 表达式=%p, 名称=%s, 类型=%s, 子表达式=%s",
              group_by_create_aggregator_list_calls, index, aggregate_expr, aggregate_expr->name(), type_name,
              aggregate_expr->child() ? aggregate_expr->child()->name() : "(null)");

    auto aggregator = aggregate_expr->create_aggregator();

    // 检测是否重复创建同一个聚合器
    uintptr_t ptr_value = reinterpret_cast<uintptr_t>(aggregator.get());
    bool is_duplicate = processed_pointers.find(ptr_value) != processed_pointers.end();
    if (is_duplicate) {
      LOG_INFO("DATA_FLOW: 检测到重复聚合器地址[%d][%d] - 聚合器=%p, 地址=%p",
                group_by_create_aggregator_list_calls, index, this, aggregator.get());
    } else {
      processed_pointers.insert(ptr_value);
    }

    LOG_INFO("DATA_FLOW: 聚合器[%d][%d]创建完成 - 聚合器地址=%p, 重复检测=%s",
              group_by_create_aggregator_list_calls, index, aggregator.get(), is_duplicate ? "是" : "否");

    aggregator_list.emplace_back(std::move(aggregator));
    index++;
  });

  LOG_INFO("DATA_FLOW: 聚合器列表创建完成[%d] - 大小=%zu, 操作符=%p",
            group_by_create_aggregator_list_calls, aggregator_list.size(), this);
}

RC GroupByPhysicalOperator::aggregate(AggregatorList &aggregator_list, const Tuple &tuple)
{
  group_by_aggregate_calls++;

  ASSERT(static_cast<int>(aggregator_list.size()) == tuple.cell_num(),
         "aggregator list size must be equal to tuple size. aggregator num: %d, tuple num: %d",
         aggregator_list.size(), tuple.cell_num());

  // 检测重复的元组地址
  uintptr_t tuple_ptr = reinterpret_cast<uintptr_t>(&tuple);
  bool is_duplicate_tuple = processed_pointers.find(tuple_ptr) != processed_pointers.end();

  LOG_INFO("DATA_FLOW: GroupByPhysicalOperator::aggregate[%d] - 聚合器数量=%zu, 元组单元格数=%d, 操作符=%p, 元组地址=%p, 重复检测=%s",
            group_by_aggregate_calls, aggregator_list.size(), tuple.cell_num(), this, &tuple, is_duplicate_tuple ? "是" : "否");

  if (is_duplicate_tuple) {
    LOG_INFO("DATA_FLOW: 警告：检测到重复元组[%d] - 元组地址=%p, 数据=%s", group_by_aggregate_calls, &tuple, tuple.to_string().c_str());
  }

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

    LOG_INFO("DATA_FLOW: 聚合计算[%d][%d] - 聚合器=%p, 输入值类型=%d, 输入值=%s",
              group_by_aggregate_calls, i, aggregator, value.attr_type(), value.to_string().c_str());

    rc = aggregator->accumulate(value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to accumulate value. rc=%s", strrc(rc));
      return rc;
    }

    LOG_INFO("DATA_FLOW: 聚合计算[%d][%d]完成 - 聚合器=%p, 累加成功", group_by_aggregate_calls, i, aggregator);
  }

  LOG_INFO("DATA_FLOW: 聚合计算完成[%d] - 操作符=%p", group_by_aggregate_calls, this);
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
  }

  evaluated_tuple.set_cells(values);
  evaluated_tuple.set_names(aggregator_names);

  composite_value_tuple.add_tuple(make_unique<ValueListTuple>(std::move(evaluated_tuple)));

  return rc;
}
