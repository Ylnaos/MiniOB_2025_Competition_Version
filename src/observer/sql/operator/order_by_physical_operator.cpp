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

  // 读取数据并物化；若存在 limit_，使用小顶堆/大顶堆保留前K
  vector<ValueListTuple> all_rows;
  struct Cmp {
    const OrderByPhysicalOperator *self;
    bool operator()(const ValueListTuple &a, const ValueListTuple &b) const {
      return self->compare_rows(a, b) < 0; // a < b => true (小顶堆需要相反)
    }
  } cmp{this};
  std::priority_queue<ValueListTuple, vector<ValueListTuple>, Cmp> heap(cmp);

  while (RC::SUCCESS == (rc = children_[0]->next())) {
    Tuple *child_tuple = children_[0]->current_tuple();
    if (child_tuple == nullptr) {
      LOG_WARN("child returned null tuple");
      return RC::INTERNAL;
    }
    ValueListTuple row;
    rc = ValueListTuple::make(*child_tuple, row);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to materialize tuple. rc=%s", strrc(rc));
      return rc;
    }
    if (limit_ > 0) {
      if ((int)heap.size() < limit_) {
        heap.emplace(std::move(row));
      } else {
        // 若新行比堆顶更优（更小），则替换
        if (compare_rows(row, heap.top()) < 0) {
          heap.pop();
          heap.emplace(std::move(row));
        }
      }
    } else {
      all_rows.emplace_back(std::move(row));
    }
  }

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("child next failed. rc=%s", strrc(rc));
    return rc;
  }

  if (limit_ > 0) {
    // 从堆中弹出，得到升序
    rows_.resize(heap.size());
    for (int i = static_cast<int>(heap.size()) - 1; i >= 0; --i) {
      rows_[i] = std::move(const_cast<ValueListTuple &>(heap.top()));
      heap.pop();
    }
  } else {
    rows_.swap(all_rows);
    std::sort(rows_.begin(), rows_.end(), [this](const ValueListTuple &a, const ValueListTuple &b) {
      int cmp = compare_rows(a, b);
      return cmp < 0;
    });
  }

  current_index_ = 0;
  opened_        = true;
  return RC::SUCCESS;
}

int OrderByPhysicalOperator::compare_rows(const ValueListTuple &lhs, const ValueListTuple &rhs) const
{
  RC    rc = RC::SUCCESS;
  Value lv;
  Value rv;
  for (const auto &item : order_by_items_) {
    Expression *expr = item.first.get();
    bool        asc  = item.second;

    rc = expr->get_value(lhs, lv);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to get value from left row for order by. rc=%s", strrc(rc));
      return 0;
    }
    rc = expr->get_value(rhs, rv);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to get value from right row for order by. rc=%s", strrc(rc));
      return 0;
    }

    int c = 0;
    Value::compare(lv, rv, c);
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
  return &rows_[idx];
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
