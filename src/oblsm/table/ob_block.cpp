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
  offsets_.clear();
  data_.clear();

  const size_t total_size = data.size();
  if (total_size < 2 * sizeof(uint32_t)) {
    return RC::INVALID_ARGUMENT;
  }

  const char *raw_data = data.data();
  const size_t trailer_offset = total_size - sizeof(uint32_t);
  const uint32_t data_size = get_numeric<uint32_t>(raw_data + trailer_offset);

  const size_t entry_region_size = static_cast<size_t>(data_size);

  if (entry_region_size > total_size) {
    return RC::INVALID_ARGUMENT;
  }

  const size_t metadata_len = total_size - entry_region_size;
  if (metadata_len < 2 * sizeof(uint32_t) || metadata_len % sizeof(uint32_t) != 0) {
    return RC::INVALID_ARGUMENT;
  }

  const char    *metadata_ptr = raw_data + entry_region_size;
  const uint32_t offset_count = get_numeric<uint32_t>(metadata_ptr);
  const size_t   expected_metadata_len = (static_cast<size_t>(offset_count) + 2) * sizeof(uint32_t);
  if (metadata_len != expected_metadata_len) {
    return RC::INVALID_ARGUMENT;
  }

  vector<uint32_t> parsed_offsets;
  parsed_offsets.reserve(offset_count);

  metadata_ptr += sizeof(uint32_t);
  for (uint32_t i = 0; i < offset_count; i++) {
    uint32_t offset = get_numeric<uint32_t>(metadata_ptr);
    metadata_ptr += sizeof(uint32_t);
    if (static_cast<size_t>(offset) >= entry_region_size) {
      return RC::INVALID_ARGUMENT;
    }
    if (i > 0 && offset <= parsed_offsets.back()) {
      return RC::INVALID_ARGUMENT;
    }
    parsed_offsets.push_back(offset);
  }

  const uint32_t data_size_footer = get_numeric<uint32_t>(metadata_ptr);
  if (data_size_footer != data_size) {
    return RC::INVALID_ARGUMENT;
  }

  data_.assign(raw_data, entry_region_size);
  offsets_.assign(parsed_offsets.begin(), parsed_offsets.end());

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
  const char *data_ptr  = data.data();
  size_t      remaining = data.size();

  if (remaining < sizeof(uint32_t)) {
    return RC::INVALID_ARGUMENT;
  }
  uint32_t first_key_size = get_numeric<uint32_t>(data_ptr);
  data_ptr += sizeof(uint32_t);
  remaining -= sizeof(uint32_t);
  if (first_key_size > remaining) {
    return RC::INVALID_ARGUMENT;
  }
  first_key_.assign(data_ptr, first_key_size);
  data_ptr += first_key_size;
  remaining -= first_key_size;

  if (remaining < sizeof(uint32_t)) {
    return RC::INVALID_ARGUMENT;
  }
  uint32_t last_key_size = get_numeric<uint32_t>(data_ptr);
  data_ptr += sizeof(uint32_t);
  remaining -= sizeof(uint32_t);
  if (last_key_size > remaining) {
    return RC::INVALID_ARGUMENT;
  }
  last_key_.assign(data_ptr, last_key_size);
  data_ptr += last_key_size;
  remaining -= last_key_size;

  if (remaining < 2 * sizeof(uint32_t)) {
    return RC::INVALID_ARGUMENT;
  }
  offset_ = get_numeric<uint32_t>(data_ptr);
  data_ptr += sizeof(uint32_t);
  remaining -= sizeof(uint32_t);
  size_ = get_numeric<uint32_t>(data_ptr);
  remaining -= sizeof(uint32_t);
  if (remaining != 0) {
    return RC::INVALID_ARGUMENT;
  }
  return RC::SUCCESS;
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
