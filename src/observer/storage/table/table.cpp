/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Meiyi & Wangyunlai on 2021/5/13.
//

#include <limits.h>
#include <string.h>

#include "common/defs.h"
#include "common/lang/string.h"
#include "common/lang/span.h"
#include "common/lang/algorithm.h"
#include "common/log/log.h"
#include "common/global_context.h"
#include "storage/db/db.h"
#include "storage/buffer/disk_buffer_pool.h"
#include "storage/common/condition_filter.h"
#include "storage/common/meta_util.h"
#include "storage/index/bplus_tree_index.h"
#include "storage/index/index.h"
#include "storage/record/record_manager.h"
#include "storage/record/lob_ref.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"
#include "storage/record/heap_record_scanner.h"
#include "storage/record/lsm_record_scanner.h"
#include "storage/table/heap_table_engine.h"
#include "storage/table/lsm_table_engine.h"

Table::~Table()
{
  if (lob_handler_ != nullptr) {
    delete lob_handler_;
    lob_handler_ = nullptr;
  }
}

RC Table::create(Db *db, int32_t table_id, const char *path, const char *name, const char *base_dir,
    span<const AttrInfoSqlNode> attributes, const vector<string> &primary_keys, StorageFormat storage_format, StorageEngine storage_engine)
{
  if (table_id < 0) {
    LOG_WARN("invalid table id. table_id=%d, table_name=%s", table_id, name);
    return RC::INVALID_ARGUMENT;
  }

  if (common::is_blank(name)) {
    LOG_WARN("Name cannot be empty");
    return RC::INVALID_ARGUMENT;
  }
  LOG_INFO("Begin to create table %s:%s", base_dir, name);

  if (attributes.size() == 0) {
    LOG_WARN("Invalid arguments. table_name=%s, attribute_count=%d", name, attributes.size());
    return RC::INVALID_ARGUMENT;
  }

  RC rc = RC::SUCCESS;

  // 使用 table_name.table记录一个表的元数据
  // 判断表文件是否已经存在
  int fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) {
    if (EEXIST == errno) {
      LOG_ERROR("Failed to create table file, it has been created. %s, EEXIST, %s", path, strerror(errno));
      return RC::SCHEMA_TABLE_EXIST;
    }
    LOG_ERROR("Create table file failed. filename=%s, errmsg=%d:%s", path, errno, strerror(errno));
    return RC::IOERR_OPEN;
  }

  close(fd);

  // 创建文件
  const vector<FieldMeta> *trx_fields = db->trx_kit().trx_fields();
  if ((rc = table_meta_.init(table_id, name, trx_fields, attributes, primary_keys, storage_format, storage_engine)) != RC::SUCCESS) {
    LOG_ERROR("Failed to init table meta. name:%s, ret:%d", name, rc);
    return rc;  // delete table file
  }

  fstream fs;
  fs.open(path, ios_base::out | ios_base::binary);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", path, strerror(errno));
    return RC::IOERR_OPEN;
  }

  // 记录元数据到文件中
  table_meta_.serialize(fs);
  fs.close();

  db_       = db;

  string             data_file = table_data_file(base_dir, name);
  BufferPoolManager &bpm       = db->buffer_pool_manager();
  rc                           = bpm.create_file(data_file.c_str());
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to create disk buffer pool of data file. file name=%s", data_file.c_str());
    return rc;
  }

  // Create LOB file for TEXT storage
  if (lob_handler_ == nullptr) {
    lob_handler_ = new LobFileHandler();
  }
  string lob_file = table_lob_file(base_dir, name);
  RC lob_rc = lob_handler_->create_file(lob_file.c_str());
  if (lob_rc != RC::SUCCESS) {
    LOG_WARN("Failed to create LOB file. file=%s, rc=%s", lob_file.c_str(), strrc(lob_rc));
    // Not fatal for non-TEXT tables, but keep handler for later use
  }

  if (table_meta_.storage_engine() == StorageEngine::HEAP) {
    engine_ = make_unique<HeapTableEngine>(&table_meta_, db_, this);
  } else if (table_meta_.storage_engine() == StorageEngine::LSM) {
    engine_ = make_unique<LsmTableEngine>(&table_meta_, db_, this);
  } else {
    rc = RC::UNSUPPORTED;
    LOG_WARN("Unsupported storage engine type: %d", table_meta_.storage_engine());
    return rc;
  }
  rc = engine_->open();
  if (rc != RC::SUCCESS) {
    LOG_WARN("Failed to open table %s due to engine open failed.", data_file.c_str());
    return rc;
  }

  LOG_INFO("Successfully create table %s:%s", base_dir, name);
  return rc;
}

