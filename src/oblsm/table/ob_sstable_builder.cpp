/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/table/ob_sstable_builder.h"
#include "oblsm/util/ob_coding.h"

namespace oceanbase {

// TODO: refactor build with mem_table/iterator logic.
RC ObSSTableBuilder::build(shared_ptr<ObMemTable> mem_table, const std::string &file_name, uint32_t sst_id)
{
  RC rc = RC::SUCCESS;

  if (mem_table == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  reset();
  sst_id_ = sst_id;

  file_writer_ = ObFileWriter::create_file_writer(file_name, false);
  if (file_writer_ == nullptr) {
    reset();
    return RC::IOERR_OPEN;
  }

  unique_ptr<ObLsmIterator> iter(mem_table->new_iterator());
  if (iter == nullptr) {
    reset();
    return RC::NOMEM;
  }

  iter->seek_to_first();

  bool block_empty = true;
  while (iter->valid()) {
    string_view key = iter->key();
    string_view value = iter->value();

    if (block_empty) {
      curr_blk_first_key_.assign(key.data(), key.size());
    }

    rc = block_builder_.add(key, value);
    if (rc == RC::SUCCESS) {
      block_empty = false;
      iter->next();
      continue;
    } else if (rc == RC::FULL) {
      finish_build_block();
      curr_blk_first_key_.clear();
      block_empty = true;
      continue;
    } else {
      reset();
      return rc;
    }
  }

  if (!block_empty) {
    finish_build_block();
    curr_blk_first_key_.clear();
    block_empty = true;
  }

  const uint32_t meta_offset = curr_offset_;

  string metadata;
  put_numeric<uint32_t>(&metadata, static_cast<uint32_t>(block_metas_.size()));
  for (const BlockMeta &meta : block_metas_) {
    string encoded = meta.encode();
    put_numeric<uint32_t>(&metadata, static_cast<uint32_t>(encoded.size()));
    metadata.append(encoded);
  }

  rc = file_writer_->write(metadata);
  if (OB_FAIL(rc)) {
    reset();
    return rc;
  }

  string footer;
  put_numeric<uint32_t>(&footer, meta_offset);
  rc = file_writer_->write(footer);
  if (OB_FAIL(rc)) {
    reset();
    return rc;
  }

  rc = file_writer_->flush();
  if (OB_FAIL(rc)) {
    reset();
    return rc;
  }

  file_size_ = static_cast<size_t>(meta_offset) + metadata.size() + footer.size();

  return RC::SUCCESS;
}

void ObSSTableBuilder::finish_build_block()
{
  string      last_key       = block_builder_.last_key();
  string_view block_contents = block_builder_.finish();
  file_writer_->write(block_contents);
  block_metas_.push_back(BlockMeta(curr_blk_first_key_, last_key, curr_offset_, block_contents.size()));
  // TODO: block aligned to BLOCK_SIZE
  curr_offset_ += block_contents.size();
  block_builder_.reset();
}

shared_ptr<ObSSTable> ObSSTableBuilder::get_built_table()
{
  // TODO: sstable should have more metadata
  shared_ptr<ObSSTable> sstable = make_shared<ObSSTable>(sst_id_, file_writer_->file_name(), comparator_, block_cache_);
  sstable->init();
  return sstable;
}

void ObSSTableBuilder::reset()
{
  block_builder_.reset();
  curr_blk_first_key_.clear();
  if (file_writer_ != nullptr) {
    file_writer_.reset(nullptr);
  }
  block_metas_.clear();
  curr_offset_ = 0;
  sst_id_      = 0;
  file_size_   = 0;
}
}  // namespace oceanbase
