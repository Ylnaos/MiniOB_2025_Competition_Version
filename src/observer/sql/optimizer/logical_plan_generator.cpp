/* Copyright (c) 2023 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2023/08/16.
//

#include "sql/optimizer/logical_plan_generator.h"

#include "common/log/log.h"

#include "sql/operator/calc_logical_operator.h"
#include "sql/operator/delete_logical_operator.h"
#include "sql/operator/explain_logical_operator.h"
#include "sql/operator/insert_logical_operator.h"
#include "sql/operator/update_logical_operator.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/logical_operator.h"
#include "sql/operator/predicate_logical_operator.h"
#include "sql/operator/project_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "sql/operator/group_by_logical_operator.h"
#include "sql/operator/order_by_logical_operator.h"
#include "sql/operator/subquery_logical_operator.h"

#include "sql/stmt/calc_stmt.h"
#include "sql/stmt/delete_stmt.h"
#include "sql/stmt/update_stmt.h"
#include "sql/stmt/explain_stmt.h"
#include "sql/stmt/filter_stmt.h"
#include "sql/stmt/insert_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/stmt.h"

#include "sql/expr/expression_iterator.h"

using namespace std;
using namespace common;

RC LogicalPlanGenerator::create(Stmt *stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  RC rc = RC::SUCCESS;
  switch (stmt->type()) {
    case StmtType::CALC: {
      CalcStmt *calc_stmt = static_cast<CalcStmt *>(stmt);

      rc = create_plan(calc_stmt, logical_operator);
    } break;

    case StmtType::SELECT: {
      SelectStmt *select_stmt = static_cast<SelectStmt *>(stmt);

      rc = create_plan(select_stmt, logical_operator);
    } break;

    case StmtType::INSERT: {
      InsertStmt *insert_stmt = static_cast<InsertStmt *>(stmt);

      rc = create_plan(insert_stmt, logical_operator);
    } break;

    case StmtType::DELETE: {
      DeleteStmt *delete_stmt = static_cast<DeleteStmt *>(stmt);

      rc = create_plan(delete_stmt, logical_operator);
    } break;

    case StmtType::UPDATE: {
      UpdateStmt *update_stmt = static_cast<UpdateStmt *>(stmt);

      rc = create_plan(update_stmt, logical_operator);
    } break;

    case StmtType::EXPLAIN: {
      ExplainStmt *explain_stmt = static_cast<ExplainStmt *>(stmt);

      rc = create_plan(explain_stmt, logical_operator);
    } break;
    default: {
      rc = RC::UNIMPLEMENTED;
    }
  }
  return rc;
}

RC LogicalPlanGenerator::create_plan(CalcStmt *calc_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  logical_operator.reset(new CalcLogicalOperator(std::move(calc_stmt->expressions())));
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(SelectStmt *select_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  // 特殊处理:如果SelectStmt包含inner_view_stmt,说明这是一个FROM子查询
  // 需要先生成内层子查询的逻辑计划,然后用SubqueryLogicalOperator封装
  // 外层查询可以像访问表一样访问子查询结果
  if (select_stmt->inner_view_stmt() != nullptr) {
    LOG_DEBUG("Processing subquery in FROM clause");
    LOG_DEBUG("create_plan: SelectStmt %p has inner_view_stmt %p",
        static_cast<void *>(select_stmt),
        static_cast<void *>(select_stmt->inner_view_stmt()));

    // 1. 生成内层子查询的完整逻辑计划
    unique_ptr<LogicalOperator> inner_logical_oper;
    RC rc = create_plan(select_stmt->inner_view_stmt(), inner_logical_oper);
    if (OB_FAIL(rc)) {
      LOG_WARN("Failed to create logical plan for subquery. rc=%s", strrc(rc));
      return rc;
    }

    // 2. 用SubqueryLogicalOperator封装内层逻辑计划
    auto subquery_oper = make_unique<SubqueryLogicalOperator>();
    subquery_oper->set_subquery_plan(std::move(inner_logical_oper));

    // 3. 将SubqueryLogicalOperator作为数据源，构建外层查询
    // 外层可能包含GROUP BY、WHERE等操作
    unique_ptr<LogicalOperator> last_oper = std::move(subquery_oper);

    // 构建外层的GROUP BY逻辑（如果有聚合函数）
    unique_ptr<LogicalOperator> group_by_oper;
    rc = create_group_by_plan(select_stmt, group_by_oper);
    if (OB_FAIL(rc)) {
      LOG_WARN("Failed to create group by plan for outer query. rc=%s", strrc(rc));
      return rc;
    }

    if (group_by_oper) {
      LOG_DEBUG("Outer query has GroupBy operator, adding SubqueryLogicalOperator as child");
      group_by_oper->add_child(std::move(last_oper));
      last_oper = std::move(group_by_oper);
    } else {
      // 检查外层查询是否包含聚合表达式
      // 如果包含聚合表达式但 GroupBy 算子未创建,说明有 bug
      bool has_agg = false;
      for (const auto &expr : select_stmt->query_expressions()) {
        if (expr && expr->type() == ExprType::AGGREGATION) {
          has_agg = true;
          break;
        }
      }

      if (has_agg) {
        // 有聚合表达式但没有创建 GroupBy 算子,这是严重错误
        LOG_ERROR("CRITICAL: Outer query has aggregate expressions but NO GroupBy operator! "
                  "query_expressions size=%d",
                  static_cast<int>(select_stmt->query_expressions().size()));
        for (size_t i = 0; i < select_stmt->query_expressions().size(); i++) {
          const auto &expr = select_stmt->query_expressions()[i];
          LOG_ERROR("  expr[%d]: type=%d name=%s",
              static_cast<int>(i),
              static_cast<int>(expr->type()),
              expr->name() ? expr->name() : "(null)");
        }
        return RC::INTERNAL;
      } else {
        // 外层查询没有聚合表达式(如 SELECT * FROM view),不需要 GroupBy 算子
        LOG_DEBUG("Outer query has NO aggregation (e.g., SELECT * FROM view), no GroupBy needed");
      }
    }

    // 添加外层的PROJECT算子
    unique_ptr<LogicalOperator> project_oper = make_unique<ProjectLogicalOperator>(std::move(select_stmt->query_expressions()));
    project_oper->add_child(std::move(last_oper));

    // 设置LIMIT（如果有）
    static_cast<ProjectLogicalOperator*>(project_oper.get())->set_limit(select_stmt->limit());

    logical_operator = std::move(project_oper);
    return RC::SUCCESS;
  }

  // 正常的SelectStmt处理流程
  unique_ptr<LogicalOperator> *last_oper = nullptr;

  unique_ptr<LogicalOperator> table_oper(nullptr);
  last_oper = &table_oper;
  unique_ptr<LogicalOperator> predicate_oper;
  unique_ptr<LogicalOperator> having_pred;

  LOG_ERROR("[TRACE] Creating logical plan for SelectStmt: filter_stmt=%s, where_expr=%s, tables=%zu",
      select_stmt->filter_stmt() ? "EXISTS" : "NULL",
      select_stmt->where_expr() ? "EXISTS" : "NULL",
      select_stmt->tables().size());

  RC rc = create_plan(select_stmt->filter_stmt(), predicate_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create predicate logical plan. rc=%s", strrc(rc));
    return rc;
  }

  LOG_ERROR("[TRACE] After create_plan(filter_stmt): predicate_oper=%s",
      predicate_oper ? "EXISTS" : "NULL");

  // [TRACE] 检查filter_stmt的具体内容
  if (select_stmt->filter_stmt()) {
    LOG_ERROR("[TRACE] filter_stmt details: filter_units=%zu",
        select_stmt->filter_stmt()->filter_units().size());
    for (size_t i = 0; i < select_stmt->filter_stmt()->filter_units().size(); i++) {
      const auto *unit = select_stmt->filter_stmt()->filter_units()[i];
      LOG_ERROR("[TRACE]   filter_unit[%zu]: comp=%d", i, static_cast<int>(unit->comp()));
    }
  }

  // 同时支持两类 WHERE：
  // - 传统 AND 链 (filter_stmt)
  // - 布尔表达式（支持 AND/OR），通常也用于 JOIN ... ON 的展开
  if (select_stmt->where_expr()) {
    auto extra_pred = make_unique<PredicateLogicalOperator>(std::move(select_stmt->where_expr()));
    if (predicate_oper) {
      // 叠加一个谓词算子，整体等价于 AND 组合
      extra_pred->add_child(std::move(predicate_oper));
      predicate_oper = std::move(extra_pred);
    } else {
      predicate_oper = std::move(extra_pred);
    }
  }

  const vector<Table *> &tables = select_stmt->tables();
  const vector<string> &aliases = select_stmt->table_aliases();
  for (size_t idx = 0; idx < tables.size(); idx++) {
    Table *table = tables[idx];

    auto table_get_ptr = new TableGetLogicalOperator(table, ReadWriteMode::READ_ONLY);
    if (idx < aliases.size()) {
      table_get_ptr->set_alias(aliases[idx]);
    }
    unique_ptr<LogicalOperator> table_get_oper(table_get_ptr);
    if (table_oper == nullptr) {
      table_oper = std::move(table_get_oper);
    } else {
      JoinLogicalOperator *join_oper = new JoinLogicalOperator;
      join_oper->add_child(std::move(table_oper));
      join_oper->add_child(std::move(table_get_oper));
      table_oper = unique_ptr<LogicalOperator>(join_oper);
    }
  }


  LOG_ERROR("[TRACE] Before connecting predicate: predicate_oper=%s, table_oper=%s",
      predicate_oper ? "EXISTS" : "NULL",
      table_oper ? "EXISTS" : "NULL");

  if (predicate_oper) {
    if (*last_oper) {
      predicate_oper->add_child(std::move(*last_oper));
      LOG_ERROR("[TRACE] Connected predicate_oper with table_oper as child");
    }

    last_oper = &predicate_oper;
  }

  // 针对 SELECT 无 FROM 且无其他算子（仅常量/表达式）的特殊优化：直接使用 CALC 输出一行结果
  if (tables.empty() && !predicate_oper && select_stmt->group_by().empty() && select_stmt->order_by().empty()) {
    logical_operator.reset(new CalcLogicalOperator(std::move(select_stmt->query_expressions())));
    return RC::SUCCESS;
  }

  unique_ptr<LogicalOperator> group_by_oper;
  rc = create_group_by_plan(select_stmt, group_by_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create group by logical plan. rc=%s", strrc(rc));
    return rc;
  }

  LOG_ERROR("[TRACE] After create_group_by_plan: group_by_oper=%s, current_last_oper=%s",
      group_by_oper ? "EXISTS" : "NULL",
      *last_oper ? "EXISTS" : "NULL");

  if (group_by_oper) {
    if (*last_oper) {
      group_by_oper->add_child(std::move(*last_oper));
      LOG_ERROR("[TRACE] Connected group_by_oper with last_oper as child");
    }

    last_oper = &group_by_oper;
  }

  // HAVING（在 GROUP BY 之后、ORDER BY 之前）
  if (select_stmt->having_expr()) {
    having_pred = make_unique<PredicateLogicalOperator>(std::move(select_stmt->having_expr()));
    if (*last_oper) {
      having_pred->add_child(std::move(*last_oper));
    }
    last_oper = &having_pred;
  }

  // ORDER BY
  unique_ptr<LogicalOperator> order_by_oper;
  if (!select_stmt->order_by().empty()) {
    order_by_oper = make_unique<OrderByLogicalOperator>(std::move(select_stmt->order_by()));
    if (*last_oper) {
      order_by_oper->add_child(std::move(*last_oper));
    }
    last_oper = &order_by_oper;
  }

  unique_ptr<LogicalOperator> project_oper = make_unique<ProjectLogicalOperator>(std::move(select_stmt->query_expressions()));
  if (*last_oper) {
    project_oper->add_child(std::move(*last_oper));
  }

  // 设置LIMIT
  static_cast<ProjectLogicalOperator*>(project_oper.get())->set_limit(select_stmt->limit());

  last_oper = &project_oper;

  logical_operator = std::move(*last_oper);
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(FilterStmt *filter_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  // 无 WHERE 条件时，filter_stmt 可能为 nullptr，此时直接返回空的谓词算子
  logical_operator.reset();
  if (filter_stmt == nullptr) {
    return RC::SUCCESS;
  }

  RC                                  rc = RC::SUCCESS;
  vector<unique_ptr<Expression>> cmp_exprs;
  const vector<FilterUnit *>    &filter_units = filter_stmt->filter_units();
  for (const FilterUnit *filter_unit : filter_units) {
    const FilterObj &filter_obj_left  = filter_unit->left();
    const FilterObj &filter_obj_right = filter_unit->right();

    unique_ptr<Expression> left;
    unique_ptr<Expression> right;

    // 处理左侧操作数
    if (filter_obj_left.is_attr == -1 && filter_obj_left.expr) {
      // 使用表达式
      left = filter_obj_left.expr->copy();
    } else if (filter_obj_left.is_attr) {
      left = make_unique<FieldExpr>(filter_obj_left.field);
    } else {
      left = make_unique<ValueExpr>(filter_obj_left.value);
    }

    // 处理右侧操作数
    if (filter_obj_right.is_attr == -1 && filter_obj_right.expr) {
      // 使用表达式
      right = filter_obj_right.expr->copy();
    } else if (filter_obj_right.is_attr) {
      right = make_unique<FieldExpr>(filter_obj_right.field);
    } else {
      right = make_unique<ValueExpr>(filter_obj_right.value);
    }

    if (filter_unit->comp() != IN_OP && filter_unit->comp() != NOT_IN_OP &&
        filter_unit->comp() != IS_NULL && filter_unit->comp() != IS_NOT_NULL &&
        // 避免对子查询做隐式转换
        left->type() != ExprType::SUBQUERY && right->type() != ExprType::SUBQUERY &&
        // 避免对含 NULL 的比较做隐式转换（与 NULL 的比较应直接交由执行期按 UNKNOWN 处理）
        left->value_type() != AttrType::NULLS && right->value_type() != AttrType::NULLS &&
        // 类型确实不同，且不包含 NULL
        left->value_type() != right->value_type()) {
      auto left_to_right_cost = implicit_cast_cost(left->value_type(), right->value_type());
      auto right_to_left_cost = implicit_cast_cost(right->value_type(), left->value_type());
      if (left_to_right_cost <= right_to_left_cost && left_to_right_cost != INT32_MAX) {
        ExprType left_type = left->type();
        auto cast_expr = make_unique<CastExpr>(std::move(left), right->value_type());
        if (left_type == ExprType::VALUE) {
          Value left_val;
          if (OB_FAIL(rc = cast_expr->try_get_value(left_val)))
          {
            LOG_WARN("failed to get value from left child", strrc(rc));
            return rc;
          }
          left = make_unique<ValueExpr>(left_val);
        } else {
          left = std::move(cast_expr);
        }
      } else if (right_to_left_cost < left_to_right_cost && right_to_left_cost != INT32_MAX) {
        ExprType right_type = right->type();
        auto cast_expr = make_unique<CastExpr>(std::move(right), left->value_type());
        if (right_type == ExprType::VALUE) {
          Value right_val;
          if (OB_FAIL(rc = cast_expr->try_get_value(right_val)))
          {
            LOG_WARN("failed to get value from right child", strrc(rc));
            return rc;
          }
          right = make_unique<ValueExpr>(right_val);
        } else {
          right = std::move(cast_expr);
        }

      } else {
        rc = RC::UNSUPPORTED;
        LOG_WARN("unsupported cast from %s to %s", attr_type_to_string(left->value_type()), attr_type_to_string(right->value_type()));
        return rc;
      }
    }

    if (filter_unit->comp() == IN_OP || filter_unit->comp() == NOT_IN_OP) {
      bool not_in = (filter_unit->comp() == NOT_IN_OP);
      InExpr *in_expr = new InExpr(std::move(left), std::move(right), not_in);
      cmp_exprs.emplace_back(in_expr);
    } else {
      ComparisonExpr *cmp_expr = new ComparisonExpr(filter_unit->comp(), std::move(left), std::move(right));
      cmp_exprs.emplace_back(cmp_expr);
    }
  }

  unique_ptr<PredicateLogicalOperator> predicate_oper;
  if (!cmp_exprs.empty()) {
    unique_ptr<ConjunctionExpr> conjunction_expr(new ConjunctionExpr(ConjunctionExpr::Type::AND, cmp_exprs));
    predicate_oper = unique_ptr<PredicateLogicalOperator>(new PredicateLogicalOperator(std::move(conjunction_expr)));
  }

  logical_operator = std::move(predicate_oper);
  return rc;
}

int LogicalPlanGenerator::implicit_cast_cost(AttrType from, AttrType to)
{
  if (from == to) {
    return 0;
  }
  return DataType::type_instance(from)->cast_cost(to);
}

RC LogicalPlanGenerator::create_plan(InsertStmt *insert_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  Table *table = insert_stmt->table();

  InsertLogicalOperator *insert_operator = new InsertLogicalOperator(table, insert_stmt->values_rows());
  logical_operator.reset(insert_operator);
  return RC::SUCCESS;
}

RC LogicalPlanGenerator::create_plan(DeleteStmt *delete_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  Table                      *table       = delete_stmt->table();
  FilterStmt                 *filter_stmt = delete_stmt->filter_stmt();
  unique_ptr<LogicalOperator> table_get_oper(new TableGetLogicalOperator(table, ReadWriteMode::READ_WRITE));

  unique_ptr<LogicalOperator> predicate_oper;

  RC rc = create_plan(filter_stmt, predicate_oper);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  unique_ptr<LogicalOperator> delete_oper(new DeleteLogicalOperator(table));

  if (predicate_oper) {
    predicate_oper->add_child(std::move(table_get_oper));
    delete_oper->add_child(std::move(predicate_oper));
  } else {
    delete_oper->add_child(std::move(table_get_oper));
  }

  logical_operator = std::move(delete_oper);
  return rc;
}

RC LogicalPlanGenerator::create_plan(UpdateStmt *update_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  Table                            *table        = update_stmt->table();
  FilterStmt                       *filter_stmt  = update_stmt->filter_stmt();
  const vector<const FieldMeta *>  &field_metas  = update_stmt->field_metas();
  const vector<Value>              &values       = update_stmt->values();

  // 转换表达式为unique_ptr
  vector<unique_ptr<Expression>> value_expressions;
  for (auto expr : update_stmt->value_expressions()) {
    if (expr) {
      value_expressions.push_back(unique_ptr<Expression>(expr->copy()));
    } else {
      value_expressions.push_back(nullptr);
    }
  }

  unique_ptr<LogicalOperator> table_get_oper(new TableGetLogicalOperator(table, ReadWriteMode::READ_WRITE));

  unique_ptr<LogicalOperator> predicate_oper;

  RC rc = create_plan(filter_stmt, predicate_oper);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  unique_ptr<LogicalOperator> update_oper(new UpdateLogicalOperator(table, field_metas, values, value_expressions));

  // 将 SET 子句中的表达式也附加到逻辑算子的 expressions_ 中，
  // 便于后续阶段做通用的表达式扫描（例如用于检测子查询从而禁用向量化）。
  for (auto &expr_up : value_expressions) {
    if (expr_up) {
      update_oper->add_expressions(expr_up->copy());
    }
  }

  if (predicate_oper) {
    predicate_oper->add_child(std::move(table_get_oper));
    update_oper->add_child(std::move(predicate_oper));
  } else {
    update_oper->add_child(std::move(table_get_oper));
  }

  logical_operator = std::move(update_oper);
  return rc;
}

RC LogicalPlanGenerator::create_plan(ExplainStmt *explain_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  unique_ptr<LogicalOperator> child_oper;

  Stmt *child_stmt = explain_stmt->child();

  RC rc = create(child_stmt, child_oper);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create explain's child operator. rc=%s", strrc(rc));
    return rc;
  }

  logical_operator = unique_ptr<LogicalOperator>(new ExplainLogicalOperator);
  logical_operator->add_child(std::move(child_oper));
  return rc;
}

RC LogicalPlanGenerator::create_group_by_plan(SelectStmt *select_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  vector<unique_ptr<Expression>> &group_by_expressions = select_stmt->group_by();
  vector<Expression *> aggregate_expressions;
  vector<unique_ptr<Expression>> &query_expressions = select_stmt->query_expressions();
  LOG_DEBUG("create_group_by_plan: processing %d query expressions",
      static_cast<int>(query_expressions.size()));
  // [DEBUG] 记录每个query expression的详细信息
  for (size_t i = 0; i < query_expressions.size(); i++) {
    const auto &expr = query_expressions[i];
    if (expr) {
      LOG_DEBUG("  [DEBUG] query_expr[%zu]: type=%d (%s) name='%s'",
          i,
          static_cast<int>(expr->type()),
          expr->type() == ExprType::AGGREGATION ? "AGGREGATION" :
          expr->type() == ExprType::UNBOUND_AGGREGATION ? "UNBOUND_AGGREGATION" :
          expr->type() == ExprType::FIELD ? "FIELD" :
          expr->type() == ExprType::VALUE ? "VALUE" :
          expr->type() == ExprType::ARITHMETIC ? "ARITHMETIC" : "OTHER",
          expr->name() ? expr->name() : "(null)");
    } else {
      LOG_DEBUG("  [DEBUG] query_expr[%zu]: nullptr", i);
    }
  }
  function<RC(unique_ptr<Expression>&)> collector = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
      if (expr->type() == ExprType::AGGREGATION) {
        bool exists = false;
        for (auto *ag : aggregate_expressions) {
          if (expr->equal(*ag)) {
            // 发现重复的聚合表达式，设置pos指向已存在的聚合表达式位置
            // 这样在后续求值时可以正确找到聚合结果
            expr->set_pos(ag->pos());
            LOG_DEBUG("aggregate collector dedup: expr=%s equals existing=%s at pos=%d",
                expr->name(), ag->name(), ag->pos());
            exists = true;
            break;
          }
        }
        if (!exists) {
          expr->set_pos(aggregate_expressions.size() + group_by_expressions.size());
          aggregate_expressions.push_back(expr.get());
          LOG_DEBUG("create_group_by_plan: added aggregation expression '%s' at pos=%d (aggregate count=%d)",
              expr->name() ? expr->name() : "",
              expr->pos(),
              static_cast<int>(aggregate_expressions.size()));
        }
      }
      rc = ExpressionIterator::iterate_child_expr(*expr, collector);
      return rc;
  };

  function<RC(unique_ptr<Expression>&)> bind_group_by_expr = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
    for (size_t i = 0; i < group_by_expressions.size(); i++) {
      auto &group_by = group_by_expressions[i];
      if (expr->type() == ExprType::AGGREGATION) {
        break;
      } else if (expr->equal(*group_by)) {
        expr->set_pos(i);
        continue;
      } else {
        rc = ExpressionIterator::iterate_child_expr(*expr, bind_group_by_expr);
      }
    }
    return rc;
  };

 bool found_unbound_column = false;
  function<RC(unique_ptr<Expression>&)> find_unbound_column = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
    if (expr->type() == ExprType::AGGREGATION) {
      // do nothing
    } else if (expr->pos() != -1) {
      // do nothing
    } else if (expr->type() == ExprType::FIELD) {
      found_unbound_column = true;
    }else {
      rc = ExpressionIterator::iterate_child_expr(*expr, find_unbound_column);
    }
    return rc;
  };
  

  for (unique_ptr<Expression> &expression : query_expressions) {
    bind_group_by_expr(expression);
  }

  for (unique_ptr<Expression> &expression : query_expressions) {
    find_unbound_column(expression);
  }

  // collect all aggregate expressions（来自 SELECT 列与 HAVING 表达式）
  LOG_DEBUG("create_group_by_plan: collecting aggregate expressions from %d query expressions",
      static_cast<int>(query_expressions.size()));
  for (size_t idx = 0; idx < query_expressions.size(); idx++) {
    const auto &expression = query_expressions[idx];
    LOG_DEBUG("  [%d] before collect: type=%d (%s) name=%s",
        static_cast<int>(idx),
        static_cast<int>(expression->type()),
        expression->type() == ExprType::AGGREGATION ? "AGGREGATION" :
        expression->type() == ExprType::UNBOUND_AGGREGATION ? "UNBOUND_AGGREGATION" :
        expression->type() == ExprType::FIELD ? "FIELD" :
        expression->type() == ExprType::VALUE ? "VALUE" : "OTHER",
        expression->name() ? expression->name() : "(null)");
    collector(query_expressions[idx]);
  }
  if (select_stmt->having_expr()) {
    LOG_DEBUG("  collecting from HAVING expression");
    collector(select_stmt->having_expr());
  }

  LOG_DEBUG("create_group_by_plan: after collection: group_by count=%d aggregate count=%d",
      static_cast<int>(group_by_expressions.size()),
      static_cast<int>(aggregate_expressions.size()));
  for (size_t i = 0; i < aggregate_expressions.size(); i++) {
    LOG_DEBUG("  aggregate[%d]: name=%s pos=%d",
        static_cast<int>(i),
        aggregate_expressions[i]->name() ? aggregate_expressions[i]->name() : "(null)",
        aggregate_expressions[i]->pos());
  }

  if (group_by_expressions.empty() && aggregate_expressions.empty()) {
    // 既没有group by也没有聚合函数，不需要group by
    LOG_DEBUG("[DEBUG] create_group_by_plan: NO GROUPING OR AGGREGATION, returning empty plan - THIS IS THE PROBLEM!");
    LOG_DEBUG("[DEBUG] This means GroupBy operator will NOT be created!");
    return RC::SUCCESS;
  }

  if (found_unbound_column) {
    LOG_WARN("column must appear in the GROUP BY clause or must be part of an aggregate function");
    return RC::INVALID_ARGUMENT;
  }

  // 如果只需要聚合，但是没有group by 语句，需要生成一个空的group by 语句

  LOG_DEBUG("create_group_by_plan: creating GroupByLogicalOperator with %d group_by expressions and %d aggregate expressions",
      static_cast<int>(group_by_expressions.size()),
      static_cast<int>(aggregate_expressions.size()));

  auto group_by_oper = make_unique<GroupByLogicalOperator>(std::move(group_by_expressions),
                                                           std::move(aggregate_expressions));
  logical_operator = std::move(group_by_oper);
  return RC::SUCCESS;
}
