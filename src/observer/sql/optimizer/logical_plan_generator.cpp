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
#include "sql/operator/union_logical_operator.h"
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
    unique_ptr<LogicalOperator> predicate_oper;
    unique_ptr<LogicalOperator> having_pred;

    rc = create_plan(select_stmt->filter_stmt(), predicate_oper);
    if (OB_FAIL(rc)) {
      LOG_WARN("Failed to create predicate plan for outer query. rc=%s", strrc(rc));
      return rc;
    }

    if (select_stmt->where_expr()) {
      auto extra_pred = make_unique<PredicateLogicalOperator>(std::move(select_stmt->where_expr()));
      if (predicate_oper) {
        extra_pred->add_child(std::move(predicate_oper));
        predicate_oper = std::move(extra_pred);
      } else {
        predicate_oper = std::move(extra_pred);
      }
    }

    if (predicate_oper) {
      predicate_oper->add_child(std::move(last_oper));
      last_oper = std::move(predicate_oper);
    }

    // 构建外层的GROUP BY逻辑（如果有聚合函数）
    unique_ptr<LogicalOperator> group_by_oper;
    rc = create_group_by_plan(select_stmt, group_by_oper);
    if (OB_FAIL(rc)) {
      LOG_WARN("Failed to create group by plan for outer query. rc=%s", strrc(rc));
      return rc;
    }

    if (group_by_oper) {
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
        return RC::INTERNAL;
      } else {
        // 外层查询没有聚合表达式(如 SELECT * FROM view),不需要 GroupBy 算子
      }
    }

    if (select_stmt->having_expr()) {
      having_pred = make_unique<PredicateLogicalOperator>(std::move(select_stmt->having_expr()));
      having_pred->add_child(std::move(last_oper));
      last_oper = std::move(having_pred);
    }

    unique_ptr<LogicalOperator> order_by_oper;
    if (!select_stmt->order_by().empty()) {
      order_by_oper = make_unique<OrderByLogicalOperator>(std::move(select_stmt->order_by()));
      order_by_oper->add_child(std::move(last_oper));
      last_oper = std::move(order_by_oper);
    }

    // 添加外层的PROJECT算子
    unique_ptr<LogicalOperator> project_oper = make_unique<ProjectLogicalOperator>(std::move(select_stmt->query_expressions()));
    project_oper->add_child(std::move(last_oper));

    // 设置LIMIT（如果有）
    static_cast<ProjectLogicalOperator*>(project_oper.get())->set_limit(select_stmt->limit());

    logical_operator = std::move(project_oper);
    return RC::SUCCESS;
  }

  if (!select_stmt->set_operations().empty()) {
    unique_ptr<LogicalOperator> current_plan;
    RC rc = create_single_select_plan(select_stmt, current_plan);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to build base plan for union. rc=%s", strrc(rc));
      return rc;
    }

    for (const auto &set_op : select_stmt->set_operations()) {
      if (set_op.stmt == nullptr) {
        LOG_WARN("union branch stmt is null");
        return RC::INVALID_ARGUMENT;
      }
      unique_ptr<LogicalOperator> right_plan;
      rc = create_plan(set_op.stmt.get(), right_plan);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to build union branch plan. rc=%s", strrc(rc));
        return rc;
      }

      auto union_oper = make_unique<UnionLogicalOperator>(set_op.type == SetOperatorType::UNION);
      union_oper->add_child(std::move(current_plan));
      union_oper->add_child(std::move(right_plan));
      current_plan = std::move(union_oper);
    }

    logical_operator = std::move(current_plan);
    return RC::SUCCESS;
  }

  return create_single_select_plan(select_stmt, logical_operator);
}

