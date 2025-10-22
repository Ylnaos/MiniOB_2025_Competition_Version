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
// Created by Wangyunlai on 2024/05/30.
//

#include "sql/expr/expression_iterator.h"
#include "sql/expr/expression.h"
#include "common/log/log.h"

using namespace std;

RC ExpressionIterator::iterate_child_expr(Expression &expr, function<RC(unique_ptr<Expression> &)> callback)
{
  RC rc = RC::SUCCESS;

  switch (expr.type()) {
    case ExprType::CAST: {
      auto &cast_expr = static_cast<CastExpr &>(expr);
      rc = callback(cast_expr.child());
    } break;

    case ExprType::COMPARISON: {

      auto &comparison_expr = static_cast<ComparisonExpr &>(expr);
      rc = callback(comparison_expr.left());

      if (OB_SUCC(rc)) {
        rc = callback(comparison_expr.right());
      }

    } break;

    case ExprType::CONJUNCTION: {
      auto &conjunction_expr = static_cast<ConjunctionExpr &>(expr);
      for (auto &child : conjunction_expr.children()) {
        rc = callback(child);
        if (OB_FAIL(rc)) {
          break;
        }
      }
    } break;

  case ExprType::ARITHMETIC: {

      auto &arithmetic_expr = static_cast<ArithmeticExpr &>(expr);
      // 左子树一定存在
      rc = callback(arithmetic_expr.left());
      if (OB_SUCC(rc)) {
        // 一元负号时右子树为空，需判空再回调，避免解引用空指针导致崩溃
        auto &right = arithmetic_expr.right();
        if (right) {
          rc = callback(right);
        }
      }
    } break;

    case ExprType::AGGREGATION: {
      auto &aggregate_expr = static_cast<AggregateExpr &>(expr);
      rc = callback(aggregate_expr.child());
    } break;

    case ExprType::UNBOUND_AGGREGATION: {
      // 解析阶段的未绑定聚合表达式：也需要递归其子节点（便于相关子查询替换等场景）
      auto &uagg = static_cast<UnboundAggregateExpr &>(expr);
      rc = callback(uagg.child());
    } break;

    case ExprType::FUNCTION: {
      auto &fn = static_cast<ScalarFunctionExpr &>(expr);
      rc = callback(fn.child());
    } break;

    case ExprType::SUBQUERY: {
      // 作为叶子节点，无需递归
    } break;

    case ExprType::EXISTS: {
      auto &exists_expr = static_cast<ExistsExpr &>(expr);
      rc = callback(exists_expr.subquery());
    } break;

    case ExprType::IN_LIST: {
      auto &in_expr = static_cast<InExpr &>(expr);
      if (OB_SUCC(rc)) {
        rc = callback(in_expr.test_expr());
      }
      if (OB_SUCC(rc)) {
        rc = callback(in_expr.set_expr());
      }
    } break;

    case ExprType::MATCH_AGAINST: {
      auto &match_expr = static_cast<MatchAgainstExpr &>(expr);
      for (auto &field : match_expr.fields()) {
        rc = callback(field);
        if (OB_FAIL(rc)) {
          break;
        }
      }
      if (OB_SUCC(rc)) {
        rc = callback(match_expr.query_expression());
      }
    } break;

    case ExprType::NONE:
    case ExprType::STAR:
    case ExprType::UNBOUND_FIELD:
    case ExprType::FIELD:
    case ExprType::VALUE: {
      // Do nothing
    } break;

    default: {
      ASSERT(false, "Unknown expression type");
    }
  }

  return rc;
}