RC Table::open(Db *db, const char *meta_file, const char *base_dir)
{
  // 加载元数据文件
  fstream fs;
  string  meta_file_path = string(base_dir) + common::FILE_PATH_SPLIT_STR + meta_file;
  fs.open(meta_file_path, ios_base::in | ios_base::binary);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open meta file for read. file name=%s, errmsg=%s", meta_file_path.c_str(), strerror(errno));
    return RC::IOERR_OPEN;
  }
  if (table_meta_.deserialize(fs) < 0) {
    LOG_ERROR("Failed to deserialize table meta. file name=%s", meta_file_path.c_str());
    fs.close();
    return RC::INTERNAL;
  }
  fs.close();

  db_       = db;

  // // 加载数据文件
  // RC rc = init_record_handler(base_dir);
  // if (rc != RC::SUCCESS) {
  //   LOG_ERROR("Failed to open table %s due to init record handler failed.", base_dir);
  //   // don't need to remove the data_file
  //   return rc;
  // }
  RC rc = RC::SUCCESS;

  if (table_meta_.storage_engine() == StorageEngine::HEAP) {
    engine_ = make_unique<HeapTableEngine>(&table_meta_, db_, this);
  }  else if (table_meta_.storage_engine() == StorageEngine::LSM) {
    engine_ = make_unique<LsmTableEngine>(&table_meta_, db_, this);
  } else {
    rc = RC::UNSUPPORTED;
    LOG_ERROR("Unsupported storage engine type: %d", table_meta_.storage_engine());
    return rc;
  }

  // Open LOB file for this table
  if (lob_handler_ == nullptr) {
    lob_handler_ = new LobFileHandler();
  }
  {
    string lob_file = table_lob_file(base_dir, table_meta_.name());
    RC lob_rc = lob_handler_->open_file(lob_file.c_str());
    if (lob_rc != RC::SUCCESS) {
      LOG_WARN("LOB file open failed or missing. file=%s, rc=%s", lob_file.c_str(), strrc(lob_rc));
      // tolerate missing lob file for legacy tables; it will be created on demand by create path
    }
  }

  rc = engine_->open();
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to open table %s due to engine open failed.", base_dir);
    return rc;
  }

  return rc;
}

RC Table::insert_record(Record &record)
{
  return engine_->insert_record(record);
}

RC Table::insert_chunk(const Chunk& chunk)
{
  return engine_->insert_chunk(chunk);
}

RC Table::visit_record(const RID &rid, function<bool(Record &)> visitor)
{
  return engine_->visit_record(rid, visitor);
}

RC Table::insert_record_with_trx(Record &record, Trx *trx)
{
  return engine_->insert_record_with_trx(record, trx);
}
RC Table::delete_record_with_trx(const Record &record, Trx *trx)
{
  return engine_->delete_record_with_trx(record, trx);
}

RC Table::update_record_with_trx(const Record &old_record, const Record &new_record, Trx* trx)
{
  return engine_->update_record_with_trx(old_record, new_record, trx);
}

RC Table::get_record(const RID &rid, Record &record)
{
  return engine_->get_record(rid, record);
}

const char *Table::name() const { return table_meta_.name(); }

const TableMeta &Table::table_meta() const { return table_meta_; }

RC Table::make_record(int value_num, const Value *values, Record &record)
{
  RC rc = RC::SUCCESS;
  // 检查字段类型是否一致
  if (value_num + table_meta_.sys_field_num() != table_meta_.field_num()) {
    LOG_WARN("Input values don't match the table's schema, table name:%s", table_meta_.name());
    return RC::SCHEMA_FIELD_MISSING;
  }

  const int normal_field_start_index = table_meta_.sys_field_num();
  // 复制所有字段的值
  int   record_size = table_meta_.record_size();
  char *record_data = (char *)malloc(record_size);
  memset(record_data, 0, record_size);

  for (int i = 0; i < value_num && OB_SUCC(rc); i++) {
    const FieldMeta *field = table_meta_.field(i + normal_field_start_index);
    const Value &    value = values[i];
    // 交由 set_value_to_record 统一处理 NULL 与类型转换
    rc = set_value_to_record(record_data, value, field);
  }
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to make record. table name:%s", table_meta_.name());
    free(record_data);
    return rc;
  }

  record.set_data_owner(record_data, record_size);
  return RC::SUCCESS;
}

