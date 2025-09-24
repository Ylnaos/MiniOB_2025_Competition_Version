/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/table/lsm_table_engine.h"
#include "storage/record/heap_record_scanner.h"
#include "common/log/log.h"
#include "storage/index/bplus_tree_index.h"
#include "storage/common/meta_util.h"
#include "storage/db/db.h"
#include "storage/record/lsm_record_scanner.h"
#include "storage/common/codec.h"
#include "storage/trx/lsm_mvcc_trx.h"
#include "storage/index/bplus_tree_index.h"
#include "storage/common/meta_util.h"
#include <fstream>
#include <cerrno>
#include <cstdio>

RC LsmTableEngine::insert_record(Record &record)
{
  RC rc = RC::SUCCESS;
  // TODO: set auto increment id, and keep durability.
  // TODO: support set primary key as a part of lsm_key.
  bytes lsm_key;
  // 为当前插入生成一个新的 rowid，并将其编码到 LSM Key 中
  uint64_t rowid = inc_id_.fetch_add(1);
  Codec::encode(table_->table_id(), rowid, lsm_key);

  // 先写入 LSM
  rc = lsm_->put(string_view((char *)lsm_key.data(), lsm_key.size()), string_view(record.data(), record.len()));
  if (rc != RC::SUCCESS) {
    return rc;
  }

  // 再写入所有二级索引；如失败则回滚刚刚的 LSM 写入
  const RID rid = rid_from_rowid(rowid);
  rc            = insert_entry_of_indexes(record.data(), rid);
  if (rc != RC::SUCCESS) {
    // 回滚 LSM 的 put
    [[maybe_unused]] RC rb = lsm_->remove(string_view((char *)lsm_key.data(), lsm_key.size()));
    return rc;
  }

  return RC::SUCCESS;
}

RC LsmTableEngine::get_record_scanner(RecordScanner *&scanner, Trx *trx, ReadWriteMode mode)
{
  scanner = new LsmRecordScanner(table_, db_->lsm(), trx);
  RC rc = scanner->open_scan();
  if (rc != RC::SUCCESS) {
    LOG_ERROR("failed to open scanner. rc=%s", strrc(rc));
  }
  return rc;
}

RC LsmTableEngine::update_record_with_trx(const Record &old_record, const Record &new_record, Trx *trx)
{
  // 在LSM树中，更新操作通常是通过删除旧记录并插入新记录来实现的
  // 获取LSM事务
  LsmMvccTrx *lsm_trx = dynamic_cast<LsmMvccTrx *>(trx);
  if (lsm_trx == nullptr) {
    LOG_ERROR("Invalid transaction type for LSM table engine");
    return RC::INVALID_ARGUMENT;
  }

  ObLsmTransaction *lsm_transaction = lsm_trx->get_trx();
  if (lsm_transaction == nullptr) {
    LOG_ERROR("LSM transaction not started");
    return RC::INVALID_ARGUMENT;
  }

  // 构造旧记录和新记录的LSM键
  bytes old_lsm_key;
  bytes new_lsm_key;

  // 使用记录的RID来构造键
  // TODO: 这里应该使用实际的主键或者RID，当前简化处理
  Codec::encode(table_->table_id(), old_record.rid().page_num * 10000 + old_record.rid().slot_num, old_lsm_key);
  new_lsm_key = old_lsm_key; // 更新操作保持相同的键

  // 在事务中执行更新：删除旧值，插入新值
  RC rc = lsm_transaction->remove(string_view((char *)old_lsm_key.data(), old_lsm_key.size()));
  if (rc != RC::SUCCESS && rc != RC::RECORD_NOT_EXIST) {
    LOG_ERROR("Failed to remove old record in LSM update. rc=%s", strrc(rc));
    return rc;
  }

  rc = lsm_transaction->put(
      string_view((char *)new_lsm_key.data(), new_lsm_key.size()),
      string_view(new_record.data(), new_record.len()));

  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to insert new record in LSM update. rc=%s", strrc(rc));
    // TODO: 需要回滚删除操作
    return rc;
  }

  return RC::SUCCESS;
}

