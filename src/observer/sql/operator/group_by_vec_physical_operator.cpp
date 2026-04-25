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
#include "common/log/log.h"
#include "common/type/attr_type.h"
#include "sql/expr/aggregate_state.h"

GroupByVecPhysicalOperator::GroupByVecPhysicalOperator(
    vector<unique_ptr<Expression>> &&group_by_exprs, vector<Expression *> &&expressions)
{
  group_by_expressions_  = std::move(group_by_exprs);
  aggregate_expressions_ = std::move(expressions);
  value_expressions_.reserve(aggregate_expressions_.size());

  for (size_t i = 0; i < group_by_expressions_.size(); i++) {
    Expression *expr = group_by_expressions_[i].get();
    output_chunk_.add_column(make_unique<Column>(expr->value_type(), expr->value_length()), static_cast<int>(i));
  }

  const int group_by_num = static_cast<int>(group_by_expressions_.size());
  for (size_t i = 0; i < aggregate_expressions_.size(); i++) {
    ASSERT(aggregate_expressions_[i]->type() == ExprType::AGGREGATION, "expect aggregate expression");
    auto *aggregate_expr = static_cast<AggregateExpr *>(aggregate_expressions_[i]);
    Expression *child_expr = aggregate_expr->child().get();
    ASSERT(child_expr != nullptr, "aggregate expression must have a child expression");
    value_expressions_.emplace_back(child_expr);
    output_chunk_.add_column(
        make_unique<Column>(aggregate_expr->value_type(), aggregate_expr->value_length()), group_by_num + i);
  }
}

RC GroupByVecPhysicalOperator::open(Trx *trx)
{
  ASSERT(children_.size() == 1, "group by operator only support one child, but got %d", children_.size());

  output_chunk_.reset_data();
  hash_table_ = make_unique<StandardAggregateHashTable>(aggregate_expressions_);
  scanner_.reset();

  PhysicalOperator &child = *children_[0];
  RC rc = child.open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open child operator. rc=%s", strrc(rc));
    return rc;
  }

  while (OB_SUCC(rc = child.next(child_chunk_))) {
    Chunk group_chunk;
    Chunk aggr_chunk;

    for (size_t i = 0; i < group_by_expressions_.size(); i++) {
      auto column = make_unique<Column>();
      rc = group_by_expressions_[i]->get_column(child_chunk_, *column);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to evaluate group by expression. rc=%s", strrc(rc));
        return rc;
      }
      group_chunk.add_column(std::move(column), static_cast<int>(i));
    }

    for (size_t i = 0; i < value_expressions_.size(); i++) {
      auto column = make_unique<Column>();
      rc = value_expressions_[i]->get_column(child_chunk_, *column);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to evaluate aggregate expression. rc=%s", strrc(rc));
        return rc;
      }
      aggr_chunk.add_column(std::move(column), static_cast<int>(i));
    }

    rc = hash_table_->add_chunk(group_chunk, aggr_chunk);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to aggregate chunk. rc=%s", strrc(rc));
      return rc;
    }
  }

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("failed to read child chunks. rc=%s", strrc(rc));
    return rc;
  }

  scanner_ = make_unique<StandardAggregateHashTable::Scanner>(hash_table_.get());
  scanner_->open_scan();
  return RC::SUCCESS;
}

RC GroupByVecPhysicalOperator::next(Chunk &chunk)
{
  if (scanner_ == nullptr) {
    return RC::RECORD_EOF;
  }

  output_chunk_.reset_data();
  RC rc = scanner_->next(output_chunk_);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (output_chunk_.rows() == 0) {
    return RC::RECORD_EOF;
  }

  return chunk.reference(output_chunk_);
}

RC GroupByVecPhysicalOperator::close()
{
  if (scanner_ != nullptr) {
    scanner_->close_scan();
    scanner_.reset();
  }
  hash_table_.reset();
  child_chunk_.reset();
  output_chunk_.reset_data();

  if (!children_.empty()) {
    children_[0]->close();
  }
  return RC::SUCCESS;
}
