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

RC LsmTableEngine::insert_record(Record &record)
{
  RC rc = RC::SUCCESS;
  // TODO: set auto increment id, and keep durability.
  // TODO: support set primary key as a part of lsm_key.
  bytes lsm_key;
  Codec::encode(table_->table_id(), inc_id_.fetch_add(1), lsm_key);
  rc = lsm_->put(string_view((char *)lsm_key.data(), lsm_key.size()), string_view(record.data(), record.len()));
  return rc;
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
  return RC::UNIMPLEMENTED;
}