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
#include "storage/index/ivfflat_index.h"
#include "storage/table/table.h"
#include "common/log/log.h"

VectorIndexScanPhysicalOperator::VectorIndexScanPhysicalOperator(
    Table *table,
    Index *index,
    const std::vector<float> &query_vector,
    size_t limit)
    : table_(table), index_(index), query_vector_(query_vector), limit_(limit)
{
  if (table_) {
    tuple_.set_schema(table_, table_->table_meta().field_metas());
  } else {
    LOG_WARN("VectorIndexScanPhysicalOperator created with null table");
  }
  if (!index_) {
    LOG_WARN("VectorIndexScanPhysicalOperator created with null index");
  }
}

string VectorIndexScanPhysicalOperator::param() const
{
  if (!table_ || !index_) {
    return "INVALID(table or index is null)";
  }
  return string(index_->index_meta().name()) + " ON " + table_->name();
}

RC VectorIndexScanPhysicalOperator::open(Trx *trx)
{
  if (nullptr == table_) {
    LOG_WARN("VectorIndexScanPhysicalOperator::open - table is null");
    return RC::INTERNAL;
  }

  if (nullptr == index_) {
    LOG_WARN("VectorIndexScanPhysicalOperator::open - index is null");
    return RC::INTERNAL;
  }

  // 调用IvfflatIndex的ann_search方法
  IvfflatIndex *ivfflat_index = dynamic_cast<IvfflatIndex *>(index_);
  if (ivfflat_index == nullptr) {
    LOG_WARN("index is not an IvfflatIndex, index name: %s",
             index_->index_meta().name());
    return RC::INTERNAL;
  }

  // 检查索引是否已经准备好
  if (!ivfflat_index->ready()) {
    LOG_WARN("VectorIndexScanPhysicalOperator::open - index not ready for ANN search");
    result_rids_.clear();
    current_idx_ = 0;
    trx_ = trx;
    return RC::SUCCESS;  // 返回空结果而不是错误
  }

  result_rids_ = ivfflat_index->ann_search(query_vector_, limit_);
  current_idx_ = 0;
  trx_ = trx;

  LOG_TRACE("VectorIndexScan found %zu results", result_rids_.size());
  return RC::SUCCESS;
}

RC VectorIndexScanPhysicalOperator::next()
{
  if (current_idx_ >= result_rids_.size()) {
    return RC::RECORD_EOF;
  }

  const RID &rid = result_rids_[current_idx_];
  RC rc = table_->get_record(rid, current_record_);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to get record. rid=%s, rc=%s", rid.to_string().c_str(), strrc(rc));
    return rc;
  }

  tuple_.set_record(&current_record_);
  current_idx_++;

  return RC::SUCCESS;
}

RC VectorIndexScanPhysicalOperator::close()
{
  result_rids_.clear();
  current_idx_ = 0;
  return RC::SUCCESS;
}

Tuple *VectorIndexScanPhysicalOperator::current_tuple()
{
  tuple_.set_record(&current_record_);
  return &tuple_;
}