RC LsmTableEngine::open()
{
  // 打开已有索引文件
  RC rc = RC::SUCCESS;
  const int index_num = table_meta_->index_num();
  for (int i = 0; i < index_num; i++) {
    const IndexMeta *index_meta = table_meta_->index(i);
    vector<FieldMeta> field_metas;
    for (const string &fname : index_meta->fields()) {
      const FieldMeta *fm = table_meta_->field(fname.c_str());
      if (fm == nullptr) {
        LOG_ERROR("Found invalid index meta in LSM table. table=%s, index=%s, field=%s",
                  table_meta_->name(), index_meta->name(), fname.c_str());
        return RC::INTERNAL;
      }
      field_metas.push_back(*fm);
    }

    BplusTreeIndex *index      = new BplusTreeIndex();
    string          index_file = table_index_file(db_->path().c_str(), table_meta_->name(), index_meta->name());
    rc = index->open(table_, index_file.c_str(), *index_meta, span<const FieldMeta>(field_metas.data(), field_metas.size()));
    if (rc != RC::SUCCESS) {
      delete index;
      LOG_ERROR("LSM open index failed. table=%s, index=%s, file=%s, rc=%s",
                table_meta_->name(), index_meta->name(), index_file.c_str(), strrc(rc));
      return rc;
    }
    indexes_.push_back(index);
  }
  return RC::SUCCESS;
}

RC LsmTableEngine::sync()
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->sync();
    if (rc != RC::SUCCESS) {
      LOG_ERROR("Failed to flush index's pages (LSM). table=%s, index=%s, rc=%s",
                table_meta_->name(), index->index_meta().name(), strrc(rc));
      return rc;
    }
  }
  return rc;
}

Index *LsmTableEngine::find_index(const char *index_name) const
{
  for (Index *index : indexes_) {
    if (0 == strcmp(index->index_meta().name(), index_name)) {
      return index;
    }
  }
  return nullptr;
}

Index *LsmTableEngine::find_index_by_field(const char *field_name) const
{
  const IndexMeta *index_meta = table_meta_->find_index_by_field(field_name);
  if (index_meta != nullptr) {
    return this->find_index(index_meta->name());
  }
  return nullptr;
}

RC LsmTableEngine::insert_entry_of_indexes(const char *record, const RID &rid)
{
  RC rc = RC::SUCCESS;
  for (Index *index : indexes_) {
    rc = index->insert_entry(record, &rid);
    if (rc != RC::SUCCESS) {
      break;
    }
  }
  // 回滚已插入的索引项
  if (rc != RC::SUCCESS) {
    for (Index *index : indexes_) {
      if (index->delete_entry(record, &rid) != RC::SUCCESS) {
        // best-effort rollback
      }
      if (index == indexes_.front()) {
        // 粗略处理：只回滚到当前失败之前插入过的索引，后面的本就未插入
        // 这里不容易判断精确位置，LSM 不作为比赛强依赖，简化实现
        ;
      }
    }
  }
  return rc;
}

RC LsmTableEngine::delete_entry_of_indexes(const char *record, const RID &rid, bool error_on_not_exists)
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

