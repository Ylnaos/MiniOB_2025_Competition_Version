/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/vector_rewrite_rule.h"

#include <cctype>
#include <memory>
#include <vector>

#include "common/log/log.h"
#include "common/lang/string.h"
#include "sql/expr/expression.h"
#include "sql/operator/order_by_logical_operator.h"
#include "sql/operator/project_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "sql/operator/vector_index_scan_logical_operator.h"
#include "storage/field/field_meta.h"
#include "storage/index/index.h"
#include "storage/index/ivfflat_index.h"
#include "storage/table/table.h"

namespace {

/**
 * @brief 去除 CastExpr 包裹，获取内部表达式
 */
Expression *unwrap_cast(Expression *expr)
{
  auto *current = expr;
  while (current != nullptr && current->type() == ExprType::CAST) {
    auto *cast_expr = static_cast<CastExpr *>(current);
    current         = cast_expr->child().get();
  }
  return current;
}

/**
 * @brief 提取字段表达式
 */
FieldExpr *extract_field(Expression *expr)
{
  if (expr == nullptr) {
    return nullptr;
  }
  if (expr->type() == ExprType::FIELD) {
    return static_cast<FieldExpr *>(expr);
  }
  if (expr->type() == ExprType::CAST) {
    auto *cast_expr = static_cast<CastExpr *>(expr);
    return extract_field(cast_expr->child().get());
  }
  return nullptr;
}

/**
 * @brief 尝试解析常量向量
 */
bool extract_vector_literal(Expression *expr, std::vector<float> &out_vec)
{
  if (expr == nullptr) {
    return false;
  }

  Value literal_value;
  RC    rc = expr->try_get_value(literal_value);
  if (OB_FAIL(rc)) {
    return false;
  }

  if (literal_value.attr_type() != AttrType::VECTORS) {
    return false;
  }

  const int len = literal_value.length();
  if (len < 0 || (len % static_cast<int>(sizeof(float))) != 0) {
    return false;
  }
  const float *data = reinterpret_cast<const float *>(literal_value.data());
  out_vec.assign(data, data + len / static_cast<int>(sizeof(float)));
  return true;
}

/**
 * @brief 匹配 L2_DISTANCE(column, vector_literal) 模式
 */
bool match_distance_expr(
    Expression *expr, FieldExpr *&field_expr, std::vector<float> &query_vector, ScalarFunctionExpr::FuncType &func_type)
{
  Expression *raw_expr = unwrap_cast(expr);
  if (raw_expr == nullptr || raw_expr->type() != ExprType::FUNCTION) {
    return false;
  }

  auto *func_expr = static_cast<ScalarFunctionExpr *>(raw_expr);
  func_type       = func_expr->function_type();
  if (func_type != ScalarFunctionExpr::FuncType::L2_DISTANCE) {
    return false;
  }

  Expression *left  = func_expr->child().get();
  Expression *right = func_expr->child2().get();

  FieldExpr *candidate_field = extract_field(left);
  Expression *vector_part    = right;
  if (candidate_field == nullptr) {
    candidate_field = extract_field(right);
    vector_part     = left;
  }
  if (candidate_field == nullptr) {
    return false;
  }

  std::vector<float> literal;
  if (!extract_vector_literal(vector_part, literal)) {
    return false;
  }

  field_expr   = candidate_field;
  query_vector = std::move(literal);
  return true;
}

}  // namespace

