/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/table/ob_sstable.h"
#include "oblsm/util/ob_coding.h"
#include "common/log/log.h"
#include "common/lang/filesystem.h"
namespace oceanbase {

void ObSSTable::init()
{
  file_reader_ = ObFileReader::create_file_reader(file_name_);
  if (file_reader_ == nullptr) {
    return;
  }

  uint32_t file_size = file_reader_->file_size();
  if (file_size < sizeof(uint32_t)) {
    return;
  }

  string meta_start_data = file_reader_->read_pos(file_size - sizeof(uint32_t), sizeof(uint32_t));
  if (meta_start_data.size() != sizeof(uint32_t)) {
    return;
  }

  uint32_t meta_start = get_numeric<uint32_t>(meta_start_data.data());
  if (meta_start + sizeof(uint32_t) > file_size) {
    return;
  }

  uint32_t pos = meta_start;
  string   meta_count_data = file_reader_->read_pos(pos, sizeof(uint32_t));
  if (meta_count_data.size() != sizeof(uint32_t)) {
    return;
  }
  uint32_t meta_count = get_numeric<uint32_t>(meta_count_data.data());
  pos += sizeof(uint32_t);

  block_metas_.clear();
  block_metas_.reserve(meta_count);
  for (uint32_t i = 0; i < meta_count; ++i) {
    string meta_size_data = file_reader_->read_pos(pos, sizeof(uint32_t));
    if (meta_size_data.size() != sizeof(uint32_t)) {
      block_metas_.clear();
      return;
    }
    uint32_t meta_size = get_numeric<uint32_t>(meta_size_data.data());
    pos += sizeof(uint32_t);
    if (pos + meta_size > file_size) {
      block_metas_.clear();
      return;
    }
    string meta_data = file_reader_->read_pos(pos, meta_size);
    if (meta_data.size() != meta_size) {
      block_metas_.clear();
      return;
    }
    pos += meta_size;

    BlockMeta meta;
    if (meta.decode(meta_data) != RC::SUCCESS) {
      block_metas_.clear();
      return;
    }
    block_metas_.emplace_back(meta);
  }
}

shared_ptr<ObBlock> ObSSTable::read_block_with_cache(uint32_t block_idx) const
{
  if (block_cache_ == nullptr) {
    return read_block(block_idx);
  }

  uint64_t            cache_key = (static_cast<uint64_t>(sst_id_) << 32) | block_idx;
  shared_ptr<ObBlock> block;
  if (block_cache_->get(cache_key, block)) {
    return block;
  }

  block = read_block(block_idx);
  if (block != nullptr) {
    block_cache_->put(cache_key, block);
  }
  return block;
}

shared_ptr<ObBlock> ObSSTable::read_block(uint32_t block_idx) const
{
  if (file_reader_ == nullptr || block_idx >= block_metas_.size()) {
    return nullptr;
  }

  const BlockMeta &meta = block_metas_[block_idx];
  string           data = file_reader_->read_pos(meta.offset_, meta.size_);
  if (data.size() != meta.size_) {
    return nullptr;
  }

  auto block = make_shared<ObBlock>(comparator_);
  if (block->decode(data) != RC::SUCCESS) {
    return nullptr;
  }
  return block;
}

void ObSSTable::remove() { filesystem::remove(file_name_); }

ObLsmIterator *ObSSTable::new_iterator() { return new TableIterator(get_shared_ptr()); }

void TableIterator::read_block_with_cache()
{
  block_ = sst_->read_block_with_cache(curr_block_idx_);
  if (block_ == nullptr) {
    block_iterator_.reset();
    return;
  }
  block_iterator_.reset(block_->new_iterator());
}

void TableIterator::seek_to_first()
{
  if (block_cnt_ == 0) {
    block_iterator_.reset();
    return;
  }
  curr_block_idx_ = 0;
  read_block_with_cache();
  if (block_iterator_ != nullptr) {
    block_iterator_->seek_to_first();
  }
}

void TableIterator::seek_to_last()
{
  if (block_cnt_ == 0) {
    block_iterator_.reset();
    return;
  }
  curr_block_idx_ = block_cnt_ - 1;
  read_block_with_cache();
  if (block_iterator_ != nullptr) {
    block_iterator_->seek_to_last();
  }
}

void TableIterator::next()
{
  block_iterator_->next();
  if (block_iterator_->valid()) {
  } else if (curr_block_idx_ < block_cnt_ - 1) {
    curr_block_idx_++;
    read_block_with_cache();
    block_iterator_->seek_to_first();
  }
}

void TableIterator::seek(const string_view &lookup_key)
{
  curr_block_idx_ = 0;
  // TODO: use binary search
  for (; curr_block_idx_ < block_cnt_; curr_block_idx_++) {
    const auto &block_meta = sst_->block_meta(curr_block_idx_);
    if (sst_->comparator()->compare(extract_user_key(block_meta.last_key_), extract_user_key_from_lookup_key(lookup_key)) >= 0) {
      break;
    }
  }
  if (curr_block_idx_ == block_cnt_) {
    block_iterator_ = nullptr;
    return;
  }
  read_block_with_cache();
  if (block_iterator_ != nullptr) {
    block_iterator_->seek(lookup_key);
  }
};

}  // namespace oceanbase
