/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/vector_index_scan_physical_operator.h"
#include "storage/table/table.h"
#include "common/log/log.h"

VectorIndexScanPhysicalOperator::VectorIndexScanPhysicalOperator(
    Table *table,
    IvfflatIndex *index,
    const std::vector<float> &query_vector,
    size_t limit,
    ReadWriteMode mode)
    : table_(table), index_(index), mode_(mode), query_vector_(query_vector), limit_(limit)
{
  tuple_.set_schema(table, table->table_meta().field_metas());
}

string VectorIndexScanPhysicalOperator::param() const
{
  return std::string(table_->name()) + " ON " + index_->index_meta().name();
}

RC VectorIndexScanPhysicalOperator::open(Trx *trx)
{
  if (nullptr == table_ || nullptr == index_) {
    return RC::INTERNAL;
  }

  trx_ = trx;

  // 执行ANN搜索
  result_rids_ = index_->ann_search(query_vector_, limit_);
  current_index_ = 0;

  LOG_INFO("Vector index scan found %zu results", result_rids_.size());

  return RC::SUCCESS;
}

RC VectorIndexScanPhysicalOperator::next()
{
  if (current_index_ >= result_rids_.size()) {
    return RC::RECORD_EOF;
  }

  // 获取当前RID对应的记录
  const RID &rid = result_rids_[current_index_];
  RC rc = table_->get_record(rid, current_record_);
  if (rc != RC::SUCCESS) {
    LOG_WARN("Failed to get record by rid. rid=%s, rc=%s",
             rid.to_string().c_str(), strrc(rc));
    return rc;
  }

  tuple_.set_record(&current_record_);
  current_index_++;

  return RC::SUCCESS;
}

RC VectorIndexScanPhysicalOperator::close()
{
  result_rids_.clear();
  current_index_ = 0;
  return RC::SUCCESS;
}

Tuple *VectorIndexScanPhysicalOperator::current_tuple()
{
  return &tuple_;
}
