/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/union_physical_operator.h"

#include "common/log/log.h"
#include "sql/expr/tuple.h"
#include "storage/trx/trx.h"

UnionPhysicalOperator::UnionPhysicalOperator(bool distinct) : distinct_(distinct)
{
}

RC UnionPhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    LOG_WARN("union physical operator has no child");
    return RC::INVALID_ARGUMENT;
  }

  trx_ = trx;
  current_child_idx_ = 0;
  current_tuple_ = nullptr;
  seen_signatures_.clear();
  child_opened_.assign(children_.size(), false);

  return switch_to_child(0);
}

RC UnionPhysicalOperator::next()
{
  if (children_.empty()) {
    return RC::RECORD_EOF;
  }

  while (current_child_idx_ < children_.size()) {
    PhysicalOperator *child = children_[current_child_idx_].get();
    RC rc = child->next();
    if (rc == RC::SUCCESS) {
      Tuple *tuple = child->current_tuple();
      if (tuple == nullptr) {
        LOG_WARN("union child returned null tuple");
        return RC::INTERNAL;
      }

      if (distinct_) {
        std::string signature;
        rc = build_signature(*tuple, signature);
        if (OB_FAIL(rc)) {
          LOG_WARN("failed to build union signature. rc=%s", strrc(rc));
          return rc;
        }
        auto insert_result = seen_signatures_.insert(signature);
        if (!insert_result.second) {
          // duplicate row, fetch next
          continue;
        }
      }

      current_tuple_ = tuple;
      return RC::SUCCESS;
    }

    if (rc == RC::RECORD_EOF) {
      if (child_opened_[current_child_idx_]) {
        child->close();
        child_opened_[current_child_idx_] = false;
      }
      ++current_child_idx_;
      if (current_child_idx_ < children_.size()) {
        RC switch_rc = switch_to_child(current_child_idx_);
        if (OB_FAIL(switch_rc)) {
          return switch_rc;
        }
        continue;
      }
      current_tuple_ = nullptr;
      return RC::RECORD_EOF;
    }

    return rc;
  }

  current_tuple_ = nullptr;
  return RC::RECORD_EOF;
}

RC UnionPhysicalOperator::close()
{
  RC rc = RC::SUCCESS;
  for (size_t i = 0; i < children_.size(); ++i) {
    if (i < child_opened_.size() && child_opened_[i]) {
      RC child_rc = children_[i]->close();
      if (rc == RC::SUCCESS && OB_FAIL(child_rc)) {
        rc = child_rc;
      }
      child_opened_[i] = false;
    }
  }
  seen_signatures_.clear();
  current_tuple_ = nullptr;
  current_child_idx_ = children_.size();
  trx_ = nullptr;
  return rc;
}

RC UnionPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  if (children_.empty()) {
    return RC::SUCCESS;
  }
  return children_[0]->tuple_schema(schema);
}

RC UnionPhysicalOperator::switch_to_child(size_t index)
{
  if (index >= children_.size()) {
    current_child_idx_ = children_.size();
    return RC::RECORD_EOF;
  }

  PhysicalOperator *child = children_[index].get();
  RC rc = child->open(trx_);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open child %zu of union operator. rc=%s", index, strrc(rc));
    return rc;
  }
  child_opened_[index] = true;
  current_child_idx_ = index;
  current_tuple_ = nullptr;
  return RC::SUCCESS;
}

RC UnionPhysicalOperator::build_signature(const Tuple &tuple, std::string &signature) const
{
  signature.clear();
  const int cell_num = tuple.cell_num();
  for (int i = 0; i < cell_num; i++) {
    Value cell;
    RC rc = tuple.cell_at(i, cell);
    if (OB_FAIL(rc)) {
      return rc;
    }
    signature.append(cell.to_string());
    signature.push_back('\x1f');
  }
  return RC::SUCCESS;
}
