/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/group_by_vec_physical_operator.h"

#include <cstdint>

#include "common/log/log.h"
#include "common/type/attr_type.h"
#include "sql/expr/expression.h"

using namespace std;

namespace {

int resolve_column_length(Expression &expr)
{
  int len = expr.value_length();
  if (len > 0) {
    return len;
  }

  switch (expr.value_type()) {
    case AttrType::INTS:
    case AttrType::DATES:
      return static_cast<int>(sizeof(int32_t));
    case AttrType::BIGINTS:
      return static_cast<int>(sizeof(int64_t));
    case AttrType::FLOATS:
      return static_cast<int>(sizeof(float));
    case AttrType::BOOLEANS:
      return static_cast<int>(sizeof(uint8_t));
    default:
      // 为了保证内存分配合法，至少返回4字节
      return static_cast<int>(sizeof(int32_t));
  }
}

}  // namespace

GroupByVecPhysicalOperator::GroupByVecPhysicalOperator(
    vector<unique_ptr<Expression>> &&group_by_exprs, vector<Expression *> &&expressions)
    : group_by_expressions_(std::move(group_by_exprs)), aggregate_expressions_(std::move(expressions))
{
  value_expressions_.reserve(aggregate_expressions_.size());
  for (Expression *expr : aggregate_expressions_) {
    ASSERT(expr->type() == ExprType::AGGREGATION, "expect aggregation expression");
    auto *aggregate_expr = static_cast<AggregateExpr *>(expr);
    Expression *child    = aggregate_expr->child() ? aggregate_expr->child().get() : nullptr;
    ASSERT(child != nullptr, "aggregate expression must have child expression");
    value_expressions_.push_back(child);
  }

  int column_id = 0;
  for (auto &expr : group_by_expressions_) {
    int length = resolve_column_length(*expr);
    output_chunk_.add_column(make_unique<Column>(expr->value_type(), length), column_id++);
  }
  for (Expression *expr : aggregate_expressions_) {
    auto *aggregate_expr = static_cast<AggregateExpr *>(expr);
    int   length         = resolve_column_length(*aggregate_expr);
    output_chunk_.add_column(make_unique<Column>(aggregate_expr->value_type(), length), column_id++);
  }
}

RC GroupByVecPhysicalOperator::open(Trx *trx)
{
  ASSERT(children_.size() == 1, "group by operator only support one child, but got %d", children_.size());

  RC rc = RC::SUCCESS;

  // 每次 open 时重新构建哈希表，避免残留状态
  hash_table_ = make_unique<StandardAggregateHashTable>(aggregate_expressions_);
  scanner_.reset();

  output_chunk_.reset_data();

  PhysicalOperator &child = *children_[0];
  rc                      = child.open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    return rc;
  }

  while (OB_SUCC(rc = child.next(input_chunk_))) {
    group_chunk_.reset();
    aggr_chunk_.reset();

    for (size_t i = 0; i < group_by_expressions_.size(); i++) {
      auto column = make_unique<Column>();
      RC   col_rc = group_by_expressions_[i]->get_column(input_chunk_, *column);
      if (OB_FAIL(col_rc)) {
        LOG_WARN("failed to evaluate group by column %zu: %s", i, strrc(col_rc));
        return col_rc;
      }
      group_chunk_.add_column(std::move(column), static_cast<int>(i));
    }

    for (size_t i = 0; i < value_expressions_.size(); i++) {
      auto column = make_unique<Column>();
      RC   col_rc = value_expressions_[i]->get_column(input_chunk_, *column);
      if (OB_FAIL(col_rc)) {
        LOG_WARN("failed to evaluate aggregate column %zu: %s", i, strrc(col_rc));
        return col_rc;
      }
      aggr_chunk_.add_column(std::move(column), static_cast<int>(i));
    }

    if (value_expressions_.empty()) {
      if (group_chunk_.column_num() > 0) {
        auto dummy_column = make_unique<Column>();
        dummy_column->reference(group_chunk_.column(0));
        aggr_chunk_.add_column(std::move(dummy_column), 0);
      } else {
        auto dummy_column = make_unique<Column>(AttrType::INTS, static_cast<int>(sizeof(int32_t)));
        dummy_column->set_count(input_chunk_.rows());
        aggr_chunk_.add_column(std::move(dummy_column), 0);
      }
    }

    RC add_rc = hash_table_->add_chunk(group_chunk_, aggr_chunk_);
    if (OB_FAIL(add_rc)) {
      LOG_WARN("failed to add chunk into hash table: %s", strrc(add_rc));
      return add_rc;
    }
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  } else if (OB_FAIL(rc)) {
    LOG_WARN("failed to fetch child chunk: %s", strrc(rc));
    return rc;
  }

  scanner_ = make_unique<StandardAggregateHashTable::Scanner>(hash_table_.get());
  scanner_->open_scan();
  output_chunk_.reset_data();

  return rc;
}

RC GroupByVecPhysicalOperator::next(Chunk &chunk)
{
  if (!scanner_) {
    return RC::RECORD_EOF;
  }

  output_chunk_.reset_data();

  RC rc = scanner_->next(output_chunk_);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  if (output_chunk_.rows() == 0) {
    return RC::RECORD_EOF;
  }

  RC ref_rc = chunk.reference(output_chunk_);
  if (OB_FAIL(ref_rc)) {
    LOG_WARN("failed to reference output chunk: %s", strrc(ref_rc));
    return ref_rc;
  }

  return RC::SUCCESS;
}

RC GroupByVecPhysicalOperator::close()
{
  RC rc = RC::SUCCESS;

  if (scanner_) {
    scanner_->close_scan();
    scanner_.reset();
  }

  hash_table_.reset();

  if (!children_.empty()) {
    RC child_rc = children_[0]->close();
    if (OB_FAIL(child_rc) && rc == RC::SUCCESS) {
      rc = child_rc;
    }
  }

  input_chunk_.reset();
  group_chunk_.reset();
  aggr_chunk_.reset();
  output_chunk_.reset_data();

  return rc;
}
