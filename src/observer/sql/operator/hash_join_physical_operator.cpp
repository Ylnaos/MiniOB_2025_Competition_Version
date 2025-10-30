/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/hash_join_physical_operator.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"

HashJoinPhysicalOperator::HashJoinPhysicalOperator(unique_ptr<Expression> join_condition)
    : join_condition_(std::move(join_condition))
{}

RC HashJoinPhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 2) {
    LOG_WARN("hash join operator should have 2 children, but have %d", children_.size());
    return RC::INTERNAL;
  }

  trx_   = trx;
  left_  = children_[0].get();
  right_ = children_[1].get();

  // 打开左表并构建哈希表
  RC rc = left_->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open left child. rc=%s", strrc(rc));
    return rc;
  }

  rc = build_hash_table();
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to build hash table. rc=%s", strrc(rc));
    left_->close();
    return rc;
  }

  // 打开右表准备探测
  rc = right_->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open right child. rc=%s", strrc(rc));
    left_->close();
    return rc;
  }

  probe_started_ = false;
  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::build_hash_table()
{
  hash_table_.clear();
  shared_specs_.reset();

  // 获取连接条件的左右表达式
  if (join_condition_ == nullptr) {
    LOG_WARN("hash join requires a join condition");
    return RC::INTERNAL;
  }

  // 遍历左表，构建哈希表
  RC rc = RC::SUCCESS;
  while (RC::SUCCESS == (rc = left_->next())) {
    Tuple *left_tuple = left_->current_tuple();
    if (left_tuple == nullptr) {
      LOG_WARN("left child returned null tuple");
      return RC::INTERNAL;
    }

    // 第一次迭代时，保存tuple schema
    if (!shared_specs_) {
      auto specs_holder = std::make_shared<vector<TupleCellSpec>>();
      specs_holder->reserve(left_tuple->cell_num());
      for (int i = 0; i < left_tuple->cell_num(); i++) {
        TupleCellSpec spec;
        RC            tmp_rc = left_tuple->spec_at(i, spec);
        if (OB_FAIL(tmp_rc)) {
          LOG_WARN("failed to fetch tuple spec. rc=%s", strrc(tmp_rc));
          return tmp_rc;
        }
        specs_holder->push_back(spec);
      }
      shared_specs_ = std::move(specs_holder);
    }

    // 物化左表tuple
    ValueListTuple materialized_tuple;
    materialized_tuple.set_shared_specs(shared_specs_);
    rc = ValueListTuple::make(*left_tuple, materialized_tuple);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to materialize left tuple. rc=%s", strrc(rc));
      return rc;
    }

    // 提取hash key (使用连接条件的左侧表达式)
    // 对于等值连接 a.id = b.id，左表使用 a.id
    Value key_value;

    // 假设join_condition_是ComparisonExpr
    ComparisonExpr *comp_expr = dynamic_cast<ComparisonExpr *>(join_condition_.get());
    if (comp_expr == nullptr || comp_expr->comp() != CompOp::EQUAL_TO) {
      LOG_WARN("hash join only supports equality comparison");
      return RC::INTERNAL;
    }

    Expression *left_expr = comp_expr->left().get();
    rc = left_expr->get_value(*left_tuple, key_value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to extract hash key from left tuple. rc=%s", strrc(rc));
      return rc;
    }

    // 插入哈希表
    HashKey hash_key{key_value};
    hash_table_.insert({hash_key, std::move(materialized_tuple)});
  }

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("failed to iterate left child. rc=%s", strrc(rc));
    return rc;
  }

  LOG_TRACE("built hash table with %zu entries", hash_table_.size());
  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::next()
{
  RC rc = RC::SUCCESS;

  while (true) {
    // 如果当前右表tuple还有匹配的左表tuple，返回下一个
    if (probe_started_ && match_current_ != match_end_) {
      joined_tuple_.set_left(&match_current_->second);
      joined_tuple_.set_right(right_tuple_);
      ++match_current_;
      return RC::SUCCESS;
    }

    // 获取下一个右表tuple
    rc = right_->next();
    if (rc != RC::SUCCESS) {
      return rc;  // EOF 或错误
    }

    right_tuple_ = right_->current_tuple();
    if (right_tuple_ == nullptr) {
      LOG_WARN("right child returned null tuple");
      return RC::INTERNAL;
    }

    // 提取右表的hash key (使用连接条件的右侧表达式)
    Value key_value;
    ComparisonExpr *comp_expr = dynamic_cast<ComparisonExpr *>(join_condition_.get());
    if (comp_expr == nullptr) {
      LOG_WARN("join condition is not a comparison expression");
      return RC::INTERNAL;
    }

    Expression *right_expr = comp_expr->right().get();
    rc = right_expr->get_value(*right_tuple_, key_value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to extract hash key from right tuple. rc=%s", strrc(rc));
      return rc;
    }

    // 在哈希表中查找匹配
    HashKey hash_key{key_value};
    auto range = hash_table_.equal_range(hash_key);
    match_begin_   = range.first;
    match_end_     = range.second;
    match_current_ = match_begin_;
    probe_started_ = true;

    // 如果找到匹配，返回第一个
    if (match_current_ != match_end_) {
      joined_tuple_.set_left(&match_current_->second);
      joined_tuple_.set_right(right_tuple_);
      ++match_current_;
      return RC::SUCCESS;
    }

    // 没有匹配，继续下一个右表tuple
  }

  return RC::RECORD_EOF;
}

RC HashJoinPhysicalOperator::close()
{
  RC rc = RC::SUCCESS;

  if (left_ != nullptr) {
    RC left_rc = left_->close();
    if (OB_FAIL(left_rc)) {
      LOG_WARN("failed to close left child. rc=%s", strrc(left_rc));
      rc = left_rc;
    }
  }

  if (right_ != nullptr) {
    RC right_rc = right_->close();
    if (OB_FAIL(right_rc)) {
      LOG_WARN("failed to close right child. rc=%s", strrc(right_rc));
      rc = right_rc;
    }
  }

  hash_table_.clear();
  shared_specs_.reset();
  probe_started_ = false;

  return rc;
}

Tuple *HashJoinPhysicalOperator::current_tuple()
{
  return &joined_tuple_;
}
