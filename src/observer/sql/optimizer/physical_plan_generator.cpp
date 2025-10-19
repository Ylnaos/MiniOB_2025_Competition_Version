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
// Created by Wangyunlai on 2022/12/14.
//

#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "session/session.h"
#include "sql/operator/aggregate_vec_physical_operator.h"
#include "sql/operator/calc_logical_operator.h"
#include "sql/operator/calc_physical_operator.h"
#include "sql/operator/delete_logical_operator.h"
#include "sql/operator/delete_physical_operator.h"
#include "sql/operator/update_logical_operator.h"
#include "sql/operator/update_physical_operator.h"
#include "sql/operator/explain_logical_operator.h"
#include "sql/operator/explain_physical_operator.h"
#include "sql/operator/expr_vec_physical_operator.h"
#include "sql/operator/group_by_vec_physical_operator.h"
#include "sql/operator/hash_join_physical_operator.h"
#include "sql/operator/index_scan_physical_operator.h"
#include "sql/operator/insert_logical_operator.h"
#include "sql/operator/insert_physical_operator.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/nested_loop_join_physical_operator.h"
#include "sql/operator/predicate_logical_operator.h"
#include "sql/operator/predicate_physical_operator.h"
#include "sql/operator/project_logical_operator.h"
#include "sql/operator/project_physical_operator.h"
#include "sql/operator/project_vec_physical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "sql/operator/table_scan_physical_operator.h"
#include "sql/operator/group_by_logical_operator.h"
#include "sql/operator/group_by_physical_operator.h"
#include "sql/operator/hash_group_by_physical_operator.h"
#include "sql/operator/scalar_group_by_physical_operator.h"
#include "sql/operator/order_by_logical_operator.h"
#include "sql/operator/order_by_physical_operator.h"
#include "sql/operator/limit_physical_operator.h"
#include "sql/operator/subquery_logical_operator.h"
#include "sql/operator/subquery_physical_operator.h"
#include "sql/operator/table_scan_vec_physical_operator.h"
#include "sql/operator/vector_index_scan_logical_operator.h"
#include "sql/operator/vector_index_scan_physical_operator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "storage/index/index.h"
#include "storage/index/ivfflat_index.h"

using namespace std;

