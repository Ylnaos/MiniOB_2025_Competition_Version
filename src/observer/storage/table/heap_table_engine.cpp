/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/table/heap_table_engine.h"
#include "storage/record/heap_record_scanner.h"
#include "common/log/log.h"
#include "storage/index/bplus_tree_index.h"
#include "storage/index/ivfflat_index.h"
#include "storage/common/meta_util.h"
#include "storage/db/db.h"


HeapTableEngine::~HeapTableEngine()
{
  if (record_handler_ != nullptr) {
    delete record_handler_;
    record_handler_ = nullptr;
  }

  if (data_buffer_pool_ != nullptr) {
    data_buffer_pool_->close_file();
    data_buffer_pool_ = nullptr;
  }

  for (vector<Index *>::iterator it = indexes_.begin(); it != indexes_.end(); ++it) {
    Index *index = *it;
    delete index;
  }
  indexes_.clear();

  LOG_INFO("Table has been closed: %s", table_meta_->name());
}
RC HeapTableEngine::insert_record(Record &record)
{
  RC rc = RC::SUCCESS;

  // 1) 先基�?UNIQUE 索引做重复检查，避免写入后回�?
if (!indexes_.empty()) {
    auto build_user_key = [&](const Index *index, vector<char> &out_key) {
      int total_len = 0;
      for (const FieldMeta &fm : index->key_fields()) {
        total_len += fm.len();
      }
      out_key.resize(total_len);

      auto put_be32 = [](uint32_t v, char *out) {
        out[0] = static_cast<char>((v >> 24) & 0xFF);
        out[1] = static_cast<char>((v >> 16) & 0xFF);
        out[2] = static_cast<char>((v >> 8) & 0xFF);
        out[3] = static_cast<char>(v & 0xFF);
      };

      int         offset = 0;
      const char *rec    = record.data();
      for (const FieldMeta &fm : index->key_fields()) {
        switch (fm.type()) {
          case AttrType::INTS:
          case AttrType::DATES: {
            int32_t iv = 0;
            memcpy(&iv, rec + fm.offset(), sizeof(int32_t));
            uint32_t uv = static_cast<uint32_t>(iv) ^ 0x80000000u;
            put_be32(uv, out_key.data() + offset);
            offset += sizeof(int32_t);
          } break;
          case AttrType::FLOATS: {
            uint32_t u = 0;
            memcpy(&u, rec + fm.offset(), sizeof(uint32_t));
            if (u & 0x80000000u) { u = ~u; } else { u ^= 0x80000000u; }
            put_be32(u, out_key.data() + offset);
            offset += sizeof(uint32_t);
          } break;
          case AttrType::BOOLEANS: {
            int32_t iv = 0;
            memcpy(&iv, rec + fm.offset(), sizeof(int32_t));
            uint32_t uv = static_cast<uint32_t>(iv) ^ 0x80000000u;
            put_be32(uv, out_key.data() + offset);
            offset += sizeof(int32_t);
          } break;
          case AttrType::CHARS:
          default: {
            memcpy(out_key.data() + offset, rec + fm.offset(), fm.len());
            offset += fm.len();
          } break;
        }
      }
    };

    for (Index *index : indexes_) {
      if (!index->index_meta().unique()) {
        continue;
      }
      vector<char> user_key;
      build_user_key(index, user_key);
      IndexScanner *scanner = index->create_scanner(user_key.data(), static_cast<int>(user_key.size()), true,
                                                    user_key.data(), static_cast<int>(user_key.size()), true);
      if (scanner == nullptr) {
        // 扫描器打开失败（可能是空树/瞬时锁），保守放行，由索引层再次兜底检查�?
LOG_TRACE("skip unique precheck due to scanner open fail. table=%s, index=%s",
                  table_meta_->name(), index->index_meta().name());
        continue;
      }
      RID exist;
      rc = scanner->next_entry(&exist);
      scanner->destroy();
      // do not short-circuit here; let index enforce uniqueness precisely
      rc = RC::SUCCESS;
    }
  }

  // 2) 写入数据文件
  rc = record_handler_->insert_record(record.data(), table_meta_->record_size(), &record.rid());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Insert record failed. table name=%s, rc=%s", table_meta_->name(), strrc(rc));
    return rc;
  }

  // 3) 维护索引
  rc = insert_entry_of_indexes(record.data(), record.rid());
  if (rc != RC::SUCCESS) {  // 可能出现了键值重�?
RC rc2 = delete_entry_of_indexes(record.data(), record.rid(), false /*error_on_not_exists*/);
    if (rc2 != RC::SUCCESS) {
      LOG_ERROR("Failed to rollback index data when insert index entries failed. table name=%s, rc=%d:%s",
                table_meta_->name(), rc2, strrc(rc2));
    }
    rc2 = record_handler_->delete_record(&record.rid());
    if (rc2 != RC::SUCCESS) {
      LOG_PANIC("Failed to rollback record data when insert index entries failed. table name=%s, rc=%d:%s",
                table_meta_->name(), rc2, strrc(rc2));
    }
  }
  return rc;
}

