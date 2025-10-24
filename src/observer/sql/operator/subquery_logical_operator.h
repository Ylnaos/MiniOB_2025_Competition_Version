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

#pragma once

#include "sql/operator/logical_operator.h"

/**
 * @brief 子查询逻辑算子
 * @ingroup LogicalOperator
 * @details 表示一个子查询，将内层查询的逻辑计划封装起来
 * 子查询可以像表一样作为数据源被外层查询使用
 */
class SubqueryLogicalOperator : public LogicalOperator
{
public:
  SubqueryLogicalOperator() = default;
  virtual ~SubqueryLogicalOperator() = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::SUBQUERY; }
  OpType get_op_type() const override { return OpType::LOGICALSUBQUERY; }

  /**
   * @brief 设置子查询的逻辑计划
   * @param subquery_plan 子查询的完整逻辑计划
   */
  void set_subquery_plan(std::unique_ptr<LogicalOperator> subquery_plan);

  /**
   * @brief 获取子查询的逻辑计划
   * @return 子查询逻辑计划的指针
   */
  LogicalOperator *subquery_plan() const;

  unique_ptr<LogicalProperty> find_log_prop(const vector<LogicalProperty *> &log_props) override;

  /**
   * @brief 设置派生表别名（用于将子查询结果作为表使用）
   * @param alias 派生表别名
   */
  void set_alias(const std::string &alias) { alias_ = alias; }

  /**
   * @brief 获取派生表别名
   * @return 派生表别名
   */
  const std::string &alias() const { return alias_; }

private:
  // 注意：不使用children_来存储，因为子查询是特殊的封装关系
  // 使用独立的成员变量更清晰
  std::unique_ptr<LogicalOperator> subquery_plan_;
  std::string alias_;  ///< 派生表别名（用于外层查询引用）
};