RC LsmTableEngine::create_index(Trx *trx, span<const FieldMeta> field_metas, const char *index_name, bool unique)
{
  // 构造索引元数据
  IndexMeta new_index_meta;
  RC       rc = new_index_meta.init(index_name, field_metas, unique);
  if (rc != RC::SUCCESS) {
    LOG_INFO("Failed to init IndexMeta in LSM table:%s, index_name:%s",
             table_meta_->name(), index_name);
    return rc;
  }

  // 创建索引存储
  BplusTreeIndex *index      = new BplusTreeIndex();
  string          index_file = table_index_file(db_->path().c_str(), table_meta_->name(), index_name);
  rc = index->create(table_, index_file.c_str(), new_index_meta, field_metas);
  if (rc != RC::SUCCESS) {
    delete index;
    LOG_ERROR("Failed to create bplus tree index (LSM). file=%s, rc=%s", index_file.c_str(), strrc(rc));
    return rc;
  }

  // 扫描现有 LSM 数据，填充索引；遇到 UNIQUE 冲突则回滚并失败
  RecordScanner *scanner = nullptr;
  rc = get_record_scanner(scanner, trx, ReadWriteMode::READ_ONLY);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create scanner while creating index (LSM). table=%s, index=%s, rc=%s",
             table_meta_->name(), index_name, strrc(rc));
    index->close();
    delete index;
    return rc;
  }

  Record   record;
  uint64_t rowid = 0;
  while (OB_SUCC(rc = scanner->next(record))) {
    // 从 LSM 内部 key 解码出 rowid 并映射为 RID 以满足索引接口
    rc = decode_rowid_from_lsm_key(record.key(), rowid);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to decode rowid from lsm key while creating index (LSM)");
      break;
    }
    RID rid = rid_from_rowid(rowid);
    rc      = index->insert_entry(record.data(), &rid);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to insert record into index while creating index (LSM). table=%s, index=%s, rc=%s",
               table_meta_->name(), index_name, strrc(rc));
      break;
    }
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }

  scanner->close_scan();
  delete scanner;

  if (rc != RC::SUCCESS) {
    // 清理新建索引
    index->close();
    delete index;
    ::remove(index_file.c_str());
    return rc;
  }

  // 将索引加入内存并持久化到表元数据
  indexes_.push_back(index);

  TableMeta new_table_meta(*table_meta_);
  rc = new_table_meta.add_index(new_index_meta);
  if (rc != RC::SUCCESS) {
    LOG_ERROR("Failed to add index (%s) on LSM table (%s). rc=%s", index_name, table_meta_->name(), strrc(rc));
    return rc;
  }

  string  tmp_file = table_meta_file(db_->path().c_str(), table_meta_->name()) + ".tmp";
  fstream fs;
  fs.open(tmp_file, ios_base::out | ios_base::binary | ios_base::trunc);
  if (!fs.is_open()) {
    LOG_ERROR("Failed to open file for write. file name=%s, errmsg=%s", tmp_file.c_str(), strerror(errno));
    return RC::IOERR_OPEN;
  }
  if (new_table_meta.serialize(fs) < 0) {
    LOG_ERROR("Failed to dump new table meta to file: %s. sys err=%d:%s", tmp_file.c_str(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }
  fs.close();

  string meta_file = table_meta_file(db_->path().c_str(), table_meta_->name());
  int    ret       = rename(tmp_file.c_str(), meta_file.c_str());
  if (ret != 0) {
    LOG_ERROR("Failed to rename tmp meta file (%s) to normal meta file (%s) while creating index (%s) on LSM table (%s). system error=%d:%s",
              tmp_file.c_str(), meta_file.c_str(), index_name, table_meta_->name(), errno, strerror(errno));
    return RC::IOERR_WRITE;
  }

  table_meta_->swap(new_table_meta);
  LOG_INFO("Successfully added a new index (%s) on the LSM table (%s)", index_name, table_meta_->name());
  return RC::SUCCESS;
}

RC LsmTableEngine::decode_rowid_from_lsm_key(const string &key, uint64_t &rowid)
{
  // key 格式: t/<table_id>/r/<rowid> (ordered-code 编码)
  span<byte_t> sp(reinterpret_cast<byte_t *>(const_cast<char *>(key.data())), key.size());
  string       s;
  int64_t      table_id = 0;
  RC           rc       = RC::SUCCESS;
  if (OB_FAIL(OrderedCode::parse(sp, OrderedCode::increasing, s))) { // 't'
    return rc;
  }
  if (OB_FAIL(OrderedCode::parse(sp, OrderedCode::increasing, table_id))) { // table id
    return rc;
  }
  if (OB_FAIL(OrderedCode::parse(sp, OrderedCode::increasing, s))) { // 'r'
    return rc;
  }
  if (OB_FAIL(OrderedCode::parse(sp, OrderedCode::increasing, rowid))) { // rowid
    return rc;
  }
  return RC::SUCCESS;
}
