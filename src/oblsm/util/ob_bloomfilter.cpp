/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/util/ob_bloomfilter.h"
#include "oblsm/util/ob_coding.h"
#include <cstring>

namespace oceanbase {

static const uint32_t BLOOMFILTER_VERSION = 1;

ObBloomfilter::ObBloomfilter(size_t hash_func_count, size_t total_bits)
    : hash_func_count_(hash_func_count), total_bits_(total_bits)
{
  // Calculate the number of 64-bit integers needed to store total_bits
  size_t array_size = (total_bits + 63) / 64;
  bit_array_.resize(array_size, 0);
  object_count_.store(0);
}

void ObBloomfilter::compute_hash(const string &object, size_t hash_index, size_t &bit_index) const
{
  // Use double hashing: h(i) = hash1 + i * hash2
  std::hash<string> hasher;
  uint64_t hash1 = hasher(object);
  uint64_t hash2 = hash1 * 0x5bd1e995;  // Simple secondary hash

  uint64_t hash = (hash1 + hash_index * hash2) % total_bits_;
  bit_index = static_cast<size_t>(hash);
}

void ObBloomfilter::insert(const string &object)
{
  std::lock_guard<std::mutex> lock(mutex_);

  for (size_t i = 0; i < hash_func_count_; ++i) {
    size_t bit_index;
    compute_hash(object, i, bit_index);

    // Set the bit: bit_array_[word_index] |= (1ULL << bit_offset)
    size_t word_index = bit_index / 64;
    size_t bit_offset = bit_index % 64;
    bit_array_[word_index] |= (1ULL << bit_offset);
  }

  object_count_.fetch_add(1);
}

bool ObBloomfilter::contains(const string &object) const
{
  for (size_t i = 0; i < hash_func_count_; ++i) {
    size_t bit_index;
    compute_hash(object, i, bit_index);

    // Check the bit
    size_t word_index = bit_index / 64;
    size_t bit_offset = bit_index % 64;

    if ((bit_array_[word_index] & (1ULL << bit_offset)) == 0) {
      return false;  // Definitely not in the set
    }
  }

  return true;  // Might be in the set
}

void ObBloomfilter::clear()
{
  std::lock_guard<std::mutex> lock(mutex_);

  // Reset all bits to 0
  std::fill(bit_array_.begin(), bit_array_.end(), 0);
  object_count_.store(0);
}

string ObBloomfilter::encode() const
{
  string result;

  // Version
  put_numeric<uint32_t>(&result, BLOOMFILTER_VERSION);

  // Hash function count
  put_numeric<uint32_t>(&result, static_cast<uint32_t>(hash_func_count_));

  // Total bits
  put_numeric<uint64_t>(&result, static_cast<uint64_t>(total_bits_));

  // Bit array size
  put_numeric<uint32_t>(&result, static_cast<uint32_t>(bit_array_.size()));

  // Bit array data
  for (uint64_t bits : bit_array_) {
    put_numeric<uint64_t>(&result, bits);
  }

  // Object count
  put_numeric<uint64_t>(&result, static_cast<uint64_t>(object_count_.load()));

  return result;
}

RC ObBloomfilter::decode(const string &data)
{
  const char *ptr = data.c_str();
  const char *end = ptr + data.size();

  // Check minimum size
  if (data.size() < sizeof(uint32_t) * 3 + sizeof(uint64_t) * 2) {
    return RC::INTERNAL;
  }

  // Read version
  uint32_t version = get_numeric<uint32_t>(ptr);
  ptr += sizeof(uint32_t);

  if (version != BLOOMFILTER_VERSION) {
    return RC::INTERNAL;
  }

  // Read hash function count
  hash_func_count_ = get_numeric<uint32_t>(ptr);
  ptr += sizeof(uint32_t);

  // Read total bits
  total_bits_ = get_numeric<uint64_t>(ptr);
  ptr += sizeof(uint64_t);

  // Read bit array size
  uint32_t array_size = get_numeric<uint32_t>(ptr);
  ptr += sizeof(uint32_t);

  // Check if enough data remains for bit array
  if (ptr + array_size * sizeof(uint64_t) + sizeof(uint64_t) > end) {
    return RC::INTERNAL;
  }

  // Read bit array
  bit_array_.clear();
  bit_array_.reserve(array_size);
  for (uint32_t i = 0; i < array_size; ++i) {
    uint64_t bits = get_numeric<uint64_t>(ptr);
    ptr += sizeof(uint64_t);
    bit_array_.push_back(bits);
  }

  // Read object count
  size_t count = get_numeric<uint64_t>(ptr);
  object_count_.store(count);

  return RC::SUCCESS;
}

}  // namespace oceanbase