RC LogicalPlanGenerator::create_single_select_plan(
    SelectStmt *select_stmt, unique_ptr<LogicalOperator> &logical_operator)
{
  unique_ptr<LogicalOperator> *last_oper = nullptr;

  unique_ptr<LogicalOperator> table_oper(nullptr);
  last_oper = &table_oper;
  unique_ptr<LogicalOperator> predicate_oper;
  unique_ptr<LogicalOperator> having_pred;
  const vector<Table *> &tables = select_stmt->tables();

  RC rc = create_plan(select_stmt->filter_stmt(), predicate_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create predicate logical plan. rc=%s", strrc(rc));
    return rc;
  }

  if (select_stmt->where_expr()) {
    auto extra_pred = make_unique<PredicateLogicalOperator>(std::move(select_stmt->where_expr()));
    if (predicate_oper) {
      extra_pred->add_child(std::move(predicate_oper));
      predicate_oper = std::move(extra_pred);
    } else {
      predicate_oper = std::move(extra_pred);
    }
  }

  const vector<SelectStmt::FromItem> &from_items = select_stmt->from_items();
  for (const auto &item : from_items) {
    unique_ptr<LogicalOperator> source_oper;
    if (item.type == SelectStmt::FromItem::Type::TABLE) {
      auto table_get_ptr = new TableGetLogicalOperator(item.table, ReadWriteMode::READ_ONLY);
      if (!item.alias.empty()) {
        table_get_ptr->set_alias(item.alias);
      }
      source_oper.reset(table_get_ptr);
    } else {
      if (item.derived == nullptr) {
        LOG_WARN("derived table item without SelectStmt");
        return RC::INTERNAL;
      }
      unique_ptr<LogicalOperator> derived_plan;
      RC rc = create_plan(item.derived, derived_plan);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to create logical plan for derived table. alias=%s rc=%s",
            item.alias.c_str(), strrc(rc));
        return rc;
      }
      auto subquery_oper = make_unique<SubqueryLogicalOperator>();
      subquery_oper->set_subquery_plan(std::move(derived_plan));
      source_oper = std::move(subquery_oper);
    }

    if (table_oper == nullptr) {
      table_oper = std::move(source_oper);
    } else {
      auto *join_oper = new JoinLogicalOperator;
      join_oper->add_child(std::move(table_oper));
      join_oper->add_child(std::move(source_oper));
      table_oper.reset(join_oper);
    }
  }

  if (predicate_oper) {
    if (*last_oper) {
      predicate_oper->add_child(std::move(*last_oper));
    }
    last_oper = &predicate_oper;
  }

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

  if (group_by_oper) {
    if (*last_oper) {
      group_by_oper->add_child(std::move(*last_oper));
    }
    last_oper = &group_by_oper;
  }

  if (select_stmt->having_expr()) {
    having_pred = make_unique<PredicateLogicalOperator>(std::move(select_stmt->having_expr()));
    if (*last_oper) {
      having_pred->add_child(std::move(*last_oper));
    }
    last_oper = &having_pred;
  }

  unique_ptr<LogicalOperator> order_by_oper;
  if (!select_stmt->order_by().empty()) {
    order_by_oper = make_unique<OrderByLogicalOperator>(std::move(select_stmt->order_by()));
    if (*last_oper) {
      order_by_oper->add_child(std::move(*last_oper));
    }
    last_oper = &order_by_oper;
  }

  unique_ptr<LogicalOperator> project_oper =
      make_unique<ProjectLogicalOperator>(std::move(select_stmt->query_expressions()));
  if (*last_oper) {
    project_oper->add_child(std::move(*last_oper));
  }

  static_cast<ProjectLogicalOperator *>(project_oper.get())->set_limit(select_stmt->limit());

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
        // 避免对子查询/EXISTS 做隐式转换
        left->type() != ExprType::SUBQUERY && right->type() != ExprType::SUBQUERY &&
        left->type() != ExprType::EXISTS && right->type() != ExprType::EXISTS &&
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
    unique_ptr<ConjunctionExpr> conjunction_expr(new ConjunctionExpr(ConjunctionExpr::Type::AND, std::move(cmp_exprs)));
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
  InsertLogicalOperator *insert_operator = new InsertLogicalOperator(insert_stmt->insert_tasks());
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
  function<RC(unique_ptr<Expression>&)> collector = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
      if (expr->type() == ExprType::AGGREGATION) {
        bool exists = false;
        for (auto *ag : aggregate_expressions) {
          if (expr->equal(*ag)) {
            // 发现重复的聚合表达式，设置pos指向已存在的聚合表达式位置
            // 这样在后续求值时可以正确找到聚合结果
            expr->set_pos(ag->pos());
            exists = true;
            break;
          }
        }
        if (!exists) {
          expr->set_pos(aggregate_expressions.size() + group_by_expressions.size());
          aggregate_expressions.push_back(expr.get());
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
    } else {
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
  for (size_t idx = 0; idx < query_expressions.size(); idx++) {
    collector(query_expressions[idx]);
  }
  if (select_stmt->having_expr()) {
    collector(select_stmt->having_expr());
  }

  if (group_by_expressions.empty() && aggregate_expressions.empty()) {
    // 既没有group by也没有聚合函数，不需要group by
    return RC::SUCCESS;
  }

  if (found_unbound_column) {
    LOG_WARN("column must appear in the GROUP BY clause or must be part of an aggregate function");
    return RC::INVALID_ARGUMENT;
  }

  // 如果只需要聚合，但是没有group by 语句，需要生成一个空的group by 语句

  auto group_by_oper = make_unique<GroupByLogicalOperator>(std::move(group_by_expressions),
                                                           std::move(aggregate_expressions));
  logical_operator = std::move(group_by_oper);
  return RC::SUCCESS;
}
