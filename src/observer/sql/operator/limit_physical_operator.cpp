/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/limit_physical_operator.h"
#include "common/log/log.h"

RC LimitPhysicalOperator::open(Trx *trx)
{
  if (children_.empty()) {
    return RC::INTERNAL;
  }

  PhysicalOperator *child = children_[0].get();
  RC rc = child->open(trx);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  count_ = 0;
  return RC::SUCCESS;
}

RC LimitPhysicalOperator::next()
{
  if (limit_ >= 0 && count_ >= limit_) {
    return RC::RECORD_EOF;
  }

  PhysicalOperator *child = children_[0].get();
  RC rc = child->next();
  if (rc == RC::SUCCESS) {
    count_++;
  }
  return rc;
}

RC LimitPhysicalOperator::close()
{
  if (!children_.empty()) {
    children_[0]->close();
  }
  return RC::SUCCESS;
}

Tuple *LimitPhysicalOperator::current_tuple()
{
  return children_[0]->current_tuple();
}
