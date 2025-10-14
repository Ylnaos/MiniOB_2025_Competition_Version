/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <vector>

#include "sql/operator/logical_operator.h"

class Table;
class Index;

/**
 * @brief 向量索引扫描逻辑算子
 * @details 仅用于邻近向量检索改写后的逻辑计划
 */
class VectorIndexScanLogicalOperator : public LogicalOperator
{
public:
  VectorIndexScanLogicalOperator(Table *table, Index *index, std::vector<float> query_vector, int limit);
  virtual ~VectorIndexScanLogicalOperator() = default;

  LogicalOperatorType type() const override { return LogicalOperatorType::VECTOR_INDEX_SCAN; }
  OpType              get_op_type() const override { return OpType::LOGICALGET; }

  Table                     *table() const { return table_; }
  Index                     *index() const { return index_; }
  const std::vector<float>  &query_vector() const { return query_vector_; }
  int                        limit() const { return limit_; }

private:
  Table                    *table_        = nullptr;
  Index                    *index_        = nullptr;
  std::vector<float>        query_vector_;
  int                       limit_        = -1;
};
