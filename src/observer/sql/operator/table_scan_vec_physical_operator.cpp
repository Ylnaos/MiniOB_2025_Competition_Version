/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/table_scan_vec_physical_operator.h"
#include "event/sql_debug.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"
#include "storage/field/field_meta.h"
#include "common/type/attr_type.h"
#include "common/value.h"

#include <algorithm>
#include <sstream>
#include <utility>

using namespace std;

namespace {

template <typename... Args>
void exec_trace(const char *fmt, Args &&... args)
{
  LOG_INFO(fmt, std::forward<Args>(args)...);
  sql_debug(fmt, std::forward<Args>(args)...);
}

const FieldMeta *find_field_by_id(const TableMeta &meta, int field_id)
{
  const int field_count = meta.field_num();
  for (int i = 0; i < field_count; ++i) {
    const FieldMeta *field = meta.field(i);
    if (field != nullptr && field->field_id() == field_id) {
      return field;
    }
  }
  return nullptr;
}

std::string dump_column_samples(const Column &column, int sample_limit = 5)
{
  std::ostringstream oss;
  const int row_count = column.count();
  oss << "rows=" << row_count;
  if (row_count == 0) {
    return oss.str();
  }

  const int limit = std::min(row_count, sample_limit);
  oss << ", samples=[";
  for (int i = 0; i < limit; ++i) {
    Value value = column.get_value(i);
    oss << value.to_string();
    if (i + 1 < limit) {
      oss << ", ";
    }
  }
  if (row_count > limit) {
    oss << ", ...";
  }
  oss << "]";
  return oss.str();
}

std::string dump_chunk(const Table &table, Chunk &chunk, int sample_limit = 5)
{
  std::ostringstream oss;
  const TableMeta    &meta = table.table_meta();
  oss << "rows=" << chunk.rows() << ", column_num=" << chunk.column_num() << ", columns=[";
  for (int col = 0; col < chunk.column_num(); ++col) {
    if (col > 0) {
      oss << "; ";
    }
    const int        field_id   = chunk.column_ids(col);
    const FieldMeta *field_meta = find_field_by_id(meta, field_id);
    const char      *field_name = field_meta != nullptr ? field_meta->name() : "<unknown>";
    const Column    &column     = chunk.column(col);
    oss << field_name << "(" << attr_type_to_string(column.attr_type()) << ")="
        << dump_column_samples(column, sample_limit);
  }
  oss << "]";
  return oss.str();
}

}  // namespace

RC TableScanVecPhysicalOperator::open(Trx *trx)
{
  RC rc = table_->get_chunk_scanner(chunk_scanner_, trx, mode_);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get chunk scanner", strrc(rc));
    return rc;
  }
  // TODO: don't need to fetch all columns from record manager
  for (int i = 0; i < table_->table_meta().field_num(); ++i) {
    all_columns_.add_column(
        make_unique<Column>(*table_->table_meta().field(i)), table_->table_meta().field(i)->field_id());
    filterd_columns_.add_column(
        make_unique<Column>(*table_->table_meta().field(i)), table_->table_meta().field(i)->field_id());
  }
  return rc;
}

RC TableScanVecPhysicalOperator::next(Chunk &chunk)
{
  RC rc = RC::SUCCESS;

  all_columns_.reset_data();
  filterd_columns_.reset_data();
  if (OB_SUCC(rc = chunk_scanner_.next_chunk(all_columns_))) {
    select_.assign(all_columns_.rows(), 1);
    if (predicates_.empty()) {
      chunk.reference(all_columns_);
      exec_trace("[TableScanVec][Chunk] table=%s %s", table_->name(), dump_chunk(*table_, chunk).c_str());
    } else {
      exec_trace("[TableScanVec][RawChunk] table=%s %s", table_->name(), dump_chunk(*table_, all_columns_).c_str());
      rc = filter(all_columns_);
      if (rc != RC::SUCCESS) {
        LOG_TRACE("filtered failed=%s", strrc(rc));
        return rc;
      }
      // TODO: if all setted, it doesn't need to set one by one
      for (int i = 0; i < all_columns_.rows(); i++) {
        if (select_[i] == 0) {
          continue;
        }
        for (int j = 0; j < all_columns_.column_num(); j++) {
          filterd_columns_.column(j).append_value(
              all_columns_.column(filterd_columns_.column_ids(j)).get_value(i));
        }
      }
      chunk.reference(filterd_columns_);
      exec_trace("[TableScanVec][FilteredChunk] table=%s %s", table_->name(), dump_chunk(*table_, chunk).c_str());
    }
  }
  return rc;
}

RC TableScanVecPhysicalOperator::close() { return chunk_scanner_.close_scan(); }

string TableScanVecPhysicalOperator::param() const { return table_->name(); }

void TableScanVecPhysicalOperator::set_predicates(vector<unique_ptr<Expression>> &&exprs)
{
  predicates_ = std::move(exprs);
}

RC TableScanVecPhysicalOperator::filter(Chunk &chunk)
{
  RC rc = RC::SUCCESS;
  for (unique_ptr<Expression> &expr : predicates_) {
    rc = expr->eval(chunk, select_);
    if (rc != RC::SUCCESS) {
      return rc;
    }
  }
  return rc;
}
