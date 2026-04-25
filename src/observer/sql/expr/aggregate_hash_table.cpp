/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/expr/aggregate_hash_table.h"
#include "sql/expr/aggregate_state.h"

// ----------------------------------StandardAggregateHashTable------------------

RC StandardAggregateHashTable::add_chunk(Chunk &groups_chunk, Chunk &aggrs_chunk)
{
    if (groups_chunk.rows() != aggrs_chunk.rows()) {
    LOG_WARN("groups_chunk and aggrs_chunk have different rows: %d, %d", groups_chunk.rows(), aggrs_chunk.rows());
    return RC::INVALID_ARGUMENT;
  }
  for (int i = 0; i < groups_chunk.rows(); i++) {
    vector<Value> group_by_values;
    vector<void*> aggr_values;

    for (int j = 0; j < groups_chunk.column_num(); j++) {
      group_by_values.emplace_back(groups_chunk.get_value(j, i));
    }

    auto it = aggr_values_.find(group_by_values);
    if (it == aggr_values_.end()) {
      for (size_t j = 0; j < aggr_types_.size(); j++) {
        void * state_ptr = create_aggregate_state(aggr_types_[j], aggr_child_types_[j]);
        if (state_ptr == nullptr) {
          LOG_WARN("create aggregate state failed");
          return RC::INTERNAL;
        }   
        aggr_values.emplace_back(state_ptr);
      }
      aggr_values_.emplace(group_by_values, aggr_values);
    }
    auto &aggr = aggr_values_.find(group_by_values)->second;
    for (size_t aggr_idx = 0; aggr_idx < aggr.size(); aggr_idx++) {
      RC rc = aggregate_state_update_by_value(aggr[aggr_idx], aggr_types_[aggr_idx], aggr_child_types_[aggr_idx], aggrs_chunk.get_value(aggr_idx, i));
      if (rc != RC::SUCCESS) {
        LOG_WARN("update aggregate state failed");
        return rc;
      }
    }
  }
  return RC::SUCCESS;
}

void StandardAggregateHashTable::Scanner::open_scan()
{
  it_  = static_cast<StandardAggregateHashTable *>(hash_table_)->begin();
  end_ = static_cast<StandardAggregateHashTable *>(hash_table_)->end();
}

RC StandardAggregateHashTable::Scanner::next(Chunk &output_chunk)
{
  RC rc = RC::SUCCESS;
  if (it_ == end_) {
    return RC::RECORD_EOF;
  }
  while (it_ != end_ && output_chunk.rows() < output_chunk.capacity()) {
    auto &group_by_values = it_->first;
    auto &aggrs           = it_->second;
    for (int i = 0; i < output_chunk.column_num(); i++) {
      auto col_idx = output_chunk.column_ids(i);
      if (col_idx >= static_cast<int>(group_by_values.size())) {
        int aggr_real_idx = col_idx - group_by_values.size();
        rc = finialize_aggregate_state(aggrs[aggr_real_idx], hash_table_->aggr_types_[aggr_real_idx],
                                       hash_table_->aggr_child_types_[aggr_real_idx], output_chunk.column(i));
        if (rc != RC::SUCCESS) {
          LOG_WARN("finialize aggregate state failed");
          return rc;
        }
      } else {
        if (OB_FAIL(output_chunk.column(i).append_value(group_by_values[col_idx]))) {
          LOG_WARN("append value failed");
          return rc;
        }
      }
    }
    it_++;
  }
  if (it_ == end_) {
    return RC::SUCCESS;
  }

  return RC::SUCCESS;
}

size_t StandardAggregateHashTable::VectorHash::operator()(const vector<Value> &vec) const
{
  size_t hash_val = 0;
  for (const auto &elem : vec) {
    hash_val ^= hash<string>()(elem.to_string());
  }
  return hash_val;
}

bool StandardAggregateHashTable::VectorEqual::operator()(const vector<Value> &lhs, const vector<Value> &rhs) const
{
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.size(); ++i) {
    if (rhs[i].compare(lhs[i]) != 0) {
      return false;
    }
  }
  return true;
}

// ----------------------------------LinearProbingAggregateHashTable------------------
#ifdef USE_SIMD
template <typename V>
RC LinearProbingAggregateHashTable<V>::add_chunk(Chunk &group_chunk, Chunk &aggr_chunk)
{
  if (group_chunk.column_num() != 1 || aggr_chunk.column_num() != 1) {
    LOG_WARN("group_chunk and aggr_chunk size must be 1.");
    return RC::INVALID_ARGUMENT;
  }
  if (group_chunk.rows() != aggr_chunk.rows()) {
    LOG_WARN("group_chunk and aggr _chunk rows must be equal.");
    return RC::INVALID_ARGUMENT;
  }
  add_batch((int *)group_chunk.column(0).data(), (V *)aggr_chunk.column(0).data(), group_chunk.rows());
  return RC::SUCCESS;
}