RC HeapTableEngine::insert_chunk(const Chunk& chunk)
{
  RC rc = RC::SUCCESS;
  rc    = record_handler_->insert_chunk(chunk, table_meta_->record_size());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Insert chunk failed. table name=%s, rc=%s", table_meta_->name(), strrc(rc));
    return rc;
  }

  // TODO: insert chunk support update index
  return rc;
}

RC HeapTableEngine::visit_record(const RID &rid, function<bool(Record &)> visitor)
{
  return record_handler_->visit_record(rid, visitor);
}

RC HeapTableEngine::get_record(const RID &rid, Record &record)
{
  RC rc = record_handler_->get_record(rid, record);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to visit record. rid=%s, table=%s, rc=%s", rid.to_string().c_str(), table_meta_->name(), strrc(rc));
    return rc;
  }

  return rc;
}

RC HeapTableEngine::delete_record(const Record &record)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->delete_entry(record.data(), &record.rid());
    ASSERT(RC::SUCCESS == rc,
           "failed to delete entry from index. table name=%s, index name=%s, rid=%s, rc=%s",
           table_meta_->name(), index->index_meta().name(), record.rid().to_string().c_str(), strrc(rc));
  }
  rc = record_handler_->delete_record(&record.rid());
  return rc;
}

RC HeapTableEngine::update_record_with_trx(const Record &old_record, const Record &new_record, Trx *trx)
{
  RC rc = RC::SUCCESS;

  // 首先删除旧的索引�?
for (Index *index : indexes_) {
    rc = index->delete_entry(old_record.data(), &old_record.rid());
    if (rc != RC::SUCCESS && rc != RC::RECORD_NOT_EXIST) {
      LOG_ERROR("failed to delete old entry from index. table name=%s, index name=%s, rid=%s, rc=%s",
                table_meta_->name(), index->index_meta().name(), old_record.rid().to_string().c_str(), strrc(rc));
      // 回滚已经删除的索�?
for (Index *rollback_index : indexes_) {
        if (rollback_index == index) {
          break;
        }
        RC rc2 = rollback_index->insert_entry(old_record.data(), &old_record.rid());
        if (rc2 != RC::SUCCESS) {
          LOG_ERROR("failed to rollback index during update. table name=%s, index name=%s, rc=%s",
                    table_meta_->name(), rollback_index->index_meta().name(), strrc(rc2));
        }
      }
      return rc;
    }
  }

  // 更新记录
  rc = record_handler_->update_record(old_record.rid(), new_record.data());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to update record. table name=%s, rid=%s, rc=%s",
              table_meta_->name(), old_record.rid().to_string().c_str(), strrc(rc));
    // 回滚索引删除
    for (Index *index : indexes_) {
      RC rc2 = index->insert_entry(old_record.data(), &old_record.rid());
      if (rc2 != RC::SUCCESS) {
        LOG_ERROR("failed to rollback index during update. table name=%s, index name=%s, rc=%s",
                  table_meta_->name(), index->index_meta().name(), strrc(rc2));
      }
    }
    return rc;
  }

  // 插入新的索引�?
for (Index *index : indexes_) {
    rc = index->insert_entry(new_record.data(), &new_record.rid());
    if (rc != RC::SUCCESS) {
      LOG_ERROR("failed to insert new entry to index. table name=%s, index name=%s, rid=%s, rc=%s",
                table_meta_->name(), index->index_meta().name(), new_record.rid().to_string().c_str(), strrc(rc));

      // 回滚：恢复原记录
      RC rc2 = record_handler_->update_record(old_record.rid(), old_record.data());
      if (rc2 != RC::SUCCESS) {
        LOG_PANIC("failed to rollback record during update. table name=%s, rid=%s, rc=%s",
                  table_meta_->name(), old_record.rid().to_string().c_str(), strrc(rc2));
      }

      // 回滚：删除已插入的新索引项，恢复旧索引项
      for (Index *rollback_index : indexes_) {
        if (rollback_index == index) {
          // 对于当前失败的索引，只需恢复旧索引项
          rc2 = rollback_index->insert_entry(old_record.data(), &old_record.rid());
        } else {
          // 对于已成功的索引，需要先删除新索引项，再恢复旧索引项
          rc2 = rollback_index->delete_entry(new_record.data(), &new_record.rid());
          if (rc2 == RC::SUCCESS) {
            rc2 = rollback_index->insert_entry(old_record.data(), &old_record.rid());
          }
        }
        if (rc2 != RC::SUCCESS) {
          LOG_ERROR("failed to rollback index during update. table name=%s, index name=%s, rc=%s",
                    table_meta_->name(), rollback_index->index_meta().name(), strrc(rc2));
        }
      }
      return rc;
    }
  }

  return RC::SUCCESS;
}

