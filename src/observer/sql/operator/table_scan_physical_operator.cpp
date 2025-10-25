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

#include "sql/operator/table_scan_physical_operator.h"
#include "event/sql_debug.h"
#include "storage/table/table.h"

#include <sstream>
#include <utility>

using namespace std;

namespace {

template <typename... Args>
void exec_trace(const char *fmt, Args &&... args)
{
  LOG_INFO(fmt, std::forward<Args>(args)...);
  sql_debug(fmt, std::forward<Args>(args)...);
}

}  // namespace

RC TableScanPhysicalOperator::open(Trx *trx)
{
  RC rc = table_->get_record_scanner(record_scanner_, trx, mode_);
  if (rc == RC::SUCCESS) {
    tuple_.set_schema(table_, table_->table_meta().field_metas(), alias_);
  }
  trx_ = trx;
  return rc;
}

RC TableScanPhysicalOperator::next()
{
  RC rc = RC::SUCCESS;

  bool filter_result = false;
  while (OB_SUCC(rc = record_scanner_->next(current_record_))) {
    const string rid_string = current_record_.rid().to_string();
    tuple_.set_record(&current_record_);
    const string tuple_string = tuple_.to_string();
    exec_trace("[TableScan][Row] table=%s rid=%s data=%s", table_->name(), rid_string.c_str(), tuple_string.c_str());

    rc = filter(tuple_, filter_result);
    if (rc != RC::SUCCESS) {
      LOG_TRACE("record filtered failed=%s", strrc(rc));
      return rc;
    }

    if (filter_result) {
      exec_trace("[TableScan][Pass] table=%s rid=%s data=%s", table_->name(), rid_string.c_str(), tuple_string.c_str());
      break;
    } else {
      exec_trace("[TableScan][Skip] table=%s rid=%s data=%s", table_->name(), rid_string.c_str(), tuple_string.c_str());
    }
  }
  return rc;
}

RC TableScanPhysicalOperator::close() {
  RC rc = RC::SUCCESS;
  if (record_scanner_ != nullptr) {
    rc = record_scanner_->close_scan();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to close record scanner");
    }
    delete record_scanner_;
    record_scanner_ = nullptr;
  }
  return rc;

}

Tuple *TableScanPhysicalOperator::current_tuple()
{
  tuple_.set_record(&current_record_);
  return &tuple_;
}

string TableScanPhysicalOperator::param() const { return table_->name(); }

void TableScanPhysicalOperator::set_predicates(vector<unique_ptr<Expression>> &&exprs)
{
  predicates_ = std::move(exprs);
}

RC TableScanPhysicalOperator::filter(RowTuple &tuple, bool &result)
{
  RC    rc = RC::SUCCESS;
  Value value;
  for (unique_ptr<Expression> &expr : predicates_) {
    rc = expr->get_value(tuple, value);
    if (rc != RC::SUCCESS) {
      return rc;
    }

    bool tmp_result = value.get_boolean();
    if (!tmp_result) {
      result = false;
      return rc;
    }
  }

  result = true;
  return rc;
}