template <typename V>
void LinearProbingAggregateHashTable<V>::Scanner::open_scan()
{
  capacity_   = static_cast<LinearProbingAggregateHashTable *>(hash_table_)->capacity();
  size_       = static_cast<LinearProbingAggregateHashTable *>(hash_table_)->size();
  scan_pos_   = 0;
  scan_count_ = 0;
}

template <typename V>
RC LinearProbingAggregateHashTable<V>::Scanner::next(Chunk &output_chunk)
{
  if (scan_pos_ >= capacity_ || scan_count_ >= size_) {
    return RC::RECORD_EOF;
  }
  auto linear_probing_hash_table = static_cast<LinearProbingAggregateHashTable *>(hash_table_);
  while (scan_pos_ < capacity_ && scan_count_ < size_ && output_chunk.rows() <= output_chunk.capacity()) {
    int key;
    V   value;
    RC  rc = linear_probing_hash_table->iter_get(scan_pos_, key, value);
    if (rc == RC::SUCCESS) {
      output_chunk.column(0).append_one((char *)&key);
      output_chunk.column(1).append_one((char *)&value);
      scan_count_++;
    }
    scan_pos_++;
  }
  return RC::SUCCESS;
}

template <typename V>
void LinearProbingAggregateHashTable<V>::Scanner::close_scan()
{
  capacity_   = -1;
  size_       = -1;
  scan_pos_   = -1;
  scan_count_ = 0;
}

template <typename V>
RC LinearProbingAggregateHashTable<V>::get(int key, V &value)
{
  RC  rc          = RC::SUCCESS;
  int index       = (key % capacity_ + capacity_) % capacity_;
  int iterate_cnt = 0;
  while (true) {
    if (keys_[index] == EMPTY_KEY) {
      rc = RC::NOT_EXIST;
      break;
    } else if (keys_[index] == key) {
      value = values_[index];
      break;
    } else {
      index += 1;
      index %= capacity_;
      iterate_cnt++;
      if (iterate_cnt > capacity_) {
        rc = RC::NOT_EXIST;
        break;
      }
    }
  }
  return rc;
}

template <typename V>
RC LinearProbingAggregateHashTable<V>::iter_get(int pos, int &key, V &value)
{
  RC rc = RC::SUCCESS;
  if (keys_[pos] == LinearProbingAggregateHashTable<V>::EMPTY_KEY) {
    rc = RC::NOT_EXIST;
  } else {
    key   = keys_[pos];
    value = values_[pos];
  }
  return rc;
}

template <typename V>
void LinearProbingAggregateHashTable<V>::aggregate(V *value, V value_to_aggregate)
{
  if (aggregate_type_ == AggregateExpr::Type::SUM) {
    *value += value_to_aggregate;
  } else {
    ASSERT(false, "unsupported aggregate type");
  }
}

template <typename V>
void LinearProbingAggregateHashTable<V>::resize()
{
  capacity_ *= 2;
  vector<int> new_keys(capacity_);
  vector<V>   new_values(capacity_);

  for (size_t i = 0; i < keys_.size(); i++) {
    auto &key   = keys_[i];
    auto &value = values_[i];
    if (key != EMPTY_KEY) {
      int index = (key % capacity_ + capacity_) % capacity_;
      while (new_keys[index] != EMPTY_KEY) {
        index = (index + 1) % capacity_;
      }
      new_keys[index]   = key;
      new_values[index] = value;
    }
  }

  keys_   = std::move(new_keys);
  values_ = std::move(new_values);
}

template <typename V>
void LinearProbingAggregateHashTable<V>::resize_if_need()
{
  if (size_ >= capacity_ / 2) {
    resize();
  }
}

template <typename V>