RC Table::set_value_to_record(char *record_data, const Value &value, const FieldMeta *field)
{
  // 处理 NULL 值
  if (value.is_null()) {
    if (!field->nullable()) {
      LOG_WARN("field not nullable. field=%s", field->name());
      return RC::INVALID_ARGUMENT;
    }
    int nb_off = table_meta_.null_bitmap_offset();
    int fid    = field->field_id();
    if (nb_off >= 0 && fid >= 0) {
      unsigned char *nb = reinterpret_cast<unsigned char *>(record_data + nb_off);
      nb[fid / 8] |= (1U << (fid % 8));
    }
    return RC::SUCCESS;
  }

  // 清空 NULL 位
  int nb_off = table_meta_.null_bitmap_offset();
  int fid    = field->field_id();
  if (nb_off >= 0 && fid >= 0) {
    unsigned char *nb = reinterpret_cast<unsigned char *>(record_data + nb_off);
    nb[fid / 8] &= ~(1U << (fid % 8));
  }

  // 必要时做类型转换
  Value real_value;
  const Value *src = &value;
  if (field->type() != value.attr_type()) {
    RC rc = Value::cast_to(value, field->type(), real_value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to cast value. field=%s, value=%s", field->name(), value.to_string().c_str());
      return rc;
    }
    src = &real_value;
  }

  // Special handling for TEXT: store into LOB file, and write LobRef to record
  if (field->type() == AttrType::TEXTS) {
    if (lob_handler_ == nullptr) {
      LOG_WARN("LOB handler not initialized for table %s", table_meta_.name());
      return RC::INTERNAL;
    }

    // Enforce TEXT length limit
    int text_len = 0;
    if (src->attr_type() == AttrType::TEXTS || src->attr_type() == AttrType::CHARS) {
      text_len = src->length();
    } else {
      // fall back to to_string
      std::string tmp = src->to_string();
      text_len = static_cast<int>(tmp.size());
    }
    if (text_len > TEXT_MAX_LENGTH) {
      LOG_WARN("TEXT too long: len=%d > %d", text_len, TEXT_MAX_LENGTH);
      return RC::IOERR_TOO_LONG;
    }

    // Write data into .lob file
    int64_t offset = 0;
    RC      lrc    = RC::SUCCESS;
    if (text_len > 0) {
      lrc = lob_handler_->insert_data(offset, text_len, src->data());
      if (OB_FAIL(lrc)) {
        LOG_WARN("failed to append LOB. rc=%s", strrc(lrc));
        return lrc;
      }
    } else {
      offset = 0; // empty
    }

    // Fill LobRef into record
    LobRef ref;
    ref.length = text_len;
    ref.offset = offset;
    const size_t record_size   = table_meta_.record_size();
    const size_t field_offset  = static_cast<size_t>(field->offset());
    const size_t field_len     = static_cast<size_t>(field->len());
    const size_t avail_in_rec  = (field_offset < record_size) ? (record_size - field_offset) : 0;
    const size_t writable_size = std::min(field_len, avail_in_rec);
    if (writable_size >= sizeof(LobRef)) {
      memcpy(record_data + field_offset, &ref, sizeof(LobRef));
    }
    return RC::SUCCESS;
  }

  // 安全写入：计算该字段在记录缓冲区内的可写范围，避免越界
  {
    const size_t record_size   = table_meta_.record_size();
    const size_t field_offset  = static_cast<size_t>(field->offset());
    const size_t field_len     = static_cast<size_t>(field->len());
    const size_t avail_in_rec  = (field_offset < record_size) ? (record_size - field_offset) : 0;
    const size_t writable_size = std::min(field_len, avail_in_rec);

    const size_t data_len = src->length();
    size_t       copy_len = 0;

    if (writable_size > 0) {
      // 先将可写区域清零
      memset(record_data + field_offset, 0, writable_size);
    } else {
      return RC::SUCCESS;
    }

    if (field->type() == AttrType::CHARS) {
      // CHARS：若为空字符串则不拷贝，区域已清零可提供终止符
      copy_len = (data_len > 0 && src->data() != nullptr) ? std::min(writable_size, data_len) : 0;
    } else if (field->type() == AttrType::TEXTS) {
      // TEXT：仅按长度截断，不强制添加'\0'
      copy_len = std::min(writable_size, data_len);
    } else {
      // 其他类型：保守处理
      copy_len = std::min(writable_size, data_len);
    }

    if (copy_len > 0) {
      memcpy(record_data + field_offset, src->data(), copy_len);
    }
    return RC::SUCCESS;
  }

  size_t       copy_len = field->len();
  const size_t data_len = src->length();
  if (field->type() == AttrType::CHARS) {
    if (copy_len > data_len) {
      copy_len = data_len; // 预留结尾'\0'不需要显式拷贝，区域已清零
    }
    memset(record_data + field->offset(), 0, field->len());
  } else if (field->type() == AttrType::TEXTS) {
    // TEXT: 截断到最多4096字节，不强制添加额外'\0'
    if (copy_len > data_len) {
      copy_len = data_len;
    }
    memset(record_data + field->offset(), 0, field->len());
  }
  if (copy_len > 0 && src->data() != nullptr) {
    memcpy(record_data + field->offset(), src->data(), copy_len);
  }
  return RC::SUCCESS;
}

RC Table::get_record_scanner(RecordScanner *&scanner, Trx *trx, ReadWriteMode mode)
{
  return engine_->get_record_scanner(scanner, trx, mode);
}

RC Table::get_chunk_scanner(ChunkFileScanner &scanner, Trx *trx, ReadWriteMode mode)
{
  return engine_->get_chunk_scanner(scanner, trx, mode);
}

RC Table::create_index(Trx *trx, span<const FieldMeta> field_metas, const char *index_name, bool unique)
{
  return engine_->create_index(trx, field_metas, index_name, unique);
}

RC Table::delete_record(const Record &record)
{
  return engine_->delete_record(record);
}

Index *Table::find_index(const char *index_name) const
{
  return engine_->find_index(index_name);
}
Index *Table::find_index_by_field(const char *field_name) const
{
  return engine_->find_index_by_field(field_name);
}

RC Table::sync()
{
  return engine_->sync();
}
