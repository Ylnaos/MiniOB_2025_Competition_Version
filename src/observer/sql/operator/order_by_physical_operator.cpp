/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/order_by_physical_operator.h"
#include "common/log/log.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

using namespace std;

OrderByPhysicalOperator::OrderByPhysicalOperator(vector<OrderItem> &&order_by_items)
    : order_by_items_(std::move(order_by_items)), merge_heap_(MergeComparator{this})
{}

bool OrderByPhysicalOperator::MergeComparator::operator()(int lhs, int rhs) const
{
  const RunCursor &left_cursor  = *op->run_cursors_[lhs];
  const RunCursor &right_cursor = *op->run_cursors_[rhs];
  return op->compare_rows(left_cursor.current, right_cursor.current) > 0;
}

RC OrderByPhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 1) {
    LOG_WARN("order by expects exactly one child, got %zu", children_.size());
    return RC::INTERNAL;
  }

  rows_.clear();
  run_files_.clear();
  run_cursors_.clear();
  shared_specs_.reset();
  current_index_               = 0;
  use_external_sort_           = false;
  has_current_external_row_    = false;
  current_external_row_        = RowWithKeys();
  chunk_limit_rows_            = 0;
  merge_heap_                  = MergeHeap(MergeComparator{this});

  RC rc = children_[0]->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open child operator. rc=%s", strrc(rc));
    return rc;
  }

  while (RC::SUCCESS == (rc = children_[0]->next())) {
    Tuple *child_tuple = children_[0]->current_tuple();
    if (child_tuple == nullptr) {
      LOG_WARN("child returned null tuple");
      rc = RC::INTERNAL;
      break;
    }

    if (!shared_specs_) {
      auto specs_holder = std::make_shared<vector<TupleCellSpec>>();
      specs_holder->reserve(child_tuple->cell_num());
      for (int i = 0; i < child_tuple->cell_num(); i++) {
        TupleCellSpec spec;
        RC            tmp_rc = child_tuple->spec_at(i, spec);
        if (OB_FAIL(tmp_rc)) {
          LOG_WARN("failed to fetch tuple spec. rc=%s", strrc(tmp_rc));
          rc = tmp_rc;
          break;
        }
        specs_holder->push_back(spec);
      }
      if (OB_FAIL(rc)) {
        break;
      }
      shared_specs_ = std::move(specs_holder);
    }

    if (chunk_limit_rows_ == 0) {
      constexpr size_t TARGET_CHUNK_BYTES = 32 * 1024 * 1024; // 32MB
      size_t           cell_num           = static_cast<size_t>(child_tuple->cell_num());
      size_t           key_num            = std::max<size_t>(1, order_by_items_.size());
      size_t           estimated_row_bytes =
          std::max<size_t>(sizeof(Value) * (cell_num + key_num), 32 * (cell_num + key_num));
      chunk_limit_rows_ = std::max<size_t>(1024, TARGET_CHUNK_BYTES / std::max<size_t>(1, estimated_row_bytes));
    }

    RowWithKeys row_with_keys;
    row_with_keys.row.set_shared_specs(shared_specs_);
    rc = ValueListTuple::make(*child_tuple, row_with_keys.row);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to materialize tuple. rc=%s", strrc(rc));
      break;
    }

    rc = compute_sort_keys(row_with_keys.row, row_with_keys.sort_keys);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to compute order by keys. rc=%s", strrc(rc));
      break;
    }

    rows_.emplace_back(std::move(row_with_keys));

    if (rows_.size() >= chunk_limit_rows_) {
      rc = flush_rows_to_run();
      if (OB_FAIL(rc)) {
        break;
      }
      use_external_sort_ = true;
    }
  }

  if (rc != RC::RECORD_EOF) {
    if (rc == RC::SUCCESS) {
      rc = RC::RECORD_EOF;
    } else {
      LOG_WARN("child next failed. rc=%s", strrc(rc));
      cleanup_external_resources();
      children_[0]->close();
      return rc;
    }
  }

  if (use_external_sort_) {
    // flush remaining rows to disk
    RC flush_rc = flush_rows_to_run();
    if (OB_FAIL(flush_rc)) {
      cleanup_external_resources();
      children_[0]->close();
      return flush_rc;
    }

    // prepare merge readers
    int idx = 0;
    for (const auto &run_path : run_files_) {
      auto cursor = std::make_unique<RunCursor>();
      cursor->file_path = run_path.string();
      cursor->file      = fopen(cursor->file_path.c_str(), "rb");
      if (cursor->file == nullptr) {
        LOG_WARN("failed to open run file %s, error=%s", cursor->file_path.c_str(), strerror(errno));
        cleanup_external_resources();
        children_[0]->close();
        return RC::IOERR_OPEN;
      }
      cursor->current.row.set_shared_specs(shared_specs_);
      RC read_rc = read_next_row(*cursor);
      if (read_rc == RC::RECORD_EOF) {
        fclose(cursor->file);
        cursor->file = nullptr;
        continue;
      } else if (OB_FAIL(read_rc)) {
        LOG_WARN("failed to read first row from run %s. rc=%s", cursor->file_path.c_str(), strrc(read_rc));
        fclose(cursor->file);
        cleanup_external_resources();
        children_[0]->close();
        return read_rc;
      }

      run_cursors_.push_back(std::move(cursor));
      merge_heap_.push(idx++);
    }

    if (merge_heap_.empty()) {
      cleanup_external_resources();
      children_[0]->close();
      return RC::RECORD_EOF;
    }
  } else {
    // in-memory sort
    std::sort(rows_.begin(), rows_.end(), [this](const RowWithKeys &a, const RowWithKeys &b) {
      return compare_rows(a, b) < 0;
    });
  }

  opened_        = true;
  current_index_ = 0;
  return RC::SUCCESS;
}