RC PhysicalPlanGenerator::create(LogicalOperator &logical_operator, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;

  switch (logical_operator.type()) {
    case LogicalOperatorType::CALC: {
      return create_plan(static_cast<CalcLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::TABLE_GET: {
      return create_plan(static_cast<TableGetLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::PREDICATE: {
      return create_plan(static_cast<PredicateLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::PROJECTION: {
      return create_plan(static_cast<ProjectLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::INSERT: {
      return create_plan(static_cast<InsertLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::DELETE: {
      return create_plan(static_cast<DeleteLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::UPDATE: {
      return create_plan(static_cast<UpdateLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::EXPLAIN: {
      return create_plan(static_cast<ExplainLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::JOIN: {
      return create_plan(static_cast<JoinLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::GROUP_BY: {
      return create_plan(static_cast<GroupByLogicalOperator &>(logical_operator), oper, session);
    } break;
    case LogicalOperatorType::ORDER_BY: {
      // create physical plan for order by
      auto &order_logical = static_cast<OrderByLogicalOperator &>(logical_operator);
      // 递归创建子计划
      ASSERT(order_logical.children().size() == 1, "order by operator should have 1 child");
      unique_ptr<PhysicalOperator> child_physical_oper;
      RC rc = create(*order_logical.children().front(), child_physical_oper, session);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to create child physical operator of order by. rc=%s", strrc(rc));
        return rc;
      }
      // 移动表达式到物理算子
      vector<OrderByPhysicalOperator::OrderItem> items;
      for (auto &p : order_logical.order_by_items()) {
        items.emplace_back(std::move(p));
      }
      auto order_phy = make_unique<OrderByPhysicalOperator>(std::move(items));
      order_phy->add_child(std::move(child_physical_oper));
      oper = std::move(order_phy);
      return RC::SUCCESS;
    } break;

    case LogicalOperatorType::VECTOR_INDEX_SCAN: {
      return create_plan(static_cast<VectorIndexScanLogicalOperator &>(logical_operator), oper, session);
    } break;

    case LogicalOperatorType::SUBQUERY: {
      return create_plan(static_cast<SubqueryLogicalOperator &>(logical_operator), oper, session);
    } break;

    default: {
      ASSERT(false, "unknown logical operator type");
      return RC::INVALID_ARGUMENT;
    }
  }
  return rc;
}

RC PhysicalPlanGenerator::create_vec(LogicalOperator &logical_operator, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;

  switch (logical_operator.type()) {
    case LogicalOperatorType::TABLE_GET: {
      return create_vec_plan(static_cast<TableGetLogicalOperator &>(logical_operator), oper, session);
    } break;
    case LogicalOperatorType::PROJECTION: {
      return create_vec_plan(static_cast<ProjectLogicalOperator &>(logical_operator), oper, session);
    } break;
    case LogicalOperatorType::GROUP_BY: {
      return create_vec_plan(static_cast<GroupByLogicalOperator &>(logical_operator), oper, session);
    } break;
    case LogicalOperatorType::EXPLAIN: {
      return create_vec_plan(static_cast<ExplainLogicalOperator &>(logical_operator), oper, session);
    } break;
    default: {
      LOG_WARN("unknown logical operator type: %d", logical_operator.type());
      return RC::INVALID_ARGUMENT;
    }
  }
  return rc;
}

RC PhysicalPlanGenerator::create_plan(TableGetLogicalOperator &table_get_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<Expression>> &predicates = table_get_oper.predicates();
  // 看看是否有可以用于索引查找的表达式
  Table *table = table_get_oper.table();

  Index     *index      = nullptr;
  ValueExpr *value_expr = nullptr;
  for (auto &expr : predicates) {
    if (expr->type() == ExprType::COMPARISON) {
      auto comparison_expr = static_cast<ComparisonExpr *>(expr.get());
      // 简单处理，就找等值查询
      if (comparison_expr->comp() != EQUAL_TO && comparison_expr->comp() != NOT_EQUAL) {
        continue;
      }

      unique_ptr<Expression> &left_expr  = comparison_expr->left();
      unique_ptr<Expression> &right_expr = comparison_expr->right();
      // 左右比较的一边最少是一个值
      if (left_expr->type() != ExprType::VALUE && right_expr->type() != ExprType::VALUE) {
        continue;
      }

      FieldExpr *field_expr = nullptr;
      if (left_expr->type() == ExprType::FIELD) {
        ASSERT(right_expr->type() == ExprType::VALUE, "right expr should be a value expr while left is field expr");
        field_expr = static_cast<FieldExpr *>(left_expr.get());
        value_expr = static_cast<ValueExpr *>(right_expr.get());
      } else if (right_expr->type() == ExprType::FIELD) {
        ASSERT(left_expr->type() == ExprType::VALUE, "left expr should be a value expr while right is a field expr");
        field_expr = static_cast<FieldExpr *>(right_expr.get());
        value_expr = static_cast<ValueExpr *>(left_expr.get());
      }

      if (field_expr == nullptr) {
        continue;
      }

      const Field &field = field_expr->field();
      index              = table->find_index_by_field(field.field_name());
      if (nullptr != index) {
        break;
      }
    }
  }

  if (index != nullptr) {
    ASSERT(value_expr != nullptr, "got an index but value expr is null ?");

    const Value               &value           = value_expr->get_value();
    IndexScanPhysicalOperator *index_scan_oper = new IndexScanPhysicalOperator(table,
        index,
        table_get_oper.read_write_mode(),
        &value,
        true /*left_inclusive*/,
        &value,
        true /*right_inclusive*/);

    index_scan_oper->set_alias(table_get_oper.alias());
    index_scan_oper->set_predicates(std::move(predicates));
    oper = unique_ptr<PhysicalOperator>(index_scan_oper);
    LOG_TRACE("use index scan");
  } else {
    auto table_scan_oper = new TableScanPhysicalOperator(table, table_get_oper.read_write_mode());
    table_scan_oper->set_alias(table_get_oper.alias());
    table_scan_oper->set_predicates(std::move(predicates));
    oper = unique_ptr<PhysicalOperator>(table_scan_oper);
    LOG_TRACE("use table scan");
  }

  return RC::SUCCESS;
}

RC PhysicalPlanGenerator::create_plan(PredicateLogicalOperator &pred_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &children_opers = pred_oper.children();
  ASSERT(children_opers.size() == 1, "predicate logical operator's sub oper number should be 1");

  LogicalOperator &child_oper = *children_opers.front();

  unique_ptr<PhysicalOperator> child_phy_oper;
  RC                           rc = create(child_oper, child_phy_oper, session);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create child operator of predicate operator. rc=%s", strrc(rc));
    return rc;
  }

  vector<unique_ptr<Expression>> &expressions = pred_oper.expressions();
  ASSERT(expressions.size() == 1, "predicate logical operator's children should be 1");

  unique_ptr<Expression> expression = std::move(expressions.front());
  oper = unique_ptr<PhysicalOperator>(new PredicatePhysicalOperator(std::move(expression)));
  oper->add_child(std::move(child_phy_oper));
  return rc;
}

RC PhysicalPlanGenerator::create_plan(ProjectLogicalOperator &project_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = project_oper.children();

  unique_ptr<PhysicalOperator> child_phy_oper;

  // 尝试将 ORDER BY l2_distance() LIMIT n 优化为向量索引扫描
  do {
    int limit = project_oper.limit();
    LOG_WARN("[VECTOR_OPT] limit=%d, child_opers.size()=%zu", limit, child_opers.size());
    if (limit <= 0 || child_opers.size() != 1 ||
        child_opers[0]->type() != LogicalOperatorType::ORDER_BY) {
      LOG_WARN("[VECTOR_OPT] early exit - limit=%d, child_opers.size()=%zu, type=%d",
                limit, child_opers.size(),
                child_opers.empty() ? -1 : static_cast<int>(child_opers[0]->type()));
      break;
    }

    auto *order_by_oper = static_cast<OrderByLogicalOperator *>(child_opers[0].get());
    auto &order_by_children = order_by_oper->children();
    auto &order_by_exprs = order_by_oper->expressions();

    LOG_WARN("[VECTOR_OPT] order_by_exprs.size()=%zu", order_by_exprs.size());
    if (!order_by_exprs.empty()) {
      LOG_WARN("[VECTOR_OPT] order_by_exprs[0]->type()=%d (ExprType::FUNCTION=%d)",
                static_cast<int>(order_by_exprs[0]->type()), static_cast<int>(ExprType::FUNCTION));
    }

    // 检查: ORDER BY function(...)
    if (order_by_exprs.size() != 1 ||
        order_by_exprs[0]->type() != ExprType::FUNCTION) {
      LOG_WARN("[VECTOR_OPT] not a function expression");
      break;
    }

    auto *func_expr = static_cast<ScalarFunctionExpr *>(order_by_exprs[0].get());
    auto func_type = func_expr->function_type();

    LOG_WARN("[VECTOR_OPT] func_type=%d (L2=%d, COSINE=%d, INNER=%d)",
              static_cast<int>(func_type),
              static_cast<int>(ScalarFunctionExpr::FuncType::L2_DISTANCE),
              static_cast<int>(ScalarFunctionExpr::FuncType::COSINE_DISTANCE),
              static_cast<int>(ScalarFunctionExpr::FuncType::INNER_PRODUCT));

    // 判断是否为向量距离函数 (L2_DISTANCE / COSINE_DISTANCE / INNER_PRODUCT)
    if (func_type != ScalarFunctionExpr::FuncType::L2_DISTANCE &&
        func_type != ScalarFunctionExpr::FuncType::COSINE_DISTANCE &&
        func_type != ScalarFunctionExpr::FuncType::INNER_PRODUCT) {
      LOG_WARN("[VECTOR_OPT] not a vector distance function");
      break;
    }

    // 提取 distance(field, vector) 或 distance(vector, field) 的两个参数
    Expression *left_expr = func_expr->child().get();
    Expression *right_expr = func_expr->child2().get();

    LOG_WARN("[VECTOR_OPT] left_expr=%p, right_expr=%p", left_expr, right_expr);
    if (left_expr) {
      LOG_WARN("[VECTOR_OPT] left_expr->type()=%d (FIELD=%d, VALUE=%d)",
                static_cast<int>(left_expr->type()),
                static_cast<int>(ExprType::FIELD),
                static_cast<int>(ExprType::VALUE));
    }
    if (right_expr) {
      LOG_WARN("[VECTOR_OPT] right_expr->type()=%d, value_type=%d (VECTORS=%d)",
                static_cast<int>(right_expr->type()),
                static_cast<int>(right_expr->value_type()),
                static_cast<int>(AttrType::VECTORS));
    }

    FieldExpr *field_expr = nullptr;
    ValueExpr *value_expr = nullptr;

    // 支持两种顺序: (field, vector) 或 (vector, field)
    if (left_expr && right_expr) {
      if (left_expr->type() == ExprType::FIELD &&
          right_expr->type() == ExprType::VALUE &&
          right_expr->value_type() == AttrType::VECTORS) {
        field_expr = static_cast<FieldExpr *>(left_expr);
        value_expr = static_cast<ValueExpr *>(right_expr);
        LOG_WARN("[VECTOR_OPT] matched pattern (field, vector)");
      } else if (right_expr->type() == ExprType::FIELD &&
                 left_expr->type() == ExprType::VALUE &&
                 left_expr->value_type() == AttrType::VECTORS) {
        field_expr = static_cast<FieldExpr *>(right_expr);
        value_expr = static_cast<ValueExpr *>(left_expr);
        LOG_WARN("[VECTOR_OPT] matched pattern (vector, field)");
      }
    }

    if (!field_expr || !value_expr) {
      LOG_WARN("[VECTOR_OPT] parameter type mismatch");
      break;  // 参数类型不匹配
    }

    // 检查是否是简单的 TABLE_GET (无复杂谓词)
    if (order_by_children.size() != 1 ||
        order_by_children[0]->type() != LogicalOperatorType::TABLE_GET) {
      LOG_WARN("[VECTOR_OPT] not a simple TABLE_GET, children.size()=%zu", order_by_children.size());
      break;
    }

    auto *table_get_oper = static_cast<TableGetLogicalOperator *>(order_by_children[0].get());
    Table *table = table_get_oper->table();

    LOG_WARN("[VECTOR_OPT] field_name=%s, table=%s", field_expr->field_name(), table->name());

    // 查找匹配的向量索引
    Index *index = table->find_index_by_field(field_expr->field_name());
    LOG_WARN("[VECTOR_OPT] index=%p", index);
    if (index) {
      LOG_WARN("[VECTOR_OPT] index->is_vector_index()=%d", index->is_vector_index());
    }

    if (!index || !index->is_vector_index()) {
      LOG_WARN("[VECTOR_OPT] no vector index found");
      break;  // 没有找到向量索引
    }

    // 从Value中提取vector<float>
    Value query_value = value_expr->get_value();
    if (query_value.attr_type() != AttrType::VECTORS) {
      break;
    }

    const char *data = query_value.data();
    int byte_length = query_value.length();
    if (byte_length <= 0 || byte_length % sizeof(float) != 0) {
      LOG_WARN("Invalid vector data length: %d", byte_length);
      break;
    }

    int dim = byte_length / sizeof(float);
    const float *float_ptr = reinterpret_cast<const float *>(data);
    std::vector<float> query_vector(float_ptr, float_ptr + dim);

    // 创建 VectorIndexScanPhysicalOperator !!!
    child_phy_oper = std::make_unique<VectorIndexScanPhysicalOperator>(
        table, index, query_vector, static_cast<size_t>(limit));

    LOG_WARN("[VECTOR_OPT] *** SUCCESS *** Optimized ORDER BY %s() LIMIT %d to VectorIndexScan with %d dims",
              func_type == ScalarFunctionExpr::FuncType::L2_DISTANCE ? "l2_distance" :
              func_type == ScalarFunctionExpr::FuncType::COSINE_DISTANCE ? "cosine_distance" : "inner_product",
              limit, dim);
  } while (false);

  RC rc = RC::SUCCESS;
  if (!child_opers.empty() && child_phy_oper == nullptr) {
    LogicalOperator *child_oper = child_opers.front().get();

    rc = create(*child_oper, child_phy_oper, session);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to create project logical operator's child physical operator. rc=%s", strrc(rc));
      return rc;
    }
  }

  auto project_operator = make_unique<ProjectPhysicalOperator>(std::move(project_oper.expressions()));
  if (child_phy_oper) {
    project_operator->add_child(std::move(child_phy_oper));
  }

  // 如果有LIMIT，包装LimitPhysicalOperator
  bool child_is_vector_scan = false;
  if (!project_operator->children().empty() &&
      project_operator->children().front() &&
      project_operator->children().front()->type() == PhysicalOperatorType::VECTOR_INDEX_SCAN) {
    child_is_vector_scan = true;
  }

  if (project_oper.limit() >= 0 && !child_is_vector_scan) {
    auto limit_operator = make_unique<LimitPhysicalOperator>(project_oper.limit());
    limit_operator->add_child(std::move(project_operator));
    oper = std::move(limit_operator);
    LOG_TRACE("create a limit physical operator with limit=%d", project_oper.limit());
  } else {
    oper = std::move(project_operator);
  }

  LOG_TRACE("create a project physical operator");
  return rc;
}

RC PhysicalPlanGenerator::create_plan(InsertLogicalOperator &insert_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  Table *table = insert_oper.table();
  InsertPhysicalOperator *insert_phy_oper = new InsertPhysicalOperator(table, std::move(insert_oper.values_rows()));
  oper.reset(insert_phy_oper);
  return RC::SUCCESS;
}

RC PhysicalPlanGenerator::create_plan(DeleteLogicalOperator &delete_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = delete_oper.children();

  unique_ptr<PhysicalOperator> child_physical_oper;

  RC rc = RC::SUCCESS;
  if (!child_opers.empty()) {
    LogicalOperator *child_oper = child_opers.front().get();

    rc = create(*child_oper, child_physical_oper, session);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create physical operator. rc=%s", strrc(rc));
      return rc;
    }
  }

  oper = unique_ptr<PhysicalOperator>(new DeletePhysicalOperator(delete_oper.table()));

  if (child_physical_oper) {
    oper->add_child(std::move(child_physical_oper));
  }
  return rc;
}

RC PhysicalPlanGenerator::create_plan(UpdateLogicalOperator &update_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = update_oper.children();

  unique_ptr<PhysicalOperator> child_physical_oper;

  RC rc = RC::SUCCESS;
  if (!child_opers.empty()) {
    LogicalOperator *child_oper = child_opers.front().get();

    rc = create(*child_oper, child_physical_oper, session);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create physical operator. rc=%s", strrc(rc));
      return rc;
    }
  }

  oper = unique_ptr<PhysicalOperator>(new UpdatePhysicalOperator(update_oper.table(), update_oper.field_metas(), update_oper.values(), update_oper.value_expressions()));

  if (child_physical_oper) {
    oper->add_child(std::move(child_physical_oper));
  }
  return rc;
}

RC PhysicalPlanGenerator::create_plan(ExplainLogicalOperator &explain_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = explain_oper.children();

  RC rc = RC::SUCCESS;

  unique_ptr<PhysicalOperator> explain_physical_oper(new ExplainPhysicalOperator);
  for (unique_ptr<LogicalOperator> &child_oper : child_opers) {
    unique_ptr<PhysicalOperator> child_physical_oper;
    rc = create(*child_oper, child_physical_oper, session);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create child physical operator. rc=%s", strrc(rc));
      return rc;
    }

    explain_physical_oper->add_child(std::move(child_physical_oper));
  }

  oper = std::move(explain_physical_oper);
  return rc;
}

RC PhysicalPlanGenerator::create_plan(JoinLogicalOperator &join_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;

  vector<unique_ptr<LogicalOperator>> &child_opers = join_oper.children();
  if (child_opers.size() != 2) {
    LOG_WARN("join operator should have 2 children, but have %d", child_opers.size());
    return RC::INTERNAL;
  }
  if (session->hash_join_on() && can_use_hash_join(join_oper)) {
    // your code here
  } else {
    unique_ptr<PhysicalOperator> join_physical_oper(new NestedLoopJoinPhysicalOperator());
    for (auto &child_oper : child_opers) {
      unique_ptr<PhysicalOperator> child_physical_oper;
      rc = create(*child_oper, child_physical_oper, session);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to create physical child oper. rc=%s", strrc(rc));
        return rc;
      }

      join_physical_oper->add_child(std::move(child_physical_oper));
    }

    oper = std::move(join_physical_oper);
  }
  return rc;
}

bool PhysicalPlanGenerator::can_use_hash_join(JoinLogicalOperator &join_oper)
{
  // your code here
  return false;
}

RC PhysicalPlanGenerator::create_plan(CalcLogicalOperator &logical_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;

  CalcPhysicalOperator *calc_oper = new CalcPhysicalOperator(std::move(logical_oper.expressions()));
  oper.reset(calc_oper);
  return rc;
}

RC PhysicalPlanGenerator::create_plan(GroupByLogicalOperator &logical_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;

  vector<unique_ptr<Expression>> &group_by_expressions = logical_oper.group_by_expressions();
  unique_ptr<GroupByPhysicalOperator> group_by_oper;
  if (group_by_expressions.empty()) {
    group_by_oper = make_unique<ScalarGroupByPhysicalOperator>(std::move(logical_oper.aggregate_expressions()));
  } else {
    group_by_oper = make_unique<HashGroupByPhysicalOperator>(std::move(logical_oper.group_by_expressions()),
        std::move(logical_oper.aggregate_expressions()));
  }

  ASSERT(logical_oper.children().size() == 1, "group by operator should have 1 child");

  LogicalOperator             &child_oper = *logical_oper.children().front();
  unique_ptr<PhysicalOperator> child_physical_oper;
  rc = create(child_oper, child_physical_oper, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create child physical operator of group by operator. rc=%s", strrc(rc));
    return rc;
  }

  group_by_oper->add_child(std::move(child_physical_oper));

  oper = std::move(group_by_oper);
  return rc;
}

RC PhysicalPlanGenerator::create_plan(
    VectorIndexScanLogicalOperator &vector_oper, unique_ptr<PhysicalOperator> &oper, Session * /*session*/)
{
  size_t limit = vector_oper.limit() > 0 ? static_cast<size_t>(vector_oper.limit()) : 0;
  auto vector_phy = make_unique<VectorIndexScanPhysicalOperator>(
      vector_oper.table(), vector_oper.index(), vector_oper.query_vector(), limit);
  oper = std::move(vector_phy);
  LOG_TRACE("create a vector index scan physical operator");
  return RC::SUCCESS;
}

RC PhysicalPlanGenerator::create_vec_plan(TableGetLogicalOperator &table_get_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<Expression>> &predicates = table_get_oper.predicates();
  Table *table = table_get_oper.table();
  TableScanVecPhysicalOperator *table_scan_oper = new TableScanVecPhysicalOperator(table, table_get_oper.read_write_mode());
  table_scan_oper->set_predicates(std::move(predicates));
  oper = unique_ptr<PhysicalOperator>(table_scan_oper);
  LOG_TRACE("use vectorized table scan");

  return RC::SUCCESS;
}

RC PhysicalPlanGenerator::create_vec_plan(GroupByLogicalOperator &logical_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  RC rc = RC::SUCCESS;
  unique_ptr<PhysicalOperator> physical_oper = nullptr;
  if (logical_oper.group_by_expressions().empty()) {
    physical_oper = make_unique<AggregateVecPhysicalOperator>(std::move(logical_oper.aggregate_expressions()));
  } else {
    physical_oper = make_unique<GroupByVecPhysicalOperator>(
      std::move(logical_oper.group_by_expressions()), std::move(logical_oper.aggregate_expressions()));

  }

  ASSERT(logical_oper.children().size() == 1, "group by operator should have 1 child");

  LogicalOperator             &child_oper = *logical_oper.children().front();
  unique_ptr<PhysicalOperator> child_physical_oper;
  rc = create_vec(child_oper, child_physical_oper, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create child physical operator of group by(vec) operator. rc=%s", strrc(rc));
    return rc;
  }

  physical_oper->add_child(std::move(child_physical_oper));

  oper = std::move(physical_oper);
  return rc;

  return RC::SUCCESS;
}

RC PhysicalPlanGenerator::create_vec_plan(ProjectLogicalOperator &project_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = project_oper.children();

  unique_ptr<PhysicalOperator> child_phy_oper;

  RC rc = RC::SUCCESS;
  if (!child_opers.empty()) {
    LogicalOperator *child_oper = child_opers.front().get();
    rc                          = create_vec(*child_oper, child_phy_oper, session);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create project logical operator's child physical operator. rc=%s", strrc(rc));
      return rc;
    }
  }

  auto project_operator = make_unique<ProjectVecPhysicalOperator>(std::move(project_oper.expressions()));

  if (child_phy_oper != nullptr) {
    vector<Expression *> expressions;
    for (auto &expr : project_operator->expressions()) {
      expressions.push_back(expr.get());
    }
    auto expr_operator = make_unique<ExprVecPhysicalOperator>(std::move(expressions));
    expr_operator->add_child(std::move(child_phy_oper));
    project_operator->add_child(std::move(expr_operator));
  }

  oper = std::move(project_operator);

  LOG_TRACE("create a project physical operator");
  return rc;
}


RC PhysicalPlanGenerator::create_vec_plan(ExplainLogicalOperator &explain_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  vector<unique_ptr<LogicalOperator>> &child_opers = explain_oper.children();

  RC rc = RC::SUCCESS;
  // reuse `ExplainPhysicalOperator` in explain vectorized physical plan
  unique_ptr<PhysicalOperator> explain_physical_oper(new ExplainPhysicalOperator);
  for (unique_ptr<LogicalOperator> &child_oper : child_opers) {
    unique_ptr<PhysicalOperator> child_physical_oper;
    rc = create_vec(*child_oper, child_physical_oper, session);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create child physical operator. rc=%s", strrc(rc));
      return rc;
    }

    explain_physical_oper->add_child(std::move(child_physical_oper));
  }

  oper = std::move(explain_physical_oper);
  return rc;
}

RC PhysicalPlanGenerator::create_plan(SubqueryLogicalOperator &subquery_oper, unique_ptr<PhysicalOperator> &oper, Session* session)
{
  // 获取子查询的逻辑计划
  LogicalOperator *subquery_logical_plan = subquery_oper.subquery_plan();
  if (subquery_logical_plan == nullptr) {
    LOG_WARN("subquery logical operator has no subquery plan");
    return RC::INTERNAL;
  }

  // 递归为子查询生成物理计划
  unique_ptr<PhysicalOperator> subquery_physical_plan;
  RC rc = create(*subquery_logical_plan, subquery_physical_plan, session);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create physical plan for subquery. rc=%s", strrc(rc));
    return rc;
  }

  // 用SubqueryPhysicalOperator封装物理计划
  auto subquery_physical_oper = make_unique<SubqueryPhysicalOperator>();
  subquery_physical_oper->set_subquery(std::move(subquery_physical_plan));

  oper = std::move(subquery_physical_oper);
  LOG_TRACE("create a subquery physical operator");
  return RC::SUCCESS;
}
