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

namespace {

RC encode_lsm_key(int32_t table_id, uint64_t row_id, bytes &lsm_key)
{
  return Codec::encode(table_id, row_id, lsm_key);
}

uint64_t row_id_from_rid(const RID &rid)
{
  return static_cast<uint64_t>(static_cast<uint32_t>(rid.slot_num));
}

void set_rid_from_row_id(Record &record, uint64_t row_id)
{
  record.set_rid(0, static_cast<SlotNum>(row_id));
}

}  // namespace

RC LsmTableEngine::insert_record(Record &record)
{
  RC rc = RC::SUCCESS;
  // TODO: set auto increment id, and keep durability.
  // TODO: support set primary key as a part of lsm_key.
  uint64_t row_id = inc_id_.fetch_add(1);
  bytes lsm_key;
  Codec::encode(table_->table_id(), row_id, lsm_key);
  rc = lsm_->put(string_view((char *)lsm_key.data(), lsm_key.size()), string_view(record.data(), record.len()));
  if (OB_SUCC(rc)) {
    set_rid_from_row_id(record, row_id);
    record.set_key(string(reinterpret_cast<char *>(lsm_key.data()), lsm_key.size()));
  }
  return rc;
}

RC LsmTableEngine::delete_record(const Record &record)
{
  string key = record.key();
  if (key.empty()) {
    bytes lsm_key;
    RC rc = encode_lsm_key(table_->table_id(), row_id_from_rid(record.rid()), lsm_key);
    if (OB_FAIL(rc)) {
      return rc;
    }
    key.assign(reinterpret_cast<char *>(lsm_key.data()), lsm_key.size());
  }
  return lsm_->remove(key);
}

RC LsmTableEngine::insert_record_with_trx(Record &record, Trx *trx)
{
  LsmMvccTrx *lsm_trx = dynamic_cast<LsmMvccTrx *>(trx);
  if (lsm_trx == nullptr) {
    return insert_record(record);
  }

  RC rc = lsm_trx->start_if_need();
  if (OB_FAIL(rc)) {
    return rc;
  }
  ObLsmTransaction *lsm_transaction = lsm_trx->get_trx();
  if (lsm_transaction == nullptr) {
    return RC::INTERNAL;
  }

  uint64_t row_id = inc_id_.fetch_add(1);
  bytes lsm_key;
  rc = encode_lsm_key(table_->table_id(), row_id, lsm_key);
  if (OB_FAIL(rc)) {
    return rc;
  }
  rc = lsm_transaction->put(
      string_view(reinterpret_cast<char *>(lsm_key.data()), lsm_key.size()), string_view(record.data(), record.len()));
  if (OB_SUCC(rc)) {
    set_rid_from_row_id(record, row_id);
    record.set_key(string(reinterpret_cast<char *>(lsm_key.data()), lsm_key.size()));
  }
  return rc;
}

RC LsmTableEngine::delete_record_with_trx(const Record &record, Trx *trx)
{
  LsmMvccTrx *lsm_trx = dynamic_cast<LsmMvccTrx *>(trx);
  if (lsm_trx == nullptr) {
    return delete_record(record);
  }

  RC rc = lsm_trx->start_if_need();
  if (OB_FAIL(rc)) {
    return rc;
  }
  ObLsmTransaction *lsm_transaction = lsm_trx->get_trx();
  if (lsm_transaction == nullptr) {
    return RC::INTERNAL;
  }

  string key = record.key();
  if (key.empty()) {
    bytes lsm_key;
    rc = encode_lsm_key(table_->table_id(), row_id_from_rid(record.rid()), lsm_key);
    if (OB_FAIL(rc)) {
      return rc;
    }
    key.assign(reinterpret_cast<char *>(lsm_key.data()), lsm_key.size());
  }
  return lsm_transaction->remove(key);
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
  RC rc = RC::SUCCESS;

  string old_lsm_key = old_record.key();
  if (old_lsm_key.empty()) {
    bytes encoded_key;
    rc = encode_lsm_key(table_->table_id(), row_id_from_rid(old_record.rid()), encoded_key);
    if (OB_FAIL(rc)) {
      return rc;
    }
    old_lsm_key.assign(reinterpret_cast<char *>(encoded_key.data()), encoded_key.size());
  }

  // 在LSM树中，更新操作通常是通过删除旧记录并插入新记录来实现的
  // 获取LSM事务
  LsmMvccTrx *lsm_trx = dynamic_cast<LsmMvccTrx *>(trx);
  if (lsm_trx == nullptr) {
    return lsm_->put(old_lsm_key, string_view(new_record.data(), new_record.len()));
  }

  rc = lsm_trx->start_if_need();
  if (OB_FAIL(rc)) {
    return rc;
  }

  ObLsmTransaction *lsm_transaction = lsm_trx->get_trx();
  if (lsm_transaction == nullptr) {
    LOG_ERROR("LSM transaction not started");
    return RC::INVALID_ARGUMENT;
  }

  // 在事务中执行更新：删除旧值，插入新值
  rc = lsm_transaction->remove(old_lsm_key);
  if (rc != RC::SUCCESS && rc != RC::RECORD_NOT_EXIST) {
    LOG_ERROR("Failed to remove old record in LSM update. rc=%s", strrc(rc));
    return rc;
  }

  rc = lsm_transaction->put(
      string_view(old_lsm_key.data(), old_lsm_key.size()),
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
  return lsm_ == nullptr ? RC::INTERNAL : RC::SUCCESS;
}

RC LsmTableEngine::get_record(const RID &rid, Record &record)
{
  bytes lsm_key;
  RC rc = encode_lsm_key(table_->table_id(), row_id_from_rid(rid), lsm_key);
  if (OB_FAIL(rc)) {
    return rc;
  }

  string value;
  string key(reinterpret_cast<char *>(lsm_key.data()), lsm_key.size());
  rc = lsm_->get(key, &value);
  if (OB_FAIL(rc)) {
    return rc;
  }
  rc = record.copy_data(value.data(), static_cast<int>(value.size()));
  if (OB_SUCC(rc)) {
    record.set_rid(rid);
    record.set_key(key);
  }
  return rc;
}