RC HeapTableEngine::get_record_scanner(RecordScanner *&scanner, Trx *trx, ReadWriteMode mode)
{
  scanner = new HeapRecordScanner(table_, *data_buffer_pool_, trx, db_->log_handler(), mode, nullptr);
  RC rc = scanner->open_scan();
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to open scanner. rc=%s", strrc(rc));
  }
  return rc;
}

RC HeapTableEngine::get_chunk_scanner(ChunkFileScanner &scanner, Trx *trx, ReadWriteMode mode)
{
  RC rc = scanner.open_scan_chunk(table_, *data_buffer_pool_, db_->log_handler(), mode);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to open scanner. rc=%s", strrc(rc));
  }
  return rc;
}

RC HeapTableEngine::create_index(Trx *trx, span<const FieldMeta> field_metas, const char *index_name, bool unique)
{
  if (common::is_blank(index_name) || field_metas.empty()) {
    LOG_INFO("Invalid input arguments, table name is %s, index_name is blank or fields empty", table_meta_->name());
    return RC::INVALID_ARGUMENT;
  }

  IndexMeta new_index_meta;

  RC rc = new_index_meta.init(index_name, field_metas, unique);
  if (rc != RC::SUCCESS) {
    LOG_INFO("Failed to init IndexMeta in table:%s, index_name:%s", 
             table_meta_->name(), index_name);
    return rc;
  }

  // 判断是否为向量索引
  bool is_vector_index = (field_metas.size() == 1 && field_metas[0].type() == AttrType::VECTORS);

  Index *index = nullptr;
  string index_file = table_index_file(db_->path().c_str(), table_meta_->name(), index_name);

  if (is_vector_index) {
    // 创建向量索引
    index = new IvfflatIndex();
    LOG_INFO("Creating IVF-Flat vector index: %s", index_name);
  } else {
    // 创建B+树索引
    index = new BplusTreeIndex();
    LOG_INFO("Creating B+Tree index: %s", index_name);
  }

  rc = index->create(table_, index_file.c_str(), new_index_meta, field_metas);
  if (rc != RC::SUCCESS) {
    delete index;
    LOG_ERROR("Failed to create index. file name=%s, rc=%d:%s", index_file.c_str(), rc, strrc(rc));
    return rc;
  }

  // 向量索引在create()时已经扫描并建立了索引，无需额外插入数据
  // B+树索引需要遍历数据逐条插入
  if (!is_vector_index) {
    RecordScanner *scanner = nullptr;
    rc = get_record_scanner(scanner, trx, ReadWriteMode::READ_ONLY);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create scanner while creating index. table=%s, index=%s, rc=%s",
               table_meta_->name(), index_name, strrc(rc));
      return rc;
    }

    Record record;
    while (OB_SUCC(rc = scanner->next(record))) {
      rc = index->insert_entry(record.data(), &record.rid());
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to insert record into index while creating index. table=%s, index=%s, rc=%s",
                 table_meta_->name(), index_name, strrc(rc));
        // cleanup index resources before return
        scanner->close_scan();
        delete scanner;
        delete index;
        // best-effort: remove the index file to avoid orphan
        string index_file_rm = table_index_file(db_->path().c_str(), table_meta_->name(), index_name);
        ::remove(index_file_rm.c_str());
        return rc;
      }
    }
    if (RC::RECORD_EOF == rc) {
      rc = RC::SUCCESS;
    } else {
      LOG_WARN("failed to insert record into index while creating index. table=%s, index=%s, rc=%s",
               table_meta_->name(), index_name, strrc(rc));
      return rc;
    }
    scanner->close_scan();
    delete scanner;
    LOG_INFO("inserted all records into new index. table=%s, index=%s", table_meta_->name(), index_name);
  }

  indexes_.push_back(index);

  /// 接下来将这个索引放到表的元数据中
  TableMeta new_table_meta(*table_meta_);
  rc = new_table_meta.add_index(new_index_meta);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to add index (%s) on table (%s). error=%d:%s", index_name, table_meta_->name(), rc, strrc(rc));
    return rc;
  }

  /// 内存中有一份元数据，磁盘文件也有一份元数据。修改磁盘文件时，先创建一个临时文件，写入完成后再rename为正式文�?  /// 这样可以防止文件内容不完�?  // 创建元数据临时文�?
