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
#include <algorithm>
#include <limits>

VectorIndexScanPhysicalOperator::VectorIndexScanPhysicalOperator(
    Table *table,
    Index *index,
    const std::vector<float> &query_vector,
    size_t limit)
    : table_(table), index_(index), query_vector_(query_vector), limit_(limit)
{
  tuple_.set_schema(table_, table_->table_meta().field_metas());
}

string VectorIndexScanPhysicalOperator::param() const
{
  return std::string(table_->name()) + " ON " + index_->index_meta().name() +
         " (limit=" + std::to_string(limit_) + ")";
}

RC VectorIndexScanPhysicalOperator::open(Trx *trx)
{
  if (nullptr == table_ || nullptr == index_) {
    return RC::INTERNAL;
  }

  // 调用IvfflatIndex的ann_search方法
  IvfflatIndex *ivfflat_index = dynamic_cast<IvfflatIndex *>(index_);
  if (ivfflat_index == nullptr) {
    LOG_WARN("index is not an IvfflatIndex");
    return RC::INTERNAL;
  }
  if (!ivfflat_index->ready()) {
    LOG_WARN("vector index not ready, fallback to empty result. table=%s index=%s",
        table_->name(), index_->index_meta().name());
    result_rids_.clear();
    current_idx_ = 0;
    return RC::SUCCESS;
  }
  const int index_dim = ivfflat_index->dimension();
  if (index_dim > 0 && static_cast<int>(query_vector_.size()) != index_dim) {
    LOG_WARN("query vector dimension mismatch. expect=%d actual=%zu", index_dim, query_vector_.size());
    result_rids_.clear();
    current_idx_ = 0;
    return RC::SUCCESS;
  }

  size_t search_limit = limit_;
  if (search_limit == 0) {
    search_limit = static_cast<size_t>(std::max(index_dim, 1));
  }

  result_rids_ = ivfflat_index->ann_search(query_vector_, search_limit);
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

  RC rc = RC::RECORD_EOF;
  while (current_idx_ < result_rids_.size()) {
    const RID &rid = result_rids_[current_idx_];
    rc = table_->get_record(rid, current_record_);
    current_idx_++;
    if (rc == RC::SUCCESS) {
      tuple_.set_record(&current_record_);
      return RC::SUCCESS;
    }
    LOG_WARN("skip invalid record from vector index. rid=%s rc=%s",
        rid.to_string().c_str(), strrc(rc));
  }

  return RC::RECORD_EOF;
}

RC VectorIndexScanPhysicalOperator::close()
{
  result_rids_.clear();
  current_idx_ = 0;
  return RC::SUCCESS;
}

Tuple *VectorIndexScanPhysicalOperator::current_tuple()
{
  return &tuple_;
}
