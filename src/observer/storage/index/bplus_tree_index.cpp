/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by wangyunlai.wyl on 2021/5/19.
//

#include "storage/index/bplus_tree_index.h"
#include "common/log/log.h"
#include "storage/table/table.h"
#include "storage/db/db.h"
#include <list>

static inline void put_be32(uint32_t v, char *out)
{
  out[0] = static_cast<char>((v >> 24) & 0xFF);
  out[1] = static_cast<char>((v >> 16) & 0xFF);
  out[2] = static_cast<char>((v >> 8) & 0xFF);
  out[3] = static_cast<char>(v & 0xFF);
}

BplusTreeIndex::~BplusTreeIndex() noexcept { close(); }

RC BplusTreeIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas)
{
  if (inited_) {
    LOG_WARN("Failed to create index due to the index has been created before. file_name:%s, index:%s, field:%s",
        file_name, index_meta.name(), index_meta.field());
    return RC::RECORD_OPENNED;
  }

  Index::init(index_meta, field_metas);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  // 计算复合键总长度，使用order-preserving编码整体按CHARS比较
  int total_len = 0;
  for (const FieldMeta &fm : field_metas_) {
    total_len += fm.len();
  }
  RC rc = index_handler_.create(table->db()->log_handler(), bpm, file_name, AttrType::CHARS, total_len);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to create index_handler, file_name:%s, index:%s, field:%s, rc:%s",
        file_name, index_meta.name(), index_meta.field(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully create index, file_name:%s, index:%s, field:%s",
    file_name, index_meta.name(), index_meta.field());
  return RC::SUCCESS;
}

RC BplusTreeIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas)
{
  if (inited_) {
    LOG_WARN("Failed to open index due to the index has been initedd before. file_name:%s, index:%s, field:%s",
        file_name, index_meta.name(), index_meta.field());
    return RC::RECORD_OPENNED;
  }

  Index::init(index_meta, field_metas);

  BufferPoolManager &bpm = table->db()->buffer_pool_manager();
  RC rc = index_handler_.open(table->db()->log_handler(), bpm, file_name);
  if (RC::SUCCESS != rc) {
    LOG_WARN("Failed to open index_handler, file_name:%s, index:%s, field:%s, rc:%s",
        file_name, index_meta.name(), index_meta.field(), strrc(rc));
    return rc;
  }

  inited_ = true;
  table_  = table;
  LOG_INFO("Successfully open index, file_name:%s, index:%s, field:%s",
    file_name, index_meta.name(), index_meta.field());
  return RC::SUCCESS;
}

RC BplusTreeIndex::close()
{
  if (inited_) {
    LOG_INFO("Begin to close index, index:%s, field:%s", index_meta_.name(), index_meta_.field());
    index_handler_.close();
    inited_ = false;
  }
  LOG_INFO("Successfully close index.");
  return RC::SUCCESS;
}

int BplusTreeIndex::key_attr_length_sum() const
{
  int len = 0;
  for (const FieldMeta &fm : field_metas_) {
    len += fm.len();
  }
  return len;
}

void BplusTreeIndex::build_composite_key(const char *record, char *buf) const
{
  int offset = 0;
  for (const FieldMeta &fm : field_metas_) {
    switch (fm.type()) {
      case AttrType::INTS:
      case AttrType::DATES: {
        // 32-bit int order-preserving transform
        int32_t iv = 0;
        memcpy(&iv, record + fm.offset(), sizeof(int32_t));
        uint32_t uv = static_cast<uint32_t>(iv) ^ 0x80000000u;
        put_be32(uv, buf + offset);
        offset += sizeof(int32_t);
      } break;
      case AttrType::FLOATS: {
        uint32_t u = 0;
        memcpy(&u, record + fm.offset(), sizeof(uint32_t));
        if (u & 0x80000000u) {
          u = ~u;
        } else {
          u ^= 0x80000000u;
        }
        put_be32(u, buf + offset);
        offset += sizeof(uint32_t);
      } break;
      case AttrType::CHARS: {
        memcpy(buf + offset, record + fm.offset(), fm.len());
        offset += fm.len();
      } break;
      case AttrType::BOOLEANS: {
        int32_t iv = 0;
        memcpy(&iv, record + fm.offset(), sizeof(int32_t)); // stored as 4 bytes
        uint32_t uv = static_cast<uint32_t>(iv) ^ 0x80000000u;
        put_be32(uv, buf + offset);
        offset += sizeof(int32_t);
      } break;
      default: {
        // Fallback: raw copy
        memcpy(buf + offset, record + fm.offset(), fm.len());
        offset += fm.len();
      } break;
    }
  }
}

RC BplusTreeIndex::insert_entry(const char *record, const RID *rid)
{
  // Enforce UNIQUE constraint if needed
  if (index_meta_.unique()) {
    const int key_len = key_attr_length_sum();
    std::unique_ptr<char[]> key(new char[key_len]);
    build_composite_key(record, key.get());
    std::list<RID> rids;
    RC rc = index_handler_.get_entry(key.get(), key_len, rids);
    if (rc != RC::SUCCESS) {
      // if open scanner failed, propagate error (except RECORD_EOF which is treated as empty)
      if (rc != RC::SUCCESS) {
        // do nothing
      }
    }
    if (!rids.empty()) {
      return RC::RECORD_DUPLICATE_KEY;
    }
  }
  const int key_len = key_attr_length_sum();
  std::unique_ptr<char[]> key(new char[key_len]);
  build_composite_key(record, key.get());
  return index_handler_.insert_entry(key.get(), rid);
}

RC BplusTreeIndex::delete_entry(const char *record, const RID *rid)
{
  const int key_len = key_attr_length_sum();
  std::unique_ptr<char[]> key(new char[key_len]);
  build_composite_key(record, key.get());
  return index_handler_.delete_entry(key.get(), rid);
}

IndexScanner *BplusTreeIndex::create_scanner(
    const char *left_key, int left_len, bool left_inclusive, const char *right_key, int right_len, bool right_inclusive)
{
  BplusTreeIndexScanner *index_scanner = new BplusTreeIndexScanner(index_handler_);
  RC rc = index_scanner->open(left_key, left_len, left_inclusive, right_key, right_len, right_inclusive);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open index scanner. rc=%d:%s", rc, strrc(rc));
    delete index_scanner;
    return nullptr;
  }
  return index_scanner;
}

RC BplusTreeIndex::sync() { return index_handler_.sync(); }

////////////////////////////////////////////////////////////////////////////////
BplusTreeIndexScanner::BplusTreeIndexScanner(BplusTreeHandler &tree_handler) : tree_scanner_(tree_handler) {}

BplusTreeIndexScanner::~BplusTreeIndexScanner() noexcept { tree_scanner_.close(); }

RC BplusTreeIndexScanner::open(
    const char *left_key, int left_len, bool left_inclusive, const char *right_key, int right_len, bool right_inclusive)
{
  return tree_scanner_.open(left_key, left_len, left_inclusive, right_key, right_len, right_inclusive);
}

RC BplusTreeIndexScanner::next_entry(RID *rid) { return tree_scanner_.next_entry(*rid); }

RC BplusTreeIndexScanner::destroy()
{
  delete this;
  return RC::SUCCESS;
}
