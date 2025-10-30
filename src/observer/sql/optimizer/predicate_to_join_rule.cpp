/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/predicate_to_join_rule.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "sql/operator/predicate_logical_operator.h"

RC PredicateToJoinRewriter::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made)
{
  RC rc = RC::SUCCESS;
  change_made = false;

  // 只处理Predicate算子
  if (oper->type() != LogicalOperatorType::PREDICATE) {
    return rc;
  }

  if (oper->children().size() != 1) {
    return rc;
  }

  unique_ptr<LogicalOperator> &child_oper = oper->children().front();

  // 子节点必须是JOIN算子
  if (child_oper->type() != LogicalOperatorType::JOIN) {
    return rc;
  }

  JoinLogicalOperator *join_oper = static_cast<JoinLogicalOperator *>(child_oper.get());

  return pushdown_to_join(oper, join_oper, change_made);
}

RC PredicateToJoinRewriter::pushdown_to_join(
    unique_ptr<LogicalOperator> &predicate_oper, JoinLogicalOperator *join_oper, bool &change_made)
{
  // 获取Predicate算子的表达式
  vector<unique_ptr<Expression>> &predicate_exprs = predicate_oper->expressions();
  if (predicate_exprs.empty() || predicate_exprs.size() != 1) {
    return RC::SUCCESS;
  }

  unique_ptr<Expression> &root_expr = predicate_exprs.front();

  // 获取JOIN的左右子节点涉及的表
  auto &join_children = join_oper->children();
  if (join_children.size() != 2) {
    return RC::SUCCESS;
  }

  std::unordered_set<std::string> left_table_ids;
  std::unordered_set<std::string> right_table_ids;
  std::unordered_set<std::string> join_table_ids;

  get_table_ids(join_children[0].get(), left_table_ids);
  get_table_ids(join_children[1].get(), right_table_ids);

  join_table_ids = left_table_ids;
  join_table_ids.insert(right_table_ids.begin(), right_table_ids.end());

  // 分解AND连接的表达式
  vector<unique_ptr<Expression>> and_exprs;
  if (root_expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conj_expr = static_cast<ConjunctionExpr *>(root_expr.get());
    if (conj_expr->conjunction_type() == ConjunctionExpr::Type::AND) {
      // 提取所有AND子表达式
      for (auto &child : conj_expr->children()) {
        and_exprs.push_back(child->copy());
      }
    } else {
      // OR表达式不做下推
      return RC::SUCCESS;
    }
  } else {
    // 单个表达式
    and_exprs.push_back(root_expr->copy());
  }

  // 分类谓词：哪些可以下推到左表，哪些下推到右表，哪些下推到JOIN
  vector<unique_ptr<Expression>> left_pushdown_exprs;
  vector<unique_ptr<Expression>> right_pushdown_exprs;
  vector<unique_ptr<Expression>> join_pushdown_exprs;
  vector<unique_ptr<Expression>> remaining_exprs;

  for (auto &expr : and_exprs) {
    std::unordered_set<std::string> expr_table_ids;
    get_expr_table_ids(expr.get(), expr_table_ids);

    if (expr_table_ids.empty()) {
      // 常量表达式，保留在Predicate中
      remaining_exprs.push_back(std::move(expr));
    } else if (expr_table_ids.size() == 1) {
      // 单表谓词
      std::string table_id = *expr_table_ids.begin();
      if (left_table_ids.count(table_id)) {
        left_pushdown_exprs.push_back(std::move(expr));
        change_made = true;
      } else if (right_table_ids.count(table_id)) {
        right_pushdown_exprs.push_back(std::move(expr));
        change_made = true;
      } else {
        remaining_exprs.push_back(std::move(expr));
      }
    } else {
      // 多表谓词
      bool all_in_left = true;
      bool all_in_right = true;
      bool all_in_join = true;

      for (const auto &tid : expr_table_ids) {
        if (!left_table_ids.count(tid)) all_in_left = false;
        if (!right_table_ids.count(tid)) all_in_right = false;
        if (!join_table_ids.count(tid)) all_in_join = false;
      }

      if (all_in_left) {
        left_pushdown_exprs.push_back(std::move(expr));
        change_made = true;
      } else if (all_in_right) {
        right_pushdown_exprs.push_back(std::move(expr));
        change_made = true;
      } else if (all_in_join) {
        // 涉及左右两表的谓词，下推到JOIN
        join_pushdown_exprs.push_back(std::move(expr));
        change_made = true;
      } else {
        remaining_exprs.push_back(std::move(expr));
      }
    }
  }

  // 下推到JOIN算子
  if (!join_pushdown_exprs.empty()) {
    for (auto &expr : join_pushdown_exprs) {
      join_oper->add_join_predicate(std::move(expr));
    }
  }

  // 下推到左右子节点
  if (!left_pushdown_exprs.empty()) {
    auto predicate_oper_for_left = make_unique<PredicateLogicalOperator>(std::move(left_pushdown_exprs.front()));
    predicate_oper_for_left->add_child(std::move(join_children[0]));
    join_children[0] = std::move(predicate_oper_for_left);
  }

  if (!right_pushdown_exprs.empty()) {
    auto predicate_oper_for_right = make_unique<PredicateLogicalOperator>(std::move(right_pushdown_exprs.front()));
    predicate_oper_for_right->add_child(std::move(join_children[1]));
    join_children[1] = std::move(predicate_oper_for_right);
  }

  // 更新剩余的Predicate表达式
  if (remaining_exprs.empty() && change_made) {
    // 所有谓词都下推了，可以移除Predicate算子
    // 但这里不方便直接删除，保留一个true常量
    Value true_value((bool)true);
    root_expr = make_unique<ValueExpr>(true_value);
  } else if (!remaining_exprs.empty() && remaining_exprs.size() < and_exprs.size()) {
    // 部分谓词下推了，重建Predicate表达式
    if (remaining_exprs.size() == 1) {
      root_expr = std::move(remaining_exprs.front());
    } else {
      root_expr = make_unique<ConjunctionExpr>(ConjunctionExpr::Type::AND, std::move(remaining_exprs));
    }
  }

  return RC::SUCCESS;
}

