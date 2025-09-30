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

InsertPhysicalOperator::InsertPhysicalOperator(Table *table, vector<vector<Value>> &&values_rows, bool from_view)
    : table_(table), values_rows_(std::move(values_rows)), from_view_(from_view)
{}

RC InsertPhysicalOperator::open(Trx *trx)
{
  RC rc = RC::SUCCESS;
  vector<RID> inserted_rids;

  for (const auto &row_values : values_rows_) {
    Record record;
    rc = table_->make_record(static_cast<int>(row_values.size()), row_values.data(), record, from_view_);
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
