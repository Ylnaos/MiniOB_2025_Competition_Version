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
// Created by Longda on 2021/4/13.
//

#include <string.h>

#include "optimize_stage.h"

#include "common/conf/ini.h"
#include "common/io/io.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "sql/operator/logical_operator.h"
#include "sql/expr/expression_iterator.h"
#include "sql/stmt/stmt.h"
#include "sql/optimizer/cascade/optimizer.h"
#include "sql/optimizer/optimizer_utils.h"

using namespace std;
using namespace common;

RC OptimizeStage::handle_request(SQLStageEvent *sql_event)
{
  unique_ptr<LogicalOperator> logical_operator;

  RC rc = create_logical_plan(sql_event, logical_operator);
  if (rc != RC::SUCCESS) {
    if (rc != RC::UNIMPLEMENTED) {
      LOG_WARN("failed to create logical plan. rc=%s", strrc(rc));
    }
    return rc;
  }

  ASSERT(logical_operator, "logical operator is null");

  // TODO: unify the RBO and CBO
  rc = rewrite(logical_operator);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to rewrite plan. rc=%s", strrc(rc));
    return rc;
  }

  // TODO: better way
  logical_operator->generate_general_child();
  Optimizer optimizer;
  // TODO: error handle
  unique_ptr<PhysicalOperator> physical_operator;
  if (sql_event->session_event()->session()->use_cascade()) {
    LOG_TRACE("use_cascade is enabled; using physical plan generator with cost-based join choices");
  }
  rc = generate_physical_plan(logical_operator, physical_operator, sql_event->session_event()->session());
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to generate physical plan. rc=%s", strrc(rc));
    return rc;
  }

  sql_event->set_operator(std::move(physical_operator));

  return rc;
}

RC OptimizeStage::optimize(unique_ptr<LogicalOperator> &oper)
{
  // do nothing
  return RC::SUCCESS;
}

RC OptimizeStage::generate_physical_plan(
    unique_ptr<LogicalOperator> &logical_operator, unique_ptr<PhysicalOperator> &physical_operator, Session *session)
{
  RC rc = RC::SUCCESS;
  // 当查询中出现子查询(尤其是聚合子查询)时，向量化通道当前不支持，会在谓词评估时触发未实现或异常。
  // 这里检测逻辑计划中的表达式是否包含子查询，如包含则强制回退到行迭代执行计划，避免崩溃/断连。
  std::function<bool(Expression &)> contains_subquery_expr = [&](Expression &expr) -> bool {
    if (expr.type() == ExprType::SUBQUERY || expr.type() == ExprType::EXISTS) {
      return true;
    }
    bool found = false;
    (void)ExpressionIterator::iterate_child_expr(expr, [&](std::unique_ptr<Expression> &child) -> RC {
      if (child) {
        if (child->type() == ExprType::SUBQUERY || child->type() == ExprType::EXISTS) {
          found = true;
          return RC::SUCCESS;
        }
        if (contains_subquery_expr(*child)) {
          found = true;
          return RC::SUCCESS;
        }
      }
      return RC::SUCCESS;
    });
    return found;
  };

  std::function<bool(LogicalOperator &)> contains_subquery_in_plan = [&](LogicalOperator &op) -> bool {
    // 检查该算子自身挂载的表达式
    for (auto &expr_up : op.expressions()) {
      if (expr_up && contains_subquery_expr(*expr_up)) {
        return true;
      }
    }
    // 递归检查子算子
    for (auto &child : op.children()) {
      if (child && contains_subquery_in_plan(*child)) {
        return true;
      }
    }
    return false;
  };

  const bool plan_has_subquery = contains_subquery_in_plan(*logical_operator);

  // 当存在聚合表达式时，当前向量化执行链路(Chunk Iterator)尚未完整覆盖空输入/NULL 语义与扫描实现，
  // 为避免崩溃或输出异常，这里强制回退到按行执行(Tuple Iterator)。
  std::function<bool(Expression &)> contains_aggregation_expr = [&](Expression &expr) -> bool {
    if (expr.type() == ExprType::AGGREGATION) {
      return true;
    }
    bool found = false;
    (void)ExpressionIterator::iterate_child_expr(expr, [&](std::unique_ptr<Expression> &child) -> RC {
      if (child) {
        if (contains_aggregation_expr(*child)) {
          found = true;
          return RC::SUCCESS;
        }
      }
      return RC::SUCCESS;
    });
    return found;
  };

  std::function<bool(LogicalOperator &)> contains_aggregation_in_plan = [&](LogicalOperator &op) -> bool {
    for (auto &expr_up : op.expressions()) {
      if (expr_up && contains_aggregation_expr(*expr_up)) {
        return true;
      }
    }
    for (auto &child : op.children()) {
      if (child && contains_aggregation_in_plan(*child)) {
        return true;
      }
    }
    return false;
  };

  const bool plan_has_aggregation = contains_aggregation_in_plan(*logical_operator);

  std::function<bool(LogicalOperator &)> can_generate_vectorized_plan = [&](LogicalOperator &op) -> bool {
    switch (op.type()) {
      case LogicalOperatorType::TABLE_GET:
      case LogicalOperatorType::PROJECTION:
      case LogicalOperatorType::GROUP_BY:
      case LogicalOperatorType::EXPLAIN:
        break;
      default:
        return false;
    }

    for (auto &child : op.children()) {
      if (child && !can_generate_vectorized_plan(*child)) {
        return false;
      }
    }
    return true;
  };

  if (!plan_has_subquery && !plan_has_aggregation && session->get_execution_mode() == ExecutionMode::CHUNK_ITERATOR &&
      can_generate_vectorized_plan(*logical_operator)) {
    LOG_TRACE("use chunk iterator");
    session->set_used_chunk_mode(true);
    rc    = physical_plan_generator_.create_vec(*logical_operator, physical_operator, session);
  } else {
    LOG_TRACE("use tuple iterator");
    session->set_used_chunk_mode(false);
    rc = physical_plan_generator_.create(*logical_operator, physical_operator, session);
  }
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create physical operator. rc=%s", strrc(rc));
  }
  return rc;
}

RC OptimizeStage::rewrite(unique_ptr<LogicalOperator> &logical_operator)
{
  RC rc = RC::SUCCESS;

  bool change_made = false;
  do {
    change_made = false;
    rc          = rewriter_.rewrite(logical_operator, change_made);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to do expression rewrite on logical plan. rc=%s", strrc(rc));
      return rc;
    }
  } while (change_made);

  return rc;
}

RC OptimizeStage::create_logical_plan(SQLStageEvent *sql_event, unique_ptr<LogicalOperator> &logical_operator)
{
  Stmt *stmt = sql_event->stmt();
  if (nullptr == stmt) {
    return RC::UNIMPLEMENTED;
  }

  return logical_plan_generator_.create(stmt, logical_operator);
}
