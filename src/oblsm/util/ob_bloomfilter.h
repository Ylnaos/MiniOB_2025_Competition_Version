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
#include <atomic>
#include <mutex>
#include <functional>
#include "common/lang/string.h"
#include "common/sys/rc.h"

namespace oceanbase {

/**
 * @class ObBloomfilter
 * @brief A simple Bloom filter implementation(Need to support concurrency).
 */
class ObBloomfilter
{
public:
  /**
   * @brief Constructs a Bloom filter with specified parameters.
   *
   * @param hash_func_count Number of hash functions to use. Default is 4.
   * @param totoal_bits Total number of bits in the Bloom filter. Default is 65536.
   */
  ObBloomfilter(size_t hash_func_count = 4, size_t totoal_bits = 65536);

  /**
   * @brief Inserts an object into the Bloom filter.
   * @details This method computes hash values for the given object and sets corresponding bits in the filter.
   * @param object The object to be inserted.
   */
  void insert(const string &object);

  /**
   * @brief Clears all entries in the Bloom filter.
   *
   * @details Resets the filter, removing all previously inserted objects.
   */
  void clear();

  /**
   * @brief Checks if an object is possibly in the Bloom filter.
   *
   * @param object The object to be checked.
   * @return true if the object might be in the filter, false if definitely not.
   */
  bool contains(const string &object) const;

  /**
   * @brief Returns the count of objects inserted into the Bloom filter.
   */
  size_t object_count() const { return object_count_.load(); }

  /**
   * @brief Checks if the Bloom filter is empty.
   * @return true if the filter is empty, false otherwise.
   */
  bool empty() const { return object_count() == 0; }

  /**
   * @brief Serializes the Bloom filter to a string.
   * @return Serialized string representation of the filter.
   */
  string encode() const;

  /**
   * @brief Deserializes the Bloom filter from a string.
   * @param data Serialized data to decode.
   * @return RC::SUCCESS on success, error code otherwise.
   */
  RC decode(const string &data);

private:
  /**
   * @brief Computes hash values for a given object.
   * @param object The object to hash.
   * @param bit_index Output parameter for the bit index.
   */
  void compute_hash(const string &object, size_t hash_index, size_t &bit_index) const;

  std::vector<uint64_t> bit_array_;       // Bit array for storing filter data
  size_t hash_func_count_;                 // Number of hash functions
  size_t total_bits_;                      // Total number of bits
  std::atomic<size_t> object_count_{0};    // Count of inserted objects
  mutable std::mutex mutex_;               // Mutex for thread safety
};

}  // namespace oceanbase