RC OrderByPhysicalOperator::compute_sort_keys(const Tuple &tuple, vector<Value> &keys) const
{
  keys.clear();
  keys.reserve(order_by_items_.size());

  for (const auto &item : order_by_items_) {
    Value key_value;
    RC    rc = item.first->get_value(tuple, key_value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to evaluate order by key, fallback to NULL. rc=%s", strrc(rc));
      key_value.set_null();
    }
    keys.push_back(std::move(key_value));
  }
  return RC::SUCCESS;
}

int OrderByPhysicalOperator::compare_keys(const vector<Value> &lhs, const vector<Value> &rhs) const
{
  ASSERT(lhs.size() == rhs.size(), "key size mismatch");
  for (size_t i = 0; i < order_by_items_.size(); ++i) {
    bool asc = order_by_items_[i].second;
    int  cmp = 0;
    Value::compare(lhs[i], rhs[i], cmp);
    if (cmp == 0) {
      continue;
    }
    return asc ? cmp : -cmp;
  }
  return 0;
}

int OrderByPhysicalOperator::compare_rows(const RowWithKeys &lhs, const RowWithKeys &rhs) const
{
  return compare_keys(lhs.sort_keys, rhs.sort_keys);
}

RC OrderByPhysicalOperator::flush_rows_to_run()
{
  if (rows_.empty()) {
    return RC::SUCCESS;
  }

  std::sort(rows_.begin(), rows_.end(), [this](const RowWithKeys &a, const RowWithKeys &b) {
    return compare_rows(a, b) < 0;
  });

  RC rc = ensure_temp_dir();
  if (OB_FAIL(rc)) {
    return rc;
  }

  char filename[64];
  snprintf(filename, sizeof(filename), "order_run_%05zu.bin", run_files_.size());
  std::filesystem::path run_path = temp_dir_path_ / filename;
  FILE *file = fopen(run_path.c_str(), "wb");
  if (file == nullptr) {
    LOG_WARN("failed to create run file %s, error=%s", run_path.c_str(), strerror(errno));
    return RC::IOERR_OPEN;
  }

  for (const RowWithKeys &row : rows_) {
    rc = write_row(file, row.row);
    if (OB_FAIL(rc)) {
      fclose(file);
      std::error_code ec;
      std::filesystem::remove(run_path, ec);
      return rc;
    }
  }

  fclose(file);
  run_files_.push_back(run_path);
  rows_.clear();
  return RC::SUCCESS;
}

