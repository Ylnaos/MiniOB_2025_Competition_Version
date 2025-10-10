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
// Created by Claude Code for MiniOB Subquery Support
//

#include "sql/operator/subquery_logical_operator.h"

void SubqueryLogicalOperator::set_subquery_plan(std::unique_ptr<LogicalOperator> subquery_plan)
{
  subquery_plan_ = std::move(subquery_plan);
}

LogicalOperator *SubqueryLogicalOperator::subquery_plan() const
{
  return subquery_plan_.get();
}

unique_ptr<LogicalProperty> SubqueryLogicalOperator::find_log_prop(const vector<LogicalProperty *> &log_props)
{
  // 子查询的逻辑属性直接继承自内层查询的逻辑属性
  if (subquery_plan_) {
    vector<LogicalProperty *> child_props;
    return subquery_plan_->find_log_prop(child_props);
  }
  return nullptr;
}
