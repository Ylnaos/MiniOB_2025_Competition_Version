/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/limit_physical_operator.h"
#include "common/log/log.h"
#include <algorithm>

RC LimitPhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 1) {
    LOG_WARN("limit operator expects exactly 1 child, got %d", children_.size());
    return RC::INTERNAL;
  }

  RC rc = children_[0]->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open child operator. rc=%s", strrc(rc));
    return rc;
  }

  returned_ = 0;
  return RC::SUCCESS;
}

RC LimitPhysicalOperator::next()
{
  // 如果limit为-1，表示不限制，直接透传
  if (limit_ < 0) {
    return children_[0]->next();
  }

  // 如果已经返回了足够多的行，直接返回EOF
  if (returned_ >= limit_) {
    return RC::RECORD_EOF;
  }

  // 否则从子算子获取下一行
  RC rc = children_[0]->next();
  if (rc == RC::SUCCESS) {
    returned_++;
  }
  return rc;
}

RC LimitPhysicalOperator::next(Chunk &chunk)
{
  if (limit_ < 0) {
    return children_[0]->next(chunk);
  }

  if (returned_ >= limit_) {
    return RC::RECORD_EOF;
  }

  while (returned_ < limit_) {
    input_chunk_.reset();
    RC rc = children_[0]->next(input_chunk_);
    if (rc != RC::SUCCESS) {
      return rc;
    }

    const int input_rows = input_chunk_.rows();
    if (input_rows <= 0) {
      continue;
    }

    const int rows_to_take = std::min(input_rows, limit_ - returned_);
    limited_chunk_.reset();
    for (int col_idx = 0; col_idx < input_chunk_.column_num(); ++col_idx) {
      const Column &input_column = input_chunk_.column(col_idx);
      auto output_column = make_unique<Column>(
          input_column.attr_type(), input_column.attr_len(), std::max(rows_to_take, 1));
      for (int row_idx = 0; row_idx < rows_to_take; ++row_idx) {
        rc = output_column->append_value(input_column.get_value(row_idx));
        if (OB_FAIL(rc)) {
          LOG_WARN("failed to append limited value. rc=%s", strrc(rc));
          return rc;
        }
      }
      limited_chunk_.add_column(std::move(output_column), input_chunk_.column_ids(col_idx));
    }

    returned_ += rows_to_take;
    return chunk.reference(limited_chunk_);
  }

  return RC::RECORD_EOF;
}

RC LimitPhysicalOperator::close()
{
  if (!children_.empty()) {
    children_[0]->close();
  }
  returned_ = 0;
  input_chunk_.reset();
  limited_chunk_.reset();
  return RC::SUCCESS;
}

Tuple *LimitPhysicalOperator::current_tuple()
{
  return children_[0]->current_tuple();
}

RC LimitPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  if (children_.empty()) {
    schema = TupleSchema{};
    return RC::SUCCESS;
  }
  // LIMIT 算子不改变列结构，直接沿用子算子的列描述
  return children_[0]->tuple_schema(schema);
}
