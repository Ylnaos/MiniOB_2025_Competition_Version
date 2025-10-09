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

#include "sql/expr/tuple.h"
#include "sql/operator/physical_operator.h"
#include "storage/index/ivfflat_index.h"
#include <vector>

/**
 * @brief 向量索引扫描物理算子
 * @ingroup PhysicalOperator
 * @details 用于执行ORDER BY vector_distance() LIMIT n的ANN查询
 */
class VectorIndexScanPhysicalOperator : public PhysicalOperator
{
public:
  VectorIndexScanPhysicalOperator(
      Table *table,
      IvfflatIndex *index,
      const std::vector<float> &query_vector,
      size_t limit,
      ReadWriteMode mode = ReadWriteMode::READ_ONLY);

  virtual ~VectorIndexScanPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::INDEX_SCAN; }

  string param() const override;

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override;

private:
  Trx          *trx_   = nullptr;
  Table        *table_ = nullptr;
  IvfflatIndex *index_ = nullptr;
  ReadWriteMode mode_  = ReadWriteMode::READ_ONLY;

  std::vector<float> query_vector_;
  size_t limit_;

  // ANN搜索结果
  std::vector<RID> result_rids_;
  size_t current_index_ = 0;

  Record   current_record_;
  RowTuple tuple_;
};
