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
// Created by Wangyunlai on 2022/12/30.
//

#include "sql/optimizer/predicate_pushdown_rewriter.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"

RC PredicatePushdownRewriter::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made)
{
  RC rc = RC::SUCCESS;
  if (oper->type() != LogicalOperatorType::PREDICATE) {
    return rc;
  }

  if (oper->children().size() != 1) {
    return rc;
  }

  unique_ptr<LogicalOperator> &child_oper = oper->children().front();
  if (child_oper->type() != LogicalOperatorType::TABLE_GET) {
    return rc;
  }

  auto table_get_oper = static_cast<TableGetLogicalOperator *>(child_oper.get());

  vector<unique_ptr<Expression>> &predicate_oper_exprs = oper->expressions();
  if (predicate_oper_exprs.size() != 1) {
    return rc;
  }

  unique_ptr<Expression>             &predicate_expr = predicate_oper_exprs.front();
  vector<unique_ptr<Expression>> pushdown_exprs;
  rc = get_exprs_can_pushdown(predicate_expr, pushdown_exprs);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get exprs can pushdown. rc=%s", strrc(rc));
    return rc;
  }

  if (!predicate_expr || is_empty_predicate(predicate_expr)) {
    // 所有的表达式都下推到了下层算子
    // 这个predicate operator其实就可以不要了。但是这里没办法删除，弄一个空的表达式吧
    LOG_TRACE("all expressions of predicate operator were pushdown to table get operator, then make a fake one");

    Value value((bool)true);
    predicate_expr = unique_ptr<Expression>(new ValueExpr(value));
  }

  if (!pushdown_exprs.empty()) {
    change_made = true;
    table_get_oper->set_predicates(std::move(pushdown_exprs));
  }
  return rc;
}

bool PredicatePushdownRewriter::is_empty_predicate(unique_ptr<Expression> &expr)
{
  bool bool_ret = false;
  if (!expr) {
    return true;
  }

  if (expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conjunction_expr = static_cast<ConjunctionExpr *>(expr.get());
    if (conjunction_expr->children().empty()) {
      bool_ret = true;
    }
  }

  return bool_ret;
}

/**
 * 查看表达式是否可以直接下放到table get算子的filter
 * @param expr 是当前的表达式。如果可以下放给table get 算子，执行完成后expr就失效了
 * @param pushdown_exprs 当前所有要下放给table get 算子的filter。此函数执行多次，
 *                       pushdown_exprs 只会增加，不要做清理操作
 */
RC PredicatePushdownRewriter::get_exprs_can_pushdown(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &pushdown_exprs)
{
  RC rc = RC::SUCCESS;
  if (expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conjunction_expr = static_cast<ConjunctionExpr *>(expr.get());
    // 或 操作的处理：如果包含子查询，不进行下推优化，直接跳过
    if (conjunction_expr->conjunction_type() == ConjunctionExpr::Type::OR) {
      // 检查是否包含子查询，如果不包含子查询可以按原来的逻辑处理
      bool has_subquery = false;
      vector<unique_ptr<Expression>> &child_exprs = conjunction_expr->children();
      for (auto &child : child_exprs) {
        if (expression_contains_subquery(child.get())) {
          has_subquery = true;
          break;
        }
      }

      if (has_subquery) {
        LOG_TRACE("or operation with subquery, skip pushdown optimization");
        // 对于包含子查询的OR操作，不进行下推，直接返回成功
        return RC::SUCCESS;
      } else {
        // 不包含子查询的OR操作，按照原来的逻辑返回未实现
        LOG_WARN("unsupported or operation without subquery");
        rc = RC::UNIMPLEMENTED;
        return rc;
      }
    }

    vector<unique_ptr<Expression>> &child_exprs = conjunction_expr->children();
    for (auto iter = child_exprs.begin(); iter != child_exprs.end();) {
      // 对每个子表达式，判断是否可以下放到table get 算子
      // 如果可以的话，就从当前孩子节点中删除他
      rc = get_exprs_can_pushdown(*iter, pushdown_exprs);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to get pushdown expressions. rc=%s", strrc(rc));
        return rc;
      }

      if (!*iter) {
        iter = child_exprs.erase(iter);
      } else {
        ++iter;
      }
    }
  } else if (expr->type() == ExprType::COMPARISON) {
    // 如果是比较操作，并且比较的左边或右边是表某个列值，那么就下推下去

    pushdown_exprs.emplace_back(std::move(expr));
  }
  return rc;
}

bool PredicatePushdownRewriter::expression_contains_subquery(Expression *expr)
{
  if (expr == nullptr) {
    return false;
  }

  // 如果是子查询表达式，直接返回true
  if (expr->type() == ExprType::SUBQUERY) {
    return true;
  }

  // 如果是EXISTS/NOT EXISTS表达式，检查子查询
  if (expr->type() == ExprType::EXISTS) {
    ExistsExpr *exists_expr = static_cast<ExistsExpr *>(expr);
    return exists_expr->subquery() && exists_expr->subquery()->type() == ExprType::SUBQUERY;
  }

  // 如果是IN/NOT IN表达式，检查右边的子查询
  if (expr->type() == ExprType::IN_LIST) {
    InExpr *in_expr = static_cast<InExpr *>(expr);
    return in_expr->set_expr() && in_expr->set_expr()->type() == ExprType::SUBQUERY;
  }

  // 如果是比较表达式，检查左右两边
  if (expr->type() == ExprType::COMPARISON) {
    ComparisonExpr *comp_expr = static_cast<ComparisonExpr *>(expr);
    return expression_contains_subquery(comp_expr->left().get()) ||
           expression_contains_subquery(comp_expr->right().get());
  }

  // 如果是复合表达式，递归检查子表达式
  if (expr->type() == ExprType::CONJUNCTION) {
    ConjunctionExpr *conj_expr = static_cast<ConjunctionExpr *>(expr);
    for (const auto &child : conj_expr->children()) {
      if (expression_contains_subquery(child.get())) {
        return true;
      }
    }
  }

  return false;
}
