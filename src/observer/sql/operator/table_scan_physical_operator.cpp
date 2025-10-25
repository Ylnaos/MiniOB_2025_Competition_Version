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
#include "common/log/log.h"

using namespace std;

// 全局计数器，用于追踪数据流
static int table_scan_open_count = 0;
static int table_scan_next_count = 0;
static int table_scan_records_yielded = 0;

RC TableScanPhysicalOperator::open(Trx *trx)
{
  table_scan_open_count++;

  LOG_INFO("DATA_FLOW: TableScanPhysicalOperator::open - 操作符=%p, 表名=%s, 打开次数=%d",
            this, table_->name(), table_scan_open_count);

  RC rc = table_->get_record_scanner(record_scanner_, trx, mode_);
  if (rc == RC::SUCCESS) {
    tuple_.set_schema(table_, table_->table_meta().field_metas(), alias_);
    LOG_INFO("DATA_FLOW: TableScanPhysicalOperator::open成功 - 操作符=%p, 表名=%s, 元组模式设置完成",
              this, table_->name());
  } else {
    LOG_INFO("DATA_FLOW: TableScanPhysicalOperator::open失败 - 操作符=%p, 表名=%s, RC=%s",
              this, table_->name(), strrc(rc));
  }

  trx_ = trx;
  return rc;
}

RC TableScanPhysicalOperator::next()
{
  table_scan_next_count++;

  // 强制输出每行数据扫描
  printf("=== FORCE OUTPUT: TableScan::next[%d] 表=%s ===\n", table_scan_next_count, table_->name());
  fflush(stdout);

  LOG_INFO("DATA_FLOW: TableScanPhysicalOperator::next开始 - 操作符=%p, 表名=%s, next调用次数=%d",
            this, table_->name(), table_scan_next_count);

  // 检查record_scanner_是否有效
  if (record_scanner_ == nullptr) {
    LOG_ERROR("DATA_FLOW: TableScanPhysicalOperator::next错误 - 操作符=%p, 表名=%s, record_scanner_为空",
              this, table_->name());
    return RC::INTERNAL;
  }

  RC rc = RC::SUCCESS;
  bool filter_result = false;

  // 使用循环查找下一条通过过滤器的记录，但限制最大尝试次数避免无限循环
  int attempt_count = 0;
  const int MAX_ATTEMPTS_PER_NEXT = 1000; // 防止无限循环的安全限制

  while (OB_SUCC(rc = record_scanner_->next(current_record_)) && attempt_count < MAX_ATTEMPTS_PER_NEXT) {
    attempt_count++;
    LOG_TRACE("got a record. rid=%s", current_record_.rid().to_string().c_str());

    tuple_.set_record(&current_record_);

    LOG_INFO("DATA_FLOW: TableScanPhysicalOperator读取记录 - 操作符=%p, 表名=%s, 记录rid=%s, 记录数据=%s, 尝试次数=%d",
              this, table_->name(), current_record_.rid().to_string().c_str(), tuple_.to_string().c_str(), attempt_count);

    // 检查记录是否有效（额外的安全检查）
    if (current_record_.rid().page_num < 0 || current_record_.rid().slot_num < 0) {
      LOG_WARN("DATA_FLOW: TableScanPhysicalOperator遇到无效记录 - 操作符=%p, 表名=%s, rid=%s",
               this, table_->name(), current_record_.rid().to_string().c_str());
      continue;
    }

    rc = filter(tuple_, filter_result);
    if (rc != RC::SUCCESS) {
      LOG_TRACE("record filtered failed=%s", strrc(rc));
      return rc;
    }

    if (filter_result) {
      table_scan_records_yielded++;
      LOG_INFO("DATA_FLOW: TableScanPhysicalOperator输出元组 - 操作符=%p, 表名=%s, 元组数据=%s, 已输出记录数=%d",
                this, table_->name(), tuple_.to_string().c_str(), table_scan_records_yielded);
      sql_debug("get a tuple: %s", tuple_.to_string().c_str());
      return RC::SUCCESS;
    } else {
      LOG_INFO("DATA_FLOW: TableScanPhysicalOperator过滤元组 - 操作符=%p, 表名=%s, 元组数据=%s",
                this, table_->name(), tuple_.to_string().c_str());
      sql_debug("a tuple is filtered: %s", tuple_.to_string().c_str());
    }
  }

  // 检查是否因为尝试次数过多而退出
  if (attempt_count >= MAX_ATTEMPTS_PER_NEXT) {
    LOG_ERROR("DATA_FLOW: TableScanPhysicalOperator::next超过最大尝试次数 - 操作符=%p, 表名=%s, 尝试次数=%d",
              this, table_->name(), attempt_count);
    return RC::INTERNAL;
  }

  if (rc == RC::RECORD_EOF) {
    LOG_INFO("DATA_FLOW: TableScanPhysicalOperator::next完成 - 操作符=%p, 表名=%s, 到达数据末尾, 总next调用=%d, 总输出记录=%d, 最后尝试次数=%d",
              this, table_->name(), table_scan_next_count, table_scan_records_yielded, attempt_count);
  } else if (rc != RC::SUCCESS) {
    LOG_INFO("DATA_FLOW: TableScanPhysicalOperator::next错误 - 操作符=%p, 表名=%s, RC=%s, 尝试次数=%d",
              this, table_->name(), strrc(rc), attempt_count);
  }

  return rc;
}

RC TableScanPhysicalOperator::close() {
  LOG_INFO("DATA_FLOW: TableScanPhysicalOperator::close开始 - 操作符=%p, 表名=%s", this, table_->name());

  RC rc = RC::SUCCESS;
  if (record_scanner_ != nullptr) {
    LOG_INFO("DATA_FLOW: TableScanPhysicalOperator关闭记录扫描器 - 操作符=%p, 表名=%s, 扫描器=%p",
              this, table_->name(), record_scanner_);

    rc = record_scanner_->close_scan();
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to close record scanner. rc=%s", strrc(rc));
    } else {
      LOG_INFO("DATA_FLOW: TableScanPhysicalOperator记录扫描器关闭成功 - 操作符=%p, 表名=%s", this, table_->name());
    }

    delete record_scanner_;
    record_scanner_ = nullptr;
  } else {
    LOG_INFO("DATA_FLOW: TableScanPhysicalOperator记录扫描器已经为空 - 操作符=%p, 表名=%s", this, table_->name());
  }

  // 重置状态
  current_record_ = Record();
  LOG_INFO("DATA_FLOW: TableScanPhysicalOperator::close完成 - 操作符=%p, 表名=%s, RC=%s",
            this, table_->name(), strrc(rc));
  return rc;
}

Tuple *TableScanPhysicalOperator::current_tuple()
{
  tuple_.set_record(&current_record_);
  LOG_INFO("DATA_FLOW: TableScanPhysicalOperator::current_tuple - 操作符=%p, 表名=%s, 返回元组=%s",
            this, table_->name(), tuple_.to_string().c_str());
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
