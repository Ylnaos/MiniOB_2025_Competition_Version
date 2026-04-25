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

HashJoinPhysicalOperator::HashJoinPhysicalOperator(unique_ptr<Expression> left_expr, unique_ptr<Expression> right_expr)
    : left_expr_(std::move(left_expr)), right_expr_(std::move(right_expr))
{}

RC HashJoinPhysicalOperator::open(Trx *trx)
{
  if (children_.size() != 2) {
    LOG_WARN("hash join operator should have 2 children");
    return RC::INTERNAL;
  }

  left_  = children_[0].get();
  right_ = children_[1].get();
  trx_   = trx;

  RC rc = build_right_table(trx);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  rc = left_->open(trx_);
  if (rc == RC::SUCCESS) {
    left_opened_ = true;
  }
  return rc;
}

RC HashJoinPhysicalOperator::build_right_table(Trx *trx)
{
  right_tuples_.clear();
  hash_table_.clear();
  matched_right_indexes_ = nullptr;
  matched_pos_           = 0;
  left_tuple_            = nullptr;
  probe_expr_            = left_expr_.get();
  build_expr_            = right_expr_.get();

  RC rc = right_->open(trx);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  right_opened_ = true;

  bool oriented = false;
  while ((rc = right_->next()) == RC::SUCCESS) {
    Tuple *tuple = right_->current_tuple();
    if (!oriented) {
      rc = orient_key_exprs(*tuple);
      if (rc != RC::SUCCESS) {
        return rc;
      }
      oriented = true;
    }

    Value key;
    rc = build_expr_->get_value(*tuple, key);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    if (key.is_null()) {
      continue;
    }

    ValueListTuple value_tuple;
    rc = ValueListTuple::make(*tuple, value_tuple);
    if (rc != RC::SUCCESS) {
      return rc;
    }

    const int index = static_cast<int>(right_tuples_.size());
    right_tuples_.push_back(std::move(value_tuple));
    hash_table_[hash_key(key)].push_back(index);
  }

  if (rc != RC::RECORD_EOF) {
    return rc;
  }

  rc = right_->close();
  right_opened_ = false;
  return rc;
}

RC HashJoinPhysicalOperator::orient_key_exprs(const Tuple &right_tuple)
{
  Value value;
  RC    rc = right_expr_->get_value(right_tuple, value);
  if (rc == RC::SUCCESS) {
    build_expr_ = right_expr_.get();
    probe_expr_ = left_expr_.get();
    return RC::SUCCESS;
  }

  rc = left_expr_->get_value(right_tuple, value);
  if (rc == RC::SUCCESS) {
    build_expr_ = left_expr_.get();
    probe_expr_ = right_expr_.get();
    return RC::SUCCESS;
  }

  LOG_WARN("failed to orient hash join key expressions");
  return RC::INVALID_ARGUMENT;
}

string HashJoinPhysicalOperator::hash_key(const Value &value) const
{
  return std::to_string(static_cast<int>(value.attr_type())) + ":" + value.to_string();
}

RC HashJoinPhysicalOperator::next()
{
  while (true) {
    if (matched_right_indexes_ != nullptr && matched_pos_ < matched_right_indexes_->size()) {
      const int right_index = (*matched_right_indexes_)[matched_pos_++];
      joined_tuple_.set_left(left_tuple_);
      joined_tuple_.set_right(&right_tuples_[right_index]);

      bool matched = false;
      RC rc = predicate_satisfied(matched);
      if (rc != RC::SUCCESS) {
        return rc;
      }
      if (matched) {
        return RC::SUCCESS;
      }
      continue;
    }

    RC rc = left_->next();
    if (rc != RC::SUCCESS) {
      return rc;
    }

    left_tuple_ = left_->current_tuple();
    Value key;
    rc = probe_expr_->get_value(*left_tuple_, key);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    if (key.is_null()) {
      matched_right_indexes_ = nullptr;
      matched_pos_ = 0;
      continue;
    }

    auto iter = hash_table_.find(hash_key(key));
    if (iter == hash_table_.end()) {
      matched_right_indexes_ = nullptr;
      matched_pos_ = 0;
      continue;
    }

    matched_right_indexes_ = &iter->second;
    matched_pos_ = 0;
  }
}

RC HashJoinPhysicalOperator::predicate_satisfied(bool &matched)
{
  matched = true;
  for (auto &predicate : predicates_) {
    Value value;
    RC rc = predicate->get_value(joined_tuple_, value);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    if (!value.get_boolean()) {
      matched = false;
      return RC::SUCCESS;
    }
  }
  return RC::SUCCESS;
}

RC HashJoinPhysicalOperator::close()
{
  RC rc = RC::SUCCESS;
  if (left_opened_) {
    rc = left_->close();
    left_opened_ = false;
  }
  if (right_opened_) {
    RC right_rc = right_->close();
    right_opened_ = false;
    if (rc == RC::SUCCESS) {
      rc = right_rc;
    }
  }

  right_tuples_.clear();
  hash_table_.clear();
  matched_right_indexes_ = nullptr;
  matched_pos_ = 0;
  left_tuple_ = nullptr;
  return rc;
}
