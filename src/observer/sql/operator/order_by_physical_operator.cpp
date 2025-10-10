/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/order_by_physical_operator.h"
#include "common/log/log.h"

RC OrderByPhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 1) {
    LOG_WARN("order by operator expects exactly 1 child, got %d", children_.size());
    return RC::INTERNAL;
  }

  RC rc = children_[0]->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open child operator. rc=%s", strrc(rc));
    return rc;
  }

  // 添加最大行数限制，防止内存溢出
  const size_t MAX_ROWS = 500000;  // 最多缓存50万行

  // 读取所有数据并物化
  while (RC::SUCCESS == (rc = children_[0]->next())) {
    // 检查是否超过最大行数限制
    if (rows_.size() >= MAX_ROWS) {
      LOG_WARN("order by result set too large, exceeds maximum of %zu rows", MAX_ROWS);
      return RC::INTERNAL;
    }

    Tuple *child_tuple = children_[0]->current_tuple();
    if (child_tuple == nullptr) {
      LOG_WARN("child returned null tuple");
      return RC::INTERNAL;
    }

    RowWithKeys row_with_keys;
    rc = ValueListTuple::make(*child_tuple, row_with_keys.row);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to materialize tuple. rc=%s", strrc(rc));
      return rc;
    }

    // 预计算所有ORDER BY表达式的值
    row_with_keys.sort_keys.reserve(order_by_items_.size());
    for (const auto &item : order_by_items_) {
      Value key_value;
      rc = item.first->get_value(row_with_keys.row, key_value);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to get order by key value. rc=%s", strrc(rc));
        return rc;
      }
      row_with_keys.sort_keys.push_back(std::move(key_value));
    }

    rows_.emplace_back(std::move(row_with_keys));
  }

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("child next failed. rc=%s", strrc(rc));
    return rc;
  }

  // 排序 - 现在使用预计算的键值进行比较
  std::sort(rows_.begin(), rows_.end(), [this](const RowWithKeys &a, const RowWithKeys &b) {
    int cmp = compare_rows(a, b);
    return cmp < 0;
  });

  current_index_ = 0;
  opened_        = true;
  return RC::SUCCESS;
}

int OrderByPhysicalOperator::compare_rows(const RowWithKeys &lhs, const RowWithKeys &rhs) const
{
  // 使用预计算的sort_keys进行比较
  for (size_t i = 0; i < order_by_items_.size(); ++i) {
    bool asc = order_by_items_[i].second;

    int c = 0;
    Value::compare(lhs.sort_keys[i], rhs.sort_keys[i], c);
    if (c == 0) {
      continue;
    }
    if (asc) {
      return c;
    } else {
      return -c;
    }
  }
  return 0;
}

RC OrderByPhysicalOperator::next()
{
  if (!opened_) {
    return RC::INTERNAL;
  }
  if (current_index_ >= rows_.size()) {
    return RC::RECORD_EOF;
  }

  if (current_index_ > 0) {
    // 移动到下一个
    ++current_index_;
  } else {
    // 第一次 next 之后不增加，current_tuple 会返回第一个
    current_index_ = 1; // 使得后续 next() 能推进
  }

  if (current_index_ > rows_.size()) {
    return RC::RECORD_EOF;
  }
  return RC::SUCCESS;
}

Tuple *OrderByPhysicalOperator::current_tuple()
{
  if (!opened_ || rows_.empty()) {
    return nullptr;
  }
  size_t idx = (current_index_ == 0 ? 0 : current_index_ - 1);
  if (idx >= rows_.size()) {
    return nullptr;
  }
  return &rows_[idx].row;
}

RC OrderByPhysicalOperator::close()
{
  if (!children_.empty()) {
    children_[0]->close();
  }
  rows_.clear();
  opened_        = false;
  current_index_ = 0;
  return RC::SUCCESS;
}

