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
  block_metas_.clear();

  file_reader_ = ObFileReader::create_file_reader(file_name_);
  if (file_reader_ == nullptr) {
    LOG_WARN("failed to create file reader for %s", file_name_.c_str());
    return;
  }

  const uint32_t file_size = file_reader_->file_size();
  if (file_size < sizeof(uint32_t)) {
    LOG_WARN("sstable file %s is too small, size=%u", file_name_.c_str(), file_size);
    return;
  }

  struct FooterCandidate
  {
    uint32_t offset = 0;
    uint32_t size   = 0;
  };

  vector<FooterCandidate> candidates;
  candidates.reserve(3);

  auto add_candidate = [&](uint32_t offset, uint32_t footer_size) {
    if (footer_size == 0 || footer_size > file_size || offset > file_size) {
      return;
    }
    for (const auto &cand : candidates) {
      if (cand.offset == offset && cand.size == footer_size) {
        return;
      }
    }
    candidates.push_back({offset, footer_size});
  };

  string footer8;
  if (file_size >= 2 * sizeof(uint32_t)) {
    footer8 = file_reader_->read_pos(file_size - 2 * sizeof(uint32_t), 2 * sizeof(uint32_t));
    if (footer8.size() == 2 * sizeof(uint32_t)) {
      const char *ptr = footer8.data();
      add_candidate(get_numeric<uint32_t>(ptr), 2 * sizeof(uint32_t));
      add_candidate(get_numeric<uint32_t>(ptr + sizeof(uint32_t)), 2 * sizeof(uint32_t));
    }
  }

  string footer4;
  if (!footer8.empty()) {
    footer4.assign(footer8.data() + sizeof(uint32_t), sizeof(uint32_t));
  } else {
    footer4 = file_reader_->read_pos(file_size - sizeof(uint32_t), sizeof(uint32_t));
  }

  if (footer4.size() == sizeof(uint32_t)) {
    add_candidate(get_numeric<uint32_t>(footer4.data()), sizeof(uint32_t));
  }

  auto try_parse_metadata = [&](uint32_t offset, uint32_t footer_size) -> bool {
    if (offset > file_size || footer_size > file_size) {
      return false;
    }
    if (offset + footer_size > file_size) {
      return false;
    }
    uint32_t meta_length = file_size - footer_size - offset;
    if (meta_length < sizeof(uint32_t)) {
      return false;
    }

    string meta_buf = file_reader_->read_pos(offset, meta_length);
    if (meta_buf.size() != meta_length) {
      return false;
    }

    const char *cursor    = meta_buf.data();
    size_t      remaining = meta_buf.size();
    uint32_t    block_cnt = get_numeric<uint32_t>(cursor);
    cursor += sizeof(uint32_t);
    remaining -= sizeof(uint32_t);

    vector<BlockMeta> metas;
    metas.reserve(block_cnt);
    for (uint32_t idx = 0; idx < block_cnt; idx++) {
      if (remaining < sizeof(uint32_t)) {
        return false;
      }
      uint32_t meta_size = get_numeric<uint32_t>(cursor);
      cursor += sizeof(uint32_t);
      remaining -= sizeof(uint32_t);
      if (meta_size > remaining) {
        return false;
      }
      BlockMeta meta;
      RC       rc = meta.decode(string(cursor, meta_size));
      if (OB_FAIL(rc)) {
        return false;
      }
      metas.push_back(meta);
      cursor += meta_size;
      remaining -= meta_size;
    }

    block_metas_.swap(metas);
    if (remaining > 0) {
      LOG_WARN("skip %zu trailing bytes in metadata for %s", remaining, file_name_.c_str());
    }
    return true;
  };

  bool parsed = false;
  for (const auto &cand : candidates) {
    if (try_parse_metadata(cand.offset, cand.size)) {
      parsed = true;
      break;
    }
  }

  if (!parsed) {
    LOG_WARN("failed to parse metadata for %s", file_name_.c_str());
  }
}

shared_ptr<ObBlock> ObSSTable::read_block_with_cache(uint32_t block_idx) const
{
  if (block_cache_ == nullptr) {
    return read_block(block_idx);
  }

  const uint64_t cache_key = (static_cast<uint64_t>(sst_id_) << 32) | block_idx;
  shared_ptr<ObBlock> cached_block;
  if (block_cache_->get(cache_key, cached_block) && cached_block != nullptr) {
    return cached_block;
  }

  shared_ptr<ObBlock> block = read_block(block_idx);
  if (block != nullptr) {
    block_cache_->put(cache_key, block);
  }
  return block;
}

shared_ptr<ObBlock> ObSSTable::read_block(uint32_t block_idx) const
{
  if (file_reader_ == nullptr) {
    LOG_WARN("sstable %s file reader not initialized", file_name_.c_str());
    return nullptr;
  }

  if (block_idx >= block_metas_.size()) {
    LOG_WARN("block idx %u out of range, total=%zu in %s", block_idx, block_metas_.size(), file_name_.c_str());
    return nullptr;
  }

  const BlockMeta &meta = block_metas_[block_idx];
  string           block_buf = file_reader_->read_pos(meta.offset_, meta.size_);
  if (block_buf.size() != meta.size_) {
    LOG_WARN("failed to read block idx=%u from %s, expect=%u actual=%zu", block_idx, file_name_.c_str(), meta.size_, block_buf.size());
    return nullptr;
  }

  shared_ptr<ObBlock> block = make_shared<ObBlock>(comparator_);
  RC rc = block->decode(block_buf);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to decode block idx=%u from %s", block_idx, file_name_.c_str());
    return nullptr;
  }
  return block;
}

void ObSSTable::remove() { filesystem::remove(file_name_); }

ObLsmIterator *ObSSTable::new_iterator() { return new TableIterator(get_shared_ptr()); }

void TableIterator::read_block_with_cache()
{
  block_ = sst_->read_block_with_cache(curr_block_idx_);
  block_iterator_.reset(block_->new_iterator());
}

void TableIterator::seek_to_first()
{
  curr_block_idx_ = 0;
  read_block_with_cache();
  block_iterator_->seek_to_first();
}

void TableIterator::seek_to_last()
{
  curr_block_idx_ = block_cnt_ - 1;
  read_block_with_cache();
  block_iterator_->seek_to_last();
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
  block_iterator_->seek(lookup_key);
};

}  // namespace oceanbase