void PredicateToJoinRewriter::get_table_ids(LogicalOperator *oper, std::unordered_set<std::string> &table_ids)
{
  if (oper == nullptr) {
    return;
  }

  if (oper->type() == LogicalOperatorType::TABLE_GET) {
    TableGetLogicalOperator *table_get = static_cast<TableGetLogicalOperator *>(oper);
    // 使用表名作为表ID
    table_ids.insert(table_get->table()->name());
    return;
  }

  // 递归获取子节点的表ID
  for (auto &child : oper->children()) {
    get_table_ids(child.get(), table_ids);
  }
}

void PredicateToJoinRewriter::get_expr_table_ids(Expression *expr, std::unordered_set<std::string> &table_ids)
{
  if (expr == nullptr) {
    return;
  }

  if (expr->type() == ExprType::FIELD) {
    FieldExpr *field_expr = static_cast<FieldExpr *>(expr);
    // 使用表名作为表ID
    table_ids.insert(field_expr->table_name());
    return;
  }

  // 递归处理子表达式
  if (expr->type() == ExprType::COMPARISON) {
    ComparisonExpr *comp_expr = static_cast<ComparisonExpr *>(expr);
    get_expr_table_ids(comp_expr->left().get(), table_ids);
    get_expr_table_ids(comp_expr->right().get(), table_ids);
  } else if (expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conj_expr = static_cast<ConjunctionExpr *>(expr);
    for (auto &child : conj_expr->children()) {
      get_expr_table_ids(child.get(), table_ids);
    }
  }
  // 其他类型的表达式不处理
}

bool PredicateToJoinRewriter::can_pushdown_to_operator(
    Expression *expr, const std::unordered_set<std::string> &oper_table_ids)
{
  std::unordered_set<std::string> expr_table_ids;
  get_expr_table_ids(expr, expr_table_ids);

  // 检查表达式涉及的所有表是否都在算子的表集合中
  for (const auto &table_id : expr_table_ids) {
    if (!oper_table_ids.count(table_id)) {
      return false;
    }
  }

  return true;
}
