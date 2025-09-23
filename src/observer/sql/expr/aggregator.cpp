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
// Created by Wangyunlai on 2024/05/29.
//

#include "sql/expr/aggregator.h"
#include "common/log/log.h"

RC SumAggregator::accumulate(const Value &value)
{
  if (value.is_null()) {
    return RC::SUCCESS; // 忽略 NULL
  }
  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    return RC::SUCCESS;
  }
  
  ASSERT(value.attr_type() == value_.attr_type(), "type mismatch. value type: %s, value_.type: %s", 
        attr_type_to_string(value.attr_type()), attr_type_to_string(value_.attr_type()));
  
  Value::add(value, value_, value_);
  return RC::SUCCESS;
}

RC SumAggregator::evaluate(Value& result)
{
  result = value_;
  return RC::SUCCESS;
}

RC CountAggregator::accumulate(const Value &value)
{
  // COUNT(expr) 忽略 NULL；COUNT(*) 在 binder 被改造成常量 1
  if (!value.is_null()) {
    if (value_.attr_type() != AttrType::INTS) {
      value_.set_int(0);
    }
    int cnt = value_.get_int();
    value_.set_int(cnt + 1);
  }
  return RC::SUCCESS;
}

RC CountAggregator::evaluate(Value &result)
{
  result = value_;
  if (result.attr_type() != AttrType::INTS) {
    result.set_int(0);
  }
  return RC::SUCCESS;
}

RC AvgAggregator::accumulate(const Value &value)
{
  if (value.is_null()) {
    return RC::SUCCESS; // 忽略 NULL
  }

  // 初始化 sum_ 类型
  if (sum_.attr_type() == AttrType::UNDEFINED) {
    // AVG 需要浮点结果，但可由整型或浮点累加
    if (value.attr_type() == AttrType::INTS) {
      sum_.set_int(0);
    } else if (value.attr_type() == AttrType::FLOATS) {
      sum_.set_float(0.0f);
    } else {
      return RC::INVALID_ARGUMENT;
    }
  }

  // 根据类型累加
  if (value.attr_type() == AttrType::INTS && sum_.attr_type() == AttrType::INTS) {
    int s = sum_.get_int();
    sum_.set_int(s + value.get_int());
  } else if (value.attr_type() == AttrType::FLOATS && sum_.attr_type() == AttrType::FLOATS) {
    float s = sum_.get_float();
    sum_.set_float(s + value.get_float());
  } else if (value.attr_type() == AttrType::INTS && sum_.attr_type() == AttrType::FLOATS) {
    float s = sum_.get_float();
    sum_.set_float(s + static_cast<float>(value.get_int()));
  } else if (value.attr_type() == AttrType::FLOATS && sum_.attr_type() == AttrType::INTS) {
    // 将整型 sum 升级为浮点，以避免精度损失
    float s = static_cast<float>(sum_.get_int());
    sum_.set_float(s + value.get_float());
  } else {
    return RC::INVALID_ARGUMENT;
  }

  count_++;
  return RC::SUCCESS;
}

RC AvgAggregator::evaluate(Value &result)
{
  if (count_ == 0) {
    result.set_null();
    return RC::SUCCESS;
  }

  float s = 0.0f;
  if (sum_.attr_type() == AttrType::INTS) {
    s = static_cast<float>(sum_.get_int());
  } else if (sum_.attr_type() == AttrType::FLOATS) {
    s = sum_.get_float();
  }
  result.set_float(s / static_cast<float>(count_));
  return RC::SUCCESS;
}

RC MaxAggregator::accumulate(const Value &value)
{
  if (value.is_null()) {
    return RC::SUCCESS; // 忽略 NULL
  }
  if (!has_value_) {
    value_ = value;
    has_value_ = true;
    return RC::SUCCESS;
  }
  int cmp = 0;
  Value::compare(value_, value, cmp);
  if (cmp < 0) { // value_ < value
    value_ = value;
  }
  return RC::SUCCESS;
}

RC MaxAggregator::evaluate(Value &result)
{
  if (!has_value_) {
    result.set_null();
  } else {
    result = value_;
  }
  return RC::SUCCESS;
}

RC MinAggregator::accumulate(const Value &value)
{
  if (value.is_null()) {
    return RC::SUCCESS; // 忽略 NULL
  }
  if (!has_value_) {
    value_ = value;
    has_value_ = true;
    return RC::SUCCESS;
  }
  int cmp = 0;
  Value::compare(value_, value, cmp);
  if (cmp > 0) { // value_ > value
    value_ = value;
  }
  return RC::SUCCESS;
}

RC MinAggregator::evaluate(Value &result)
{
  if (!has_value_) {
    result.set_null();
  } else {
    result = value_;
  }
  return RC::SUCCESS;
}
