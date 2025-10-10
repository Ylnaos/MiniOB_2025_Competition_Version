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
// Created by Claude Code for MiniOB Subquery Support
//

#include "sql/operator/subquery_physical_operator.h"
#include "common/log/log.h"

using namespace common;

void SubqueryPhysicalOperator::set_subquery(std::unique_ptr<PhysicalOperator> child_oper)
{
  children_.clear();
  children_.emplace_back(std::move(child_oper));
}

RC SubqueryPhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    LOG_WARN("subquery physical operator has no child");
    return RC::INTERNAL;
  }

  trx_ = trx;

  RC rc = children_[0]->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open subquery. rc=%s", strrc(rc));
    return rc;
  }

  return RC::SUCCESS;
}

RC SubqueryPhysicalOperator::next()
{
  if (children_.empty()) {
    return RC::INTERNAL;
  }

  return children_[0]->next();
}

RC SubqueryPhysicalOperator::close()
{
  if (!children_.empty()) {
    children_[0]->close();
  }
  trx_ = nullptr;
  return RC::SUCCESS;
}

Tuple *SubqueryPhysicalOperator::current_tuple()
{
  if (children_.empty()) {
    return nullptr;
  }

  return children_[0]->current_tuple();
}