RC OrderByPhysicalOperator::write_row(FILE *file, const ValueListTuple &tuple) const
{
  const int cell_num = tuple.cell_num();
  if (cell_num < 0) {
    return RC::INVALID_ARGUMENT;
  }

  int32_t cell_num32 = static_cast<int32_t>(cell_num);
  if (fwrite(&cell_num32, sizeof(cell_num32), 1, file) != 1) {
    LOG_WARN("failed to write cell count");
    return RC::IOERR_WRITE;
  }

  for (int i = 0; i < cell_num; ++i) {
    Value value;
    RC    rc = tuple.cell_at(i, value);
    if (OB_FAIL(rc)) {
      return rc;
    }

    int32_t attr = static_cast<int32_t>(value.attr_type());
    if (fwrite(&attr, sizeof(attr), 1, file) != 1) {
      LOG_WARN("failed to write attr type");
      return RC::IOERR_WRITE;
    }

    uint8_t is_null = (value.attr_type() == AttrType::NULLS);
    if (fwrite(&is_null, sizeof(is_null), 1, file) != 1) {
      LOG_WARN("failed to write null flag");
      return RC::IOERR_WRITE;
    }

    if (!is_null) {
      int32_t len = value.length();
      if (fwrite(&len, sizeof(len), 1, file) != 1) {
        LOG_WARN("failed to write value length");
        return RC::IOERR_WRITE;
      }
      if (len > 0) {
        if (fwrite(value.data(), 1, len, file) != static_cast<size_t>(len)) {
          LOG_WARN("failed to write value payload");
          return RC::IOERR_WRITE;
        }
      }
    }
  }

  return RC::SUCCESS;
}

RC OrderByPhysicalOperator::read_next_row(RunCursor &cursor)
{
  cursor.current = RowWithKeys();
  cursor.current.row.set_shared_specs(shared_specs_);
  RC rc = deserialize_row(cursor.file, cursor.current.row);
  if (rc == RC::RECORD_EOF || OB_FAIL(rc)) {
    return rc;
  }

  rc = compute_sort_keys(cursor.current.row, cursor.current.sort_keys);
  if (OB_FAIL(rc)) {
    return rc;
  }
  return RC::SUCCESS;
}

RC OrderByPhysicalOperator::deserialize_row(FILE *file, ValueListTuple &tuple)
{
  int32_t cell_num32 = 0;
  size_t  read_count = fread(&cell_num32, sizeof(cell_num32), 1, file);
  if (read_count != 1) {
    if (feof(file)) {
      return RC::RECORD_EOF;
    }
    LOG_WARN("failed to read cell count, error=%s", strerror(errno));
    return RC::IOERR_READ;
  }

  vector<Value> cells;
  cells.reserve(cell_num32);

  for (int32_t i = 0; i < cell_num32; ++i) {
    int32_t attr_raw = 0;
    if (fread(&attr_raw, sizeof(attr_raw), 1, file) != 1) {
      LOG_WARN("failed to read attr type");
      return RC::IOERR_READ;
    }
    AttrType attr = static_cast<AttrType>(attr_raw);

    uint8_t is_null = 0;
    if (fread(&is_null, sizeof(is_null), 1, file) != 1) {
      LOG_WARN("failed to read null flag");
      return RC::IOERR_READ;
    }

    if (is_null) {
      Value value;
      value.set_null();
      cells.push_back(std::move(value));
      continue;
    }

    int32_t len = 0;
    if (fread(&len, sizeof(len), 1, file) != 1) {
      LOG_WARN("failed to read value length");
      return RC::IOERR_READ;
    }

    vector<char> buffer;
    buffer.resize(std::max<int32_t>(len, 0));
    if (len > 0) {
      if (fread(buffer.data(), 1, len, file) != static_cast<size_t>(len)) {
        LOG_WARN("failed to read value payload");
        return RC::IOERR_READ;
      }
    }

    Value value;
    switch (attr) {
      case AttrType::INTS: {
        int32_t v = 0;
        if (len > 0) {
          memcpy(&v, buffer.data(), std::min<int32_t>(len, static_cast<int32_t>(sizeof(v))));
        }
        value.set_int(v);
      } break;
      case AttrType::FLOATS: {
        float v = 0;
        if (len > 0) {
          memcpy(&v, buffer.data(), std::min<int32_t>(len, static_cast<int32_t>(sizeof(v))));
        }
        value.set_float(v);
      } break;
      case AttrType::DATES: {
        int32_t v = 0;
        if (len > 0) {
          memcpy(&v, buffer.data(), std::min<int32_t>(len, static_cast<int32_t>(sizeof(v))));
        }
        value.set_date(v);
      } break;
      case AttrType::BOOLEANS: {
        bool v = false;
        if (len > 0) {
          v = buffer[0] != 0;
        }
        value.set_boolean(v);
      } break;
      case AttrType::CHARS:
      case AttrType::TEXTS:
      case AttrType::VECTORS: {
        value.set_type(attr);
        value.set_data(buffer.data(), len);
      } break;
      default: {
        LOG_WARN("unsupported attribute type in external sort: %d", static_cast<int>(attr));
        return RC::UNIMPLEMENTED;
      } break;
    }
    cells.push_back(std::move(value));
  }

  tuple.set_cells(std::move(cells));
  return RC::SUCCESS;
}

