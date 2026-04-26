/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/table/ob_block.h"
#include "oblsm/util/ob_coding.h"
#include "common/lang/memory.h"

namespace oceanbase {

RC ObBlock::decode(const string &data)
{
  if (data.size() < sizeof(uint32_t)) {
    return RC::INVALID_ARGUMENT;
  }

  uint32_t data_size = get_numeric<uint32_t>(data.data() + data.size() - sizeof(uint32_t));
  if (data_size + 2 * sizeof(uint32_t) > data.size()) {
    return RC::INVALID_ARGUMENT;
  }

  uint32_t offset_count = get_numeric<uint32_t>(data.data() + data_size);
  size_t   expect_size  = data_size + sizeof(uint32_t) + offset_count * sizeof(uint32_t) + sizeof(uint32_t);
  if (expect_size != data.size()) {
    return RC::INVALID_ARGUMENT;
  }

  data_.assign(data.data(), data_size);
  offsets_.clear();
  offsets_.reserve(offset_count);
  const char *offset_ptr = data.data() + data_size + sizeof(uint32_t);
  for (uint32_t i = 0; i < offset_count; ++i) {
    offsets_.push_back(get_numeric<uint32_t>(offset_ptr));
    offset_ptr += sizeof(uint32_t);
  }
  return RC::SUCCESS;
}

string_view ObBlock::get_entry(uint32_t offset) const
{
  uint32_t    curr_begin = offsets_[offset];
  uint32_t    curr_end   = offset == offsets_.size() - 1 ? data_.size() : offsets_[offset + 1];
  string_view curr       = string_view(data_.data() + curr_begin, curr_end - curr_begin);
  return curr;
}

ObLsmIterator *ObBlock::new_iterator() const { return new BlockIterator(comparator_, this, size()); }

void BlockIterator::parse_entry()
{
  curr_entry_         = data_->get_entry(index_);
  uint32_t key_size   = get_numeric<uint32_t>(curr_entry_.data());
  key_                = string_view(curr_entry_.data() + sizeof(uint32_t), key_size);
  uint32_t value_size = get_numeric<uint32_t>(curr_entry_.data() + sizeof(uint32_t) + key_size);
  value_              = string_view(curr_entry_.data() + 2 * sizeof(uint32_t) + key_size, value_size);
}

string BlockMeta::encode() const
{
  string ret;
  put_numeric<uint32_t>(&ret, first_key_.size());
  ret.append(first_key_);
  put_numeric<uint32_t>(&ret, last_key_.size());
  ret.append(last_key_);
  put_numeric<uint32_t>(&ret, offset_);
  put_numeric<uint32_t>(&ret, size_);
  return ret;
}

RC BlockMeta::decode(const string &data)
{
  RC rc = RC::SUCCESS;
  const char *data_ptr       = data.c_str();
  uint32_t    first_key_size = get_numeric<uint32_t>(data_ptr);
  data_ptr += sizeof(uint32_t);
  first_key_.assign(data_ptr, first_key_size);
  data_ptr += first_key_size;
  uint32_t last_key_size = get_numeric<uint32_t>(data_ptr);
  data_ptr += sizeof(uint32_t);
  last_key_.assign(data_ptr, last_key_size);
  data_ptr += last_key_size;
  offset_ = get_numeric<uint32_t>(data_ptr);
  data_ptr += sizeof(uint32_t);
  size_ = get_numeric<uint32_t>(data_ptr);
  return rc;
}

void BlockIterator::seek(const string_view &lookup_key)
{
   index_ = 0;
   while(valid()) {
    parse_entry();
    if (comparator_->compare(extract_user_key(key_), extract_user_key_from_lookup_key(lookup_key)) >= 0) {
      break;
    }
    index_++;
   }
}
}  // namespace oceanbase