string  tmp_file = table_meta_file(db_->path().c_str(), table_meta_->name()) + ".tmp";
  fstream fs;
  fs.open(tmp_file, ios_base::out | ios_base::binary | ios_base::trunc);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", tmp_file.c_str(), strerror(errno));
    return RC::IOERR_OPEN;  // 创建索引中途出错，要做还原操作
  }
  if (new_table_meta.serialize(fs) < 0) {
    LOG_ERROR("Failed to dump new table meta to file: %s. sys err=%d:%s", tmp_file.c_str(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }
  fs.close();

  // 覆盖原始元数据文�?
string meta_file = table_meta_file(db_->path().c_str(), table_meta_->name());

  int ret = rename(tmp_file.c_str(), meta_file.c_str());
  if (ret != 0) {
    LOG_ERROR("Failed to rename tmp meta file (%s) to normal meta file (%s) while creating index (%s) on table (%s). "
              "system error=%d:%s",
              tmp_file.c_str(), meta_file.c_str(), index_name, table_meta_->name(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }

  table_meta_->swap(new_table_meta);

  LOG_INFO("Successfully added a new index (%s) on the table (%s)", index_name, table_meta_->name());
  return rc;
}

RC HeapTableEngine::insert_entry_of_indexes(const char *record, const RID &rid)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->insert_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      break;
    }
  }
  return rc;
}

RC HeapTableEngine::delete_entry_of_indexes(const char *record, const RID &rid, bool error_on_not_exists)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->delete_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      if (rc != RC::RECORD_INVALID_KEY || !error_on_not_exists) {
        break;
      }
    }
  }
  return rc;
}

RC HeapTableEngine::sync()
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->sync();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to flush index's pages. table=%s, index=%s, rc=%d:%s",
          table_meta_->name(),
          index->index_meta().name(),
          rc,
          strrc(rc));
      return rc;
    }
  }

  rc = data_buffer_pool_->flush_all_pages();
  LOG_INFO("Sync table over. table=%s", table_meta_->name());
  return rc;
}

Index *HeapTableEngine::find_index(const char *index_name) const
{
  for (Index *index : indexes_) {
    if (0 == strcmp(index->index_meta().name(), index_name)) {
      return index;
    }
  }
  return nullptr;
}
Index *HeapTableEngine::find_index_by_field(const char *field_name) const
{
  const IndexMeta *index_meta = table_meta_->find_index_by_field(field_name);
  if (index_meta != nullptr) {
    return this->find_index(index_meta->name());
  }
  return nullptr;
}

RC HeapTableEngine::init()
{
  string data_file = table_data_file(db_->path().c_str(), table_meta_->name());

  BufferPoolManager &bpm = db_->buffer_pool_manager();
  RC                 rc  = bpm.open_file(db_->log_handler(), data_file.c_str(), data_buffer_pool_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to open disk buffer pool for file:%s. rc=%d:%s", data_file.c_str(), rc, strrc(rc));
    return rc;
  }

  record_handler_ = new RecordFileHandler(table_meta_->storage_format());

  rc = record_handler_->init(*data_buffer_pool_, db_->log_handler(), table_meta_, table_->lob_handler_);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to init record handler. rc=%s", strrc(rc));
    delete record_handler_;
    record_handler_ = nullptr;
    return rc;
  }

  return rc;
}

RC HeapTableEngine::open()
{
  RC rc = RC::SUCCESS;
  init();
  const int index_num = table_meta_->index_num();
  for (int i = 0; i < index_num; i++) {
    const IndexMeta *index_meta = table_meta_->index(i);
    vector<FieldMeta> field_metas;
    for (const string &fname : index_meta->fields()) {
      const FieldMeta *fm = table_meta_->field(fname.c_str());
      if (fm == nullptr) {
        LOG_ERROR("Found invalid index meta info which has a non-exists field. table=%s, index=%s, field=%s",
                  table_meta_->name(), index_meta->name(), fname.c_str());
        return RC::INTERNAL;
      }
      field_metas.push_back(*fm);
    }

    // 判断是否为向量索引
    bool is_vector_index = (field_metas.size() == 1 && field_metas[0].type() == AttrType::VECTORS);

    Index *index = nullptr;
    string index_file = table_index_file(db_->path().c_str(), table_meta_->name(), index_meta->name());

    if (is_vector_index) {
      index = new IvfflatIndex();
      LOG_INFO("Opening IVF-Flat vector index: %s", index_meta->name());
    } else {
      index = new BplusTreeIndex();
      LOG_INFO("Opening B+Tree index: %s", index_meta->name());
    }

    rc = index->open(table_, index_file.c_str(), *index_meta, span<const FieldMeta>(field_metas.data(), field_metas.size()));
    if (rc != RC::SUCCESS) {
      delete index;
      LOG_ERROR("Failed to open index. table=%s, index=%s, file=%s, rc=%s",
                table_meta_->name(), index_meta->name(), index_file.c_str(), strrc(rc));
      // skip cleanup
      //  do all cleanup action in destructive Table function.
      return rc;
    }
    indexes_.push_back(index);
  }
  return rc;
}