RC VectorRewriteRule::rewrite(std::unique_ptr<LogicalOperator> &oper, bool &change_made)
{
  change_made = false;
  if (!oper) {
    LOG_TRACE("vector rewrite skip: oper is null");
    return RC::SUCCESS;
  }

  if (oper->type() != LogicalOperatorType::PROJECTION) {
    LOG_TRACE("vector rewrite skip: oper type is not PROJECTION (type=%d)", static_cast<int>(oper->type()));
    return RC::SUCCESS;
  }

  auto *project_oper = static_cast<ProjectLogicalOperator *>(oper.get());
  if (project_oper->limit() <= 0) {
    LOG_TRACE("vector rewrite skip: limit not set or invalid (limit=%d)", project_oper->limit());
    return RC::SUCCESS;
  }
  LOG_TRACE("vector rewrite: found PROJECT with limit=%d", project_oper->limit());

  auto &proj_children = project_oper->children();
  if (proj_children.size() != 1 || !proj_children.front()) {
    LOG_TRACE("vector rewrite skip: PROJECT children count != 1 (count=%zu)", proj_children.size());
    return RC::SUCCESS;
  }

  auto &order_child = proj_children.front();
  if (order_child->type() != LogicalOperatorType::ORDER_BY) {
    LOG_TRACE("vector rewrite skip: PROJECT child is not ORDER_BY (type=%d)", static_cast<int>(order_child->type()));
    return RC::SUCCESS;
  }
  LOG_TRACE("vector rewrite: found ORDER_BY child");

  auto *order_oper = static_cast<OrderByLogicalOperator *>(order_child.get());
  auto &order_items = order_oper->order_by_items();
  if (order_items.size() != 1) {
    LOG_TRACE("vector rewrite skip: ORDER_BY items count != 1 (count=%zu)", order_items.size());
    return RC::SUCCESS;
  }
  if (!order_items.front().second) {
    LOG_TRACE("vector rewrite skip: ORDER_BY direction is not ASC (is_asc=%d)", order_items.front().second);
    return RC::SUCCESS;
  }
  LOG_TRACE("vector rewrite: ORDER_BY has 1 item in ASC order");

  FieldExpr                       *target_field   = nullptr;
  std::vector<float>               query_vector;
  ScalarFunctionExpr::FuncType     func_type = ScalarFunctionExpr::FuncType::L2_DISTANCE;
  if (!match_distance_expr(order_items.front().first.get(), target_field, query_vector, func_type)) {
    LOG_TRACE("vector rewrite skip: ORDER_BY expression does not match L2_DISTANCE pattern");
    return RC::SUCCESS;
  }
  LOG_TRACE("vector rewrite: matched L2_DISTANCE in ORDER_BY, query_vector size=%zu", query_vector.size());

  if (order_child->children().size() != 1 || !order_child->children().front()) {
    LOG_TRACE("vector rewrite skip: ORDER_BY children count != 1");
    return RC::SUCCESS;
  }
  auto &scan_child = order_child->children().front();
  if (scan_child->type() != LogicalOperatorType::TABLE_GET) {
    LOG_TRACE("vector rewrite skip: ORDER_BY child is not TABLE_GET (type=%d)", static_cast<int>(scan_child->type()));
    return RC::SUCCESS;
  }
  LOG_TRACE("vector rewrite: found TABLE_GET under ORDER_BY");

  auto *table_get = static_cast<TableGetLogicalOperator *>(scan_child.get());
  if (!table_get->predicates().empty()) {
    LOG_TRACE("vector rewrite skip: table get still has predicates (count=%zu)", table_get->predicates().size());
    return RC::SUCCESS;
  }

  Table *table = table_get->table();
  if (table == nullptr || target_field == nullptr) {
    LOG_TRACE("vector rewrite skip: table or target_field is null (table=%p, field=%p)",
              static_cast<void*>(table), static_cast<void*>(target_field));
    return RC::SUCCESS;
  }
  LOG_TRACE("vector rewrite: table=%s, field=%s", table->name(), target_field->field().field_name());

  if (target_field->field().meta() == nullptr || target_field->field().meta()->type() != AttrType::VECTORS) {
    LOG_TRACE("vector rewrite skip: field is not VECTOR type (meta=%p, type=%d)",
              static_cast<const void*>(target_field->field().meta()),
              target_field->field().meta() ? static_cast<int>(target_field->field().meta()->type()) : -1);
    return RC::SUCCESS;
  }

  // 要求字段归属同一张表
  if (target_field->field().table() != table) {
    if (strcasecmp(target_field->field().table_name(), table->name()) != 0) {
      LOG_TRACE("vector rewrite skip: field table mismatch (field_table=%s, scan_table=%s)",
                target_field->field().table_name(), table->name());
      return RC::SUCCESS;
    }
  }
  LOG_TRACE("vector rewrite: field type is VECTOR and table matches");

  Index *index = table->find_index_by_field(target_field->field().meta()->name());
  if (index == nullptr) {
    LOG_TRACE("vector rewrite skip: no index found on field %s", target_field->field().meta()->name());
    return RC::SUCCESS;
  }
  if (!index->is_vector_index()) {
    LOG_TRACE("vector rewrite skip: index %s is not a vector index", index->index_meta().name());
    return RC::SUCCESS;
  }
  LOG_TRACE("vector rewrite: found vector index %s", index->index_meta().name());

  auto *ivf_index = dynamic_cast<IvfflatIndex *>(index);
  if (ivf_index == nullptr) {
    LOG_TRACE("vector rewrite skip: vector index type unsupported (not IvfflatIndex)");
    return RC::SUCCESS;
  }
  if (!ivf_index->ready()) {
    LOG_TRACE("vector rewrite skip: vector index %s not ready for ANN search", index->index_meta().name());
    return RC::SUCCESS;
  }
  LOG_TRACE("vector rewrite: IvfflatIndex is ready");

  // 验证查询向量维度是否与索引匹配
  int index_dimension = ivf_index->dimension();
  if (index_dimension > 0 && static_cast<int>(query_vector.size()) != index_dimension) {
    LOG_TRACE("vector rewrite skip: query vector dimension %zu does not match index dimension %d",
              query_vector.size(), index_dimension);
    return RC::SUCCESS;
  }
  LOG_TRACE("vector rewrite: dimension check passed (query=%zu, index=%d)", query_vector.size(), index_dimension);

  // 保存需要的信息，因为替换子节点后 target_field 会被释放
  const char *table_name = table->name();
  const char *field_name = target_field->field().field_name();
  int limit = project_oper->limit();

  auto vector_scan_oper =
      make_unique<VectorIndexScanLogicalOperator>(table, index, std::move(query_vector), project_oper->limit());
  proj_children.front() = std::move(vector_scan_oper);

  change_made = true;
  LOG_INFO("vector rewrite SUCCESS: replaced ORDER_BY+TABLE_GET with VECTOR_INDEX_SCAN on table=%s, field=%s, limit=%d",
           table_name, field_name, limit);
  return RC::SUCCESS;
}
