/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "sql/operator/physical_operator.h"
#include "sql/expr/expression.h"
#include <cstdio>
#include <filesystem>
#include <memory>
#include <queue>
#include <string>
#include <vector>

/**
 * @brief ORDER BY 物理算子
 */
class OrderByPhysicalOperator : public PhysicalOperator
{
public:
  using OrderItem = pair<unique_ptr<Expression>, bool>; // bool: true for ASC, false for DESC

  OrderByPhysicalOperator(vector<OrderItem> &&order_by_items);

  virtual ~OrderByPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::ORDER_BY; }
  OpType               get_op_type() const override { return OpType::ORDERBY; }

  RC open(Trx *trx) override;
  RC next() override;
  RC close() override;

  Tuple *current_tuple() override;

private:
  // 用于存储每一行及其预计算的排序键值
  struct RowWithKeys {
    ValueListTuple row;
    vector<Value>  sort_keys; // 预计算的排序键值
  };

  struct RunCursor {
    FILE         *file     = nullptr;
    RowWithKeys   current;
    std::string   file_path;
  };

  struct MergeComparator {
    const OrderByPhysicalOperator *op = nullptr;
    bool operator()(int lhs, int rhs) const;
  };

  using MergeHeap = std::priority_queue<int, std::vector<int>, MergeComparator>;

  int  compare_rows(const RowWithKeys &lhs, const RowWithKeys &rhs) const;
  int  compare_keys(const vector<Value> &lhs, const vector<Value> &rhs) const;
  RC   compute_sort_keys(const Tuple &tuple, vector<Value> &keys) const;
  RC   flush_rows_to_run();
  RC   write_row(FILE *file, const ValueListTuple &tuple) const;
  RC   read_next_row(RunCursor &cursor);
  RC   deserialize_row(FILE *file, ValueListTuple &tuple);
  RC   ensure_temp_dir();
  void cleanup_external_resources();

private:
  vector<OrderItem>       order_by_items_;
  vector<RowWithKeys>     rows_;
  size_t                  current_index_ = 0;
  bool                    opened_        = false;
  std::shared_ptr<vector<TupleCellSpec>> shared_specs_;
  bool                    use_external_sort_      = false;
  bool                    has_current_external_row_ = false;
  RowWithKeys             current_external_row_;
  std::filesystem::path   temp_dir_path_;
  bool                    temp_dir_ready_         = false;
  vector<std::unique_ptr<RunCursor>> run_cursors_;
  vector<std::filesystem::path>      run_files_;
  MergeHeap               merge_heap_;
  size_t                  chunk_limit_rows_ = 0;
};
