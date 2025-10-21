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

InsertPhysicalOperator::InsertPhysicalOperator(vector<InsertTask> tasks)
    : insert_tasks_(std::move(tasks))
{}

RC InsertPhysicalOperator::open(Trx *trx)
{
  RC rc = RC::SUCCESS;
  struct InsertedRecord
  {
    Table *table = nullptr;
    RID    rid;
  };
  vector<InsertedRecord> inserted_records;

  for (const auto &task : insert_tasks_) {
    Table *table = task.table;
    if (table == nullptr) {
      LOG_WARN("insert physical operator got null table");
      rc = RC::INTERNAL;
      break;
    }

    for (const auto &row_values : task.rows) {
      Record record;
      rc = table->make_record(static_cast<int>(row_values.size()), row_values.data(), record);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to make record. table=%s rc=%s", table->name(), strrc(rc));
        break;
      }

      rc = trx->insert_record(table, record);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to insert record by transaction. table=%s rc=%s", table->name(), strrc(rc));
        break;
      }
      inserted_records.push_back({table, record.rid()});
    }

    if (rc != RC::SUCCESS) {
      break;
    }
  }

  if (rc != RC::SUCCESS) {
    for (auto it = inserted_records.rbegin(); it != inserted_records.rend(); ++it) {
      Record rec;
      RC rc_get = it->table->get_record(it->rid, rec);
      if (rc_get != RC::SUCCESS) {
        LOG_WARN("failed to get record for rollback. table=%s rid=%s rc=%s",
            it->table->name(), it->rid.to_string().c_str(), strrc(rc_get));
        continue;
      }
      RC rc_del = trx->delete_record(it->table, rec);
      if (rc_del != RC::SUCCESS) {
        LOG_WARN("failed to delete record for rollback. table=%s rid=%s rc=%s",
            it->table->name(), it->rid.to_string().c_str(), strrc(rc_del));
      }
    }
  }

  return rc;
}

RC InsertPhysicalOperator::next() { return RC::RECORD_EOF; }

RC InsertPhysicalOperator::close() { return RC::SUCCESS; }
