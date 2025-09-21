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
// Created by WangYunlai on 2022/12/26.
//

#include "sql/operator/update_logical_operator.h"

UpdateLogicalOperator::UpdateLogicalOperator(Table *table, const vector<const FieldMeta *> &field_metas,
                                             const vector<Value> &values,
                                             const vector<unique_ptr<Expression>> &value_expressions)
    : table_(table), field_metas_(field_metas), values_(values)
{
  // 复制表达式
  for (const auto& expr : value_expressions) {
    if (expr) {
      value_expressions_.push_back(expr->copy());
    } else {
      value_expressions_.push_back(nullptr);
    }
  }
}