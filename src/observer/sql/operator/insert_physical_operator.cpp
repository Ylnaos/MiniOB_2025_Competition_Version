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
// Created by WangYunlai on 2021/6/9.
//

#include "sql/operator/insert_physical_operator.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

using namespace std;

InsertPhysicalOperator::InsertPhysicalOperator(Table *table, vector<vector<Value>> &&values_rows)
    : table_(table), values_rows_(std::move(values_rows))
{}

RC InsertPhysicalOperator::open(Trx *trx)
{
  RC rc = RC::SUCCESS;
  vector<RID> inserted_rids;

  for (const auto &row_values : values_rows_) {
    // 1) 行级列数校验（与表可见列数一致）
    const TableMeta &table_meta = table_->table_meta();
    const int expected_values = table_meta.field_num() - table_meta.sys_field_num();
    if (static_cast<int>(row_values.size()) != expected_values) {
      LOG_WARN("insert values count mismatch. table=%s expect=%d got=%zu",
               table_meta.name(), expected_values, row_values.size());
      rc = RC::SCHEMA_FIELD_MISSING;
      break;
    }

    // 2) 向量字段的维度预校验（提前失败，便于与官方期望一致返回 FAILURE）
    // 与 make_record -> set_value_to_record 的严格校验保持一致
    for (int i = 0; i < expected_values; ++i) {
      const FieldMeta *field_meta = table_meta.field(i + table_meta.sys_field_num());
      if (field_meta == nullptr) {
        rc = RC::INTERNAL;
        break;
      }
      if (field_meta->type() == AttrType::VECTORS && field_meta->len() != static_cast<int>(sizeof(LobRef))) {
        Value src = row_values[i];
        if (src.attr_type() != AttrType::VECTORS) {
          Value casted;
          RC    rc2 = Value::cast_to(src, AttrType::VECTORS, casted);
          if (OB_FAIL(rc2)) { rc = rc2; break; }
          src = std::move(casted);
        }
        const int expect_len = field_meta->len();
        const int actual_len = src.length();
        if (actual_len != expect_len || (actual_len % static_cast<int>(sizeof(float)) != 0)) {
          const int expect_dim = expect_len / static_cast<int>(sizeof(float));
          const int actual_dim = actual_len / static_cast<int>(sizeof(float));
          LOG_WARN("vector dimension mismatch before insert. table=%s field=%s expect_dim=%d actual_dim=%d",
                   table_meta.name(), field_meta->name(), expect_dim, actual_dim);
          rc = RC::INVALID_ARGUMENT;
          break;
        }
      }
    }
    if (OB_FAIL(rc)) { break; }

    Record record;
    rc = table_->make_record(static_cast<int>(row_values.size()), row_values.data(), record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to make record. rc=%s", strrc(rc));
      break;
    }

    rc = trx->insert_record(table_, record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to insert record by transaction. rc=%s", strrc(rc));
      break;
    }
    inserted_rids.push_back(record.rid());
  }

  if (rc != RC::SUCCESS) {
    // 发生错误，回滚本语句已插入的行，保证一次插入要么全部成功要么全部失败
    for (auto it = inserted_rids.rbegin(); it != inserted_rids.rend(); ++it) {
      const RID &rid = *it;
      Record rec;
      RC rc_get = table_->get_record(rid, rec);
      if (rc_get != RC::SUCCESS) {
        LOG_WARN("failed to get record for rollback. rid=%s rc=%s", rid.to_string().c_str(), strrc(rc_get));
        continue;
      }
      RC rc_del = trx->delete_record(table_, rec);
      if (rc_del != RC::SUCCESS) {
        LOG_WARN("failed to delete record for rollback. rid=%s rc=%s", rid.to_string().c_str(), strrc(rc_del));
      }
    }
  }

  return rc;
}

RC InsertPhysicalOperator::next() { return RC::RECORD_EOF; }

RC InsertPhysicalOperator::close() { return RC::SUCCESS; }
