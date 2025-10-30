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

#include "common/lang/vector.h"
#include "common/lang/unordered_set.h"
#include "sql/optimizer/rewrite_rule.h"

class Expression;
class LogicalOperator;
class JoinLogicalOperator;

/**
 * @brief 将一些谓词表达式下推到join中
 * @ingroup Rewriter
 * @details 将WHERE子句中的谓词条件下推到JOIN算子或JOIN的子节点
 */
class PredicateToJoinRewriter : public RewriteRule
{
public:
  PredicateToJoinRewriter()          = default;
  virtual ~PredicateToJoinRewriter() = default;

  RC rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made) override;

private:
  /**
   * @brief 获取算子涉及的所有表ID
   * @param oper 算子
   * @param table_ids 输出的表ID集合
   */
  void get_table_ids(LogicalOperator *oper, std::unordered_set<std::string> &table_ids);

  /**
   * @brief 获取表达式涉及的所有表ID
   * @param expr 表达式
   * @param table_ids 输出的表ID集合
   */
  void get_expr_table_ids(Expression *expr, std::unordered_set<std::string> &table_ids);

  /**
   * @brief 检查谓词是否可以下推到指定的算子
   * @param expr 谓词表达式
   * @param oper_table_ids 算子涉及的表ID集合
   * @return true if the predicate can be pushed down
   */
  bool can_pushdown_to_operator(Expression *expr, const std::unordered_set<std::string> &oper_table_ids);

  /**
   * @brief 尝试将谓词下推到JOIN算子
   * @param predicate_oper Predicate算子
   * @param join_oper JOIN算子
   * @param change_made 是否发生了改变
   * @return RC
   */
  RC pushdown_to_join(
      unique_ptr<LogicalOperator> &predicate_oper, JoinLogicalOperator *join_oper, bool &change_made);
};
