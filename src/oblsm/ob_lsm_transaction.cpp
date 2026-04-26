/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/include/ob_lsm_transaction.h"
#include "oblsm/util/ob_comparator.h"
#include "common/lang/memory.h"

namespace oceanbase {

/**
 * @brief merge TrxInnerMapIterator and ObUserIterator
 * @details Merges two iterators of different types into one.
 * If the two iterators have the same key, only
 * produce the key once and prefer the entry from left.
 */
class TrxIterator : public ObLsmIterator
{
public:
  explicit TrxIterator(map<string, string> &&entries) : entries_(std::move(entries)), iter_(entries_.end()) {}
  ~TrxIterator() override = default;

  bool valid() const override { return iter_ != entries_.end(); }
  void seek_to_first() override { iter_ = entries_.begin(); }
  void seek_to_last() override
  {
    if (entries_.empty()) {
      iter_ = entries_.end();
    } else {
      iter_ = entries_.end();
      --iter_;
    }
  }
  void seek(const string_view &key) override { iter_ = entries_.lower_bound(string(key)); }
  void next() override
  {
    if (iter_ != entries_.end()) {
      ++iter_;
    }
  }

  string_view key() const override { return iter_->first; }
  string_view value() const override { return iter_->second; }

private:
  map<string, string>                 entries_;
  map<string, string>::const_iterator iter_;
};

ObLsmTransaction::ObLsmTransaction(ObLsm *db, uint64_t ts) : db_(db), ts_(ts)
{
  (void)db_;
  (void)ts_;
}

RC ObLsmTransaction::get(const string_view &key, string *value)
{
  auto iter = inner_store_.find(string(key));
  if (iter != inner_store_.end()) {
    if (iter->second.empty()) {
      return RC::NOT_EXIST;
    }
    *value = iter->second;
    return RC::SUCCESS;
  }
  ObLsmReadOptions options;
  options.seq = ts_;
  unique_ptr<ObLsmIterator> db_iter(db_->new_iterator(options));
  db_iter->seek(key);
  if (!db_iter->valid() || db_iter->key() != key) {
    return RC::NOT_EXIST;
  }
  *value = string(db_iter->value());
  return value->empty() ? RC::NOT_EXIST : RC::SUCCESS;
}

RC ObLsmTransaction::put(const string_view &key, const string_view &value)
{
  inner_store_[string(key)] = string(value);
  return RC::SUCCESS;
}

RC ObLsmTransaction::remove(const string_view &key)
{
  inner_store_[string(key)] = string();
  return RC::SUCCESS;
}

ObLsmIterator *ObLsmTransaction::new_iterator(ObLsmReadOptions options)
{
  if (options.seq == -1) {
    options.seq = ts_;
  }
  map<string, string> entries;
  unique_ptr<ObLsmIterator> iter(db_->new_iterator(options));
  for (iter->seek_to_first(); iter->valid(); iter->next()) {
    entries[string(iter->key())] = string(iter->value());
  }
  for (const auto &entry : inner_store_) {
    if (entry.second.empty()) {
      entries.erase(entry.first);
    } else {
      entries[entry.first] = entry.second;
    }
  }
  auto trx_iter = new TrxIterator(std::move(entries));
  trx_iter->seek_to_first();
  return trx_iter;
}

RC ObLsmTransaction::commit()
{
  for (const auto &entry : inner_store_) {
    RC rc = entry.second.empty() ? db_->remove(entry.first) : db_->put(entry.first, entry.second);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }
  inner_store_.clear();
  return RC::SUCCESS;
}

RC ObLsmTransaction::rollback()
{
  inner_store_.clear();
  return RC::SUCCESS;
}

}  // namespace oceanbase