RC OrderByPhysicalOperator::ensure_temp_dir()
{
  if (temp_dir_ready_) {
    return RC::SUCCESS;
  }

  std::error_code ec;
  std::filesystem::path base_dir = std::filesystem::temp_directory_path(ec);
  if (ec) {
    LOG_WARN("failed to get temp directory: %s", ec.message().c_str());
    return RC::IOERR_ACCESS;
  }

  std::filesystem::path root_dir = base_dir / "miniob_order_by";
  std::filesystem::create_directories(root_dir, ec);
  if (ec) {
    LOG_WARN("failed to create root temp directory %s: %s", root_dir.c_str(), ec.message().c_str());
    return RC::IOERR_ACCESS;
  }

  auto        now      = std::chrono::steady_clock::now().time_since_epoch().count();
  std::string dir_name = std::to_string(reinterpret_cast<uintptr_t>(this)) + "_" + std::to_string(now);
  std::filesystem::path uniq_dir = root_dir / dir_name;
  std::filesystem::create_directories(uniq_dir, ec);
  if (ec) {
    LOG_WARN("failed to create unique temp directory %s: %s", uniq_dir.c_str(), ec.message().c_str());
    return RC::IOERR_ACCESS;
  }

  temp_dir_path_  = std::move(uniq_dir);
  temp_dir_ready_ = true;
  return RC::SUCCESS;
}

void OrderByPhysicalOperator::cleanup_external_resources()
{
  for (auto &cursor : run_cursors_) {
    if (cursor && cursor->file != nullptr) {
      fclose(cursor->file);
      cursor->file = nullptr;
    }
  }
  run_cursors_.clear();

  merge_heap_ = MergeHeap(MergeComparator{this});

  for (const auto &path : run_files_) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
  run_files_.clear();

  if (temp_dir_ready_) {
    std::error_code ec;
    std::filesystem::remove(temp_dir_path_, ec);
    temp_dir_ready_ = false;
  }

  has_current_external_row_ = false;
  current_external_row_     = RowWithKeys();
}

RC OrderByPhysicalOperator::next()
{
  if (!opened_) {
    return RC::INTERNAL;
  }

  if (!use_external_sort_) {
    if (rows_.empty()) {
      return RC::RECORD_EOF;
    }

    if (current_index_ >= rows_.size()) {
      return RC::RECORD_EOF;
    }

    if (current_index_ == 0) {
      current_index_ = 1;
    } else {
      ++current_index_;
    }

    if (current_index_ > rows_.size()) {
      return RC::RECORD_EOF;
    }
    return RC::SUCCESS;
  }

  if (merge_heap_.empty()) {
    has_current_external_row_ = false;
    return RC::RECORD_EOF;
  }

  int top_idx = merge_heap_.top();
  merge_heap_.pop();

  RunCursor &cursor = *run_cursors_[top_idx];
  current_external_row_      = std::move(cursor.current);
  has_current_external_row_  = true;

  RC rc = read_next_row(cursor);
  if (rc == RC::SUCCESS) {
    merge_heap_.push(top_idx);
  } else if (rc == RC::RECORD_EOF) {
    if (cursor.file != nullptr) {
      fclose(cursor.file);
      cursor.file = nullptr;
    }
  } else {
    has_current_external_row_ = false;
    return rc;
  }

  ++current_index_;
  return RC::SUCCESS;
}

Tuple *OrderByPhysicalOperator::current_tuple()
{
  if (!opened_) {
    return nullptr;
  }

  if (!use_external_sort_) {
    if (rows_.empty()) {
      return nullptr;
    }
    size_t idx = (current_index_ == 0 ? 0 : current_index_ - 1);
    if (idx >= rows_.size()) {
      return nullptr;
    }
    return &rows_[idx].row;
  }

  if (!has_current_external_row_) {
    return nullptr;
  }
  return &current_external_row_.row;
}

RC OrderByPhysicalOperator::close()
{
  if (!children_.empty()) {
    children_[0]->close();
  }
  cleanup_external_resources();
  rows_.clear();
  shared_specs_.reset();
  opened_        = false;
  current_index_ = 0;
  return RC::SUCCESS;
}
