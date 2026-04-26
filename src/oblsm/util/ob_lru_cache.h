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

#include <stdint.h>
#include <cstddef>
#include <iterator>
#include <list>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace oceanbase {

/**
 * @class ObLRUCache
 * @brief A thread-safe implementation of an LRU (Least Recently Used) cache.
 *
 * The `ObLRUCache` class provides a fixed-size cache that evicts the least recently used
 * entries when the cache exceeds its capacity. It supports thread-safe operations for
 * inserting, retrieving, and checking the existence of cache entries.
 *
 * @tparam KeyType The type of keys used to identify cache entries.
 * @tparam ValueType The type of values stored in the cache.
 */
template <typename KeyType, typename ValueType>
class ObLRUCache
{
public:
  /**
   * @brief Constructs an `ObLRUCache` with a specified capacity.
   *
   * @param capacity The maximum number of elements the cache can hold.
   */
  ObLRUCache(size_t capacity) : capacity_(capacity) {}

  /**
   * @brief Retrieves a value from the cache using the specified key.
   *
   * This method searches for the specified key in the cache. If the key is found, the
   * corresponding value is returned and the key-value pair is moved to the front of the
   * LRU list (indicating recent use).
   *
   * @param key The key to search for in the cache.
   * @param value A reference to store the value associated with the key.
   * @return `true` if the key is found and the value is retrieved; `false` otherwise.
   */
  bool get(const KeyType &key, ValueType &value)
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        iter = cache_.find(key);
    if (iter == cache_.end()) {
      return false;
    }

    items_.splice(items_.begin(), items_, iter->second);
    value = iter->second->second;
    return true;
  }

  /**
   * @brief Inserts a key-value pair into the cache.
   *
   * If the key already exists in the cache, its value is updated, and the key-value pair
   * is moved to the front of the LRU list. If the cache exceeds its capacity after insertion,
   * the least recently used entry is evicted.
   *
   * @param key The key to insert into the cache.
   * @param value The value to associate with the specified key.
   */
  void put(const KeyType &key, const ValueType &value)
  {
    if (capacity_ == 0) {
      return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto                        iter = cache_.find(key);
    if (iter != cache_.end()) {
      iter->second->second = value;
      items_.splice(items_.begin(), items_, iter->second);
      return;
    }

    items_.emplace_front(key, value);
    cache_[key] = items_.begin();
    if (cache_.size() > capacity_) {
      auto last = std::prev(items_.end());
      cache_.erase(last->first);
      items_.pop_back();
    }
  }

  /**
   * @brief Checks whether the specified key exists in the cache.
   *
   * @param key The key to check in the cache.
   * @return `true` if the key exists; `false` otherwise.
   */
  bool contains(const KeyType &key) const
  {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.find(key) != cache_.end();
  }

private:
  /**
   * @brief The maximum number of elements the cache can hold.
   */
  size_t capacity_;
  using ListType = std::list<std::pair<KeyType, ValueType>>;

  mutable std::mutex mutex_;
  ListType           items_;
  std::unordered_map<KeyType, typename ListType::iterator> cache_;
};

/**
 * @brief Creates a new instance of `ObLRUCache` with the specified capacity.
 *
 * This factory function constructs an `ObLRUCache` instance for the specified key and
 * value types, and initializes it with the given capacity.
 *
 * @tparam Key The type of keys used to identify cache entries.
 * @tparam Value The type of values stored in the cache.
 * @param capacity The maximum number of elements the cache can hold.
 * @return A pointer to the newly created `ObLRUCache` instance.
 */
template <typename Key, typename Value>
ObLRUCache<Key, Value> *new_lru_cache(uint32_t capacity)
{
  return new ObLRUCache<Key, Value>(capacity);
}

}  // namespace oceanbase
