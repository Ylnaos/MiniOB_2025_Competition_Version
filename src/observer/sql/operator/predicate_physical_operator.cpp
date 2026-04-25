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
// Created by WangYunlai on 2022/6/27.
//

#include "sql/operator/predicate_physical_operator.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/field/field.h"
#include "storage/record/record.h"
#include "sql/expr/expression_iterator.h"

#include <algorithm>

PredicatePhysicalOperator::PredicatePhysicalOperator(std::unique_ptr<Expression> expr) : expression_(std::move(expr))
{
  ASSERT(expression_->value_type() == AttrType::BOOLEANS, "predicate's expression should be BOOLEAN type");
}

RC PredicatePhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 1) {
    LOG_WARN("predicate operator must has one child");
    return RC::INTERNAL;
  }

  return children_[0]->open(trx);
}

RC PredicatePhysicalOperator::next()
{
  RC                rc   = RC::SUCCESS;
  PhysicalOperator *oper = children_.front().get();

  while (RC::SUCCESS == (rc = oper->next())) {
    Tuple *tuple = oper->current_tuple();
    if (nullptr == tuple) {
      rc = RC::INTERNAL;
      LOG_WARN("failed to get tuple from operator");
      break;
    }

    // 在求值每个tuple前，重置表达式树中所有子查询的缓存状态
    // 这确保子查询对每个tuple都能正确执行（对于非关联子查询，第一次执行后会重新缓存）
    ExpressionIterator::reset_subquery_cache(*expression_);

    Value value;
    rc = expression_->get_value(*tuple, value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get predicate value. rc=%s", strrc(rc));
      return rc;
    }

    if (value.get_boolean()) {
      return rc;
    }
  }
  return rc;
}

RC PredicatePhysicalOperator::next(Chunk &chunk)
{
  RC                rc   = RC::SUCCESS;
  PhysicalOperator *oper = children_.front().get();

  while (RC::SUCCESS == (rc = oper->next(input_chunk_))) {
    const int rows = input_chunk_.rows();
    select_.assign(rows, 1);
    ExpressionIterator::reset_subquery_cache(*expression_);

    rc = expression_->eval(input_chunk_, select_);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to evaluate vectorized predicate. rc=%s", strrc(rc));
      return rc;
    }

    filtered_chunk_.reset();
    for (int col_idx = 0; col_idx < input_chunk_.column_num(); ++col_idx) {
      const Column &input_column = input_chunk_.column(col_idx);
      auto output_column = make_unique<Column>(
          input_column.attr_type(), input_column.attr_len(), std::max(rows, 1));

      for (int row_idx = 0; row_idx < rows; ++row_idx) {
        if (select_[row_idx] == 0) {
          continue;
        }
        rc = output_column->append_value(input_column.get_value(row_idx));
        if (OB_FAIL(rc)) {
          LOG_WARN("failed to append filtered value. rc=%s", strrc(rc));
          return rc;
        }
      }

      filtered_chunk_.add_column(std::move(output_column), input_chunk_.column_ids(col_idx));
    }

    if (filtered_chunk_.rows() > 0) {
      return chunk.reference(filtered_chunk_);
    }

    input_chunk_.reset_data();
  }

  return rc;
}

RC PredicatePhysicalOperator::close()
{
  children_[0]->close();
  input_chunk_.reset();
  filtered_chunk_.reset();
  select_.clear();
  return RC::SUCCESS;
}

Tuple *PredicatePhysicalOperator::current_tuple() { return children_[0]->current_tuple(); }

RC PredicatePhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  return children_[0]->tuple_schema(schema);
}
