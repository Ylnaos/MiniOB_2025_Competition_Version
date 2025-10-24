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

#include "sql/operator/physical_operator.h"
#include "common/sys/rc.h"

/**
 * @brief 子查询物理算子
 * @ingroup PhysicalOperator
 * @details 封装一个完整的子查询物理计划，将子查询的结果透明地传递给外层查询
 * 子查询的结果可以像表一样被扫描
 */
class SubqueryPhysicalOperator : public PhysicalOperator
{
public:
  SubqueryPhysicalOperator() = default;
  virtual ~SubqueryPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::SUBQUERY; }
  OpType get_op_type() const override { return OpType::SUBQUERY; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override;

  /**
   * @brief 设置子查询的物理计划
   * @param child_oper 子查询的完整物理计划
   */
  void set_subquery(std::unique_ptr<PhysicalOperator> child_oper);

  /**
   * @brief 设置派生表别名（用于JOIN中的字段查找）
   * @param alias 别名
   */
  void set_alias(const std::string &alias) { alias_ = alias; }

  /**
   * @brief 获取派生表别名
   * @return 别名
   */
  const std::string &alias() const { return alias_; }

private:
  Trx *trx_ = nullptr;  ///< 用于子查询执行的事务上下文
  std::string alias_;   ///< 派生表别名
  std::unique_ptr<Tuple> aliased_tuple_;  ///< 带别名的tuple包装器
};