void LinearProbingAggregateHashTable<V>::add_batch(int *input_keys, V *input_values, int len)
{
  if (len <= 0) {
    resize_if_need();
    return;
  }

  alignas(32) int key_lane[SIMD_WIDTH];
  alignas(32) V   value_lane[SIMD_WIDTH];
  alignas(32) int hash_index[SIMD_WIDTH];
  alignas(32) int table_key_array[SIMD_WIDTH];
  alignas(32) int empty_mask_array[SIMD_WIDTH];
  alignas(32) int match_mask_array[SIMD_WIDTH];
  int              inv[SIMD_WIDTH];
  int              off[SIMD_WIDTH];
  int              base_hash[SIMD_WIDTH];

  for (int lane = 0; lane < SIMD_WIDTH; lane++) {
    inv[lane]        = -1;
    off[lane]        = 0;
    base_hash[lane]  = 0;
    key_lane[lane]   = 0;
    value_lane[lane] = static_cast<V>(0);
  }

  int processed = 0;
  const __m256i empty_key_vec = _mm256_set1_epi32(EMPTY_KEY);

  auto has_active_lane = [&inv]() {
    for (int lane = 0; lane < SIMD_WIDTH; lane++) {
      if (inv[lane] == 0) {
        return true;
      }
    }
    return false;
  };

  while (processed < len || has_active_lane()) {
    int load_mask[SIMD_WIDTH];
    int remaining     = len - processed;
    int lanes_to_load = 0;

    for (int lane = 0; lane < SIMD_WIDTH; lane++) {
      if (inv[lane] == -1 && remaining > 0) {
        load_mask[lane] = -1;
        remaining--;
        lanes_to_load++;
      } else {
        load_mask[lane] = 0;
      }
    }

    if (lanes_to_load > 0) {
      __m256i load_inv = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(load_mask));
      selective_load(input_keys, processed, key_lane, load_inv);
      selective_load(input_values, processed, value_lane, load_inv);

      for (int lane = 0; lane < SIMD_WIDTH; lane++) {
        if (load_mask[lane] == -1) {
          inv[lane]       = 0;
          off[lane]       = 0;
          int base        = key_lane[lane] % capacity_;
          if (base < 0) {
            base += capacity_;
          }
          base_hash[lane] = base;
        }
      }
      processed += lanes_to_load;
    }

    if (!has_active_lane()) {
      if (processed >= len) {
        break;
      }
      continue;
    }

    for (int lane = 0; lane < SIMD_WIDTH; lane++) {
      if (inv[lane] == 0) {
        int idx = base_hash[lane] + off[lane];
        if (idx >= capacity_) {
          idx %= capacity_;
        }
        hash_index[lane] = idx;
      } else {
        hash_index[lane] = 0;
      }
    }

    __m256i hash_vec       = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(hash_index));
    __m256i table_key_vec  = _mm256_i32gather_epi32(reinterpret_cast<const int *>(keys_.data()), hash_vec, 4);
    __m256i key_vec        = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(key_lane));
    __m256i empty_mask_vec = _mm256_cmpeq_epi32(table_key_vec, empty_key_vec);
    __m256i match_mask_vec = _mm256_cmpeq_epi32(table_key_vec, key_vec);

    _mm256_storeu_si256(reinterpret_cast<__m256i *>(table_key_array), table_key_vec);
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(empty_mask_array), empty_mask_vec);
    _mm256_storeu_si256(reinterpret_cast<__m256i *>(match_mask_array), match_mask_vec);

    for (int lane = 0; lane < SIMD_WIDTH; lane++) {
      if (inv[lane] != 0) {
        continue;
      }

      int idx = hash_index[lane];
      if (empty_mask_array[lane] != 0) {
        keys_[idx]   = key_lane[lane];
        values_[idx] = value_lane[lane];
        size_++;
        inv[lane]       = -1;
        off[lane]       = 0;
        base_hash[lane] = 0;
        key_lane[lane]  = 0;
        value_lane[lane] = static_cast<V>(0);
      } else if (match_mask_array[lane] != 0) {
        aggregate(&values_[idx], value_lane[lane]);
        inv[lane]       = -1;
        off[lane]       = 0;
        base_hash[lane] = 0;
        key_lane[lane]  = 0;
        value_lane[lane] = static_cast<V>(0);
      } else {
        off[lane] += 1;
        if (off[lane] >= capacity_) {
          off[lane] %= capacity_;
        }
      }
    }
  }

  for (; processed < len; processed++) {
    int key   = input_keys[processed];
    V   value = input_values[processed];
    int index = key % capacity_;
    if (index < 0) {
      index += capacity_;
    }
    while (keys_[index] != EMPTY_KEY && keys_[index] != key) {
      index = (index + 1) % capacity_;
    }
    if (keys_[index] == EMPTY_KEY) {
      keys_[index]   = key;
      values_[index] = value;
      size_++;
    } else {
      aggregate(&values_[index], value);
    }
  }

  resize_if_need();
}



template <typename V>
const int LinearProbingAggregateHashTable<V>::EMPTY_KEY = 0xffffffff;
template <typename V>
const int LinearProbingAggregateHashTable<V>::DEFAULT_CAPACITY = 16384;

template class LinearProbingAggregateHashTable<int>;
template class LinearProbingAggregateHashTable<float>;
#endif
