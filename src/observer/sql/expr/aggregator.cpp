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

// 全局计数器，用于追踪聚合器操作
static int sum_aggregator_accumulate_count = 0;
static int sum_aggregator_evaluate_count = 0;
static int sum_aggregator_total_calls = 0;

static int count_aggregator_accumulate_count = 0;
static int count_aggregator_evaluate_count = 0;
static int count_aggregator_total_calls = 0;

static int avg_aggregator_accumulate_count = 0;
static int avg_aggregator_evaluate_count = 0;
static int avg_aggregator_total_calls = 0;

RC SumAggregator::accumulate(const Value &value)
{
  sum_aggregator_accumulate_count++;
  sum_aggregator_total_calls++;

  // 强制输出每个SUM累加操作
  printf("=== FORCE OUTPUT: SumAggregator::accumulate[%d] 值=%s ===\n",
         sum_aggregator_accumulate_count, value.to_string().c_str());
  fflush(stdout);

  LOG_INFO("DATA_FLOW: SumAggregator::accumulate[%d] - 聚合器=%p, 输入值类型=%d, 输入值=%s, 当前值类型=%d, 当前值=%s",
            sum_aggregator_accumulate_count, this, value.attr_type(), value.to_string().c_str(),
            value_.attr_type(), value_.to_string().c_str());

  if (value.is_null()) {
    LOG_INFO("DATA_FLOW: SumAggregator忽略NULL值[%d] - 聚合器=%p", sum_aggregator_accumulate_count, this);
    return RC::SUCCESS; // 忽略 NULL
  }

  if (value_.attr_type() == AttrType::UNDEFINED) {
    value_ = value;
    LOG_INFO("DATA_FLOW: SumAggregator初始化[%d] - 聚合器=%p, 设置初始值=%s",
              sum_aggregator_accumulate_count, this, value_.to_string().c_str());
    return RC::SUCCESS;
  }

  ASSERT(value.attr_type() == value_.attr_type(), "type mismatch. value type: %s, value_.type: %s",
        attr_type_to_string(value.attr_type()), attr_type_to_string(value_.attr_type()));

  Value old_value = value_;
  Value::add(value, value_, value_);

  LOG_INFO("DATA_FLOW: SumAggregator累加完成[%d] - 聚合器=%p, 旧值=%s, 加数=%s, 新值=%s",
            sum_aggregator_accumulate_count, this, old_value.to_string().c_str(), value.to_string().c_str(), value_.to_string().c_str());

  return RC::SUCCESS;
}

RC SumAggregator::evaluate(Value& result)
{
  sum_aggregator_evaluate_count++;

  // 没有任何非 NULL 的输入时，SUM 应返回 NULL
  if (value_.attr_type() == AttrType::UNDEFINED) {
    result.set_null();
    LOG_INFO("DATA_FLOW: SumAggregator::evaluate[%d] - 聚合器=%p, 无非空输入，返回NULL, 总调用=%d",
              sum_aggregator_evaluate_count, this, sum_aggregator_total_calls);
    return RC::SUCCESS;
  }

  result = value_;
  LOG_INFO("DATA_FLOW: SumAggregator::evaluate[%d] - 聚合器=%p, 返回结果=%s, 总调用=%d",
            sum_aggregator_evaluate_count, this, result.to_string().c_str(), sum_aggregator_total_calls);
  return RC::SUCCESS;
}

RC CountAggregator::accumulate(const Value &value)
{
  count_aggregator_accumulate_count++;
  count_aggregator_total_calls++;

  // 强制输出每个COUNT累加操作
  printf("=== FORCE OUTPUT: CountAggregator::accumulate[%d] 值=%s ===\n",
         count_aggregator_accumulate_count, value.to_string().c_str());
  fflush(stdout);

  // COUNT(expr) 忽略 NULL；COUNT(*) 在 binder 被改造成常量 1
  LOG_INFO("DATA_FLOW: CountAggregator::accumulate[%d] - 聚合器=%p, 输入值类型=%d, 输入值=%s, 当前计数=%d",
            count_aggregator_accumulate_count, this, value.attr_type(), value.to_string().c_str(),
            value_.attr_type() == AttrType::INTS ? value_.get_int() : 0);

  if (!value.is_null()) {
    if (value_.attr_type() != AttrType::INTS) {
      value_.set_int(0);
      LOG_INFO("DATA_FLOW: CountAggregator初始化计数[%d] - 聚合器=%p, 设置为0", count_aggregator_accumulate_count, this);
    }
    int cnt = value_.get_int();
    value_.set_int(cnt + 1);
    LOG_INFO("DATA_FLOW: CountAggregator计数增加[%d] - 聚合器=%p, 旧计数=%d, 新计数=%d",
              count_aggregator_accumulate_count, this, cnt, cnt + 1);
  } else {
    LOG_INFO("DATA_FLOW: CountAggregator忽略NULL值[%d] - 聚合器=%p", count_aggregator_accumulate_count, this);
  }

  return RC::SUCCESS;
}

RC CountAggregator::evaluate(Value &result)
{
  count_aggregator_evaluate_count++;

  result = value_;
  if (result.attr_type() != AttrType::INTS) {
    result.set_int(0);
    LOG_INFO("DATA_FLOW: CountAggregator::evaluate[%d] - 聚合器=%p, 结果类型错误，修正为0, 总调用=%d",
              count_aggregator_evaluate_count, this, count_aggregator_total_calls);
  }

  LOG_INFO("DATA_FLOW: CountAggregator::evaluate[%d] - 聚合器=%p, 返回计数=%d, 总调用=%d",
            count_aggregator_evaluate_count, this, result.get_int(), count_aggregator_total_calls);
  return RC::SUCCESS;
}

RC AvgAggregator::accumulate(const Value &value)
{
  avg_aggregator_accumulate_count++;
  avg_aggregator_total_calls++;

  LOG_INFO("DATA_FLOW: AvgAggregator::accumulate[%d] - 聚合器=%p, 输入值类型=%d, 输入值=%s, 当前和类型=%d, 当前和=%s, 当前计数=%d",
            avg_aggregator_accumulate_count, this, value.attr_type(), value.to_string().c_str(),
            sum_.attr_type(), sum_.to_string().c_str(), count_);

  if (value.is_null()) {
    LOG_DEBUG("DEBUG_LOG: AvgAggregator忽略NULL值 - 聚合器=%p", this);
    return RC::SUCCESS; // 忽略 NULL
  }

  // 初始化 sum_ 类型
  if (sum_.attr_type() == AttrType::UNDEFINED) {
    // AVG 需要浮点结果，但可由整型或浮点累加
    if (value.attr_type() == AttrType::INTS) {
      sum_.set_int(0);
      LOG_DEBUG("DEBUG_LOG: AvgAggregator初始化和 - 聚合器=%p, 使用INT类型，初始值=0", this);
    } else if (value.attr_type() == AttrType::FLOATS) {
      sum_.set_float(0.0f);
      LOG_DEBUG("DEBUG_LOG: AvgAggregator初始化和 - 聚合器=%p, 使用FLOAT类型，初始值=0.0", this);
    } else {
      LOG_WARN("DEBUG_LOG: AvgAggregator初始化失败 - 聚合器=%p, 不支持的值类型=%d", this, value.attr_type());
      return RC::INVALID_ARGUMENT;
    }
  }

  // 根据类型累加
  if (value.attr_type() == AttrType::INTS && sum_.attr_type() == AttrType::INTS) {
    int s = sum_.get_int();
    int v = value.get_int();
    sum_.set_int(s + v);
    LOG_DEBUG("DEBUG_LOG: AvgAggregator INT累加 - 聚合器=%p, 旧和=%d, 加数=%d, 新和=%d", this, s, v, s + v);
  } else if (value.attr_type() == AttrType::FLOATS && sum_.attr_type() == AttrType::FLOATS) {
    float s = sum_.get_float();
    float v = value.get_float();
    sum_.set_float(s + v);
    LOG_DEBUG("DEBUG_LOG: AvgAggregator FLOAT累加 - 聚合器=%p, 旧和=%f, 加数=%f, 新和=%f", this, s, v, s + v);
  } else if (value.attr_type() == AttrType::INTS && sum_.attr_type() == AttrType::FLOATS) {
    float s = sum_.get_float();
    float v = static_cast<float>(value.get_int());
    sum_.set_float(s + v);
    LOG_DEBUG("DEBUG_LOG: AvgAggregator INT转FLOAT累加 - 聚合器=%p, 旧和=%f, 加数=%f, 新和=%f", this, s, v, s + v);
  } else if (value.attr_type() == AttrType::FLOATS && sum_.attr_type() == AttrType::INTS) {
    // 将整型 sum 升级为浮点，以避免精度损失
    float s = static_cast<float>(sum_.get_int());
    float v = value.get_float();
    sum_.set_float(s + v);
    LOG_DEBUG("DEBUG_LOG: AvgAggregator FLOAT升级累加 - 聚合器=%p, 旧和(升级)=%f, 加数=%f, 新和=%f", this, s, v, s + v);
  } else {
    LOG_WARN("DEBUG_LOG: AvgAggregator累加失败 - 聚合器=%p, 不兼容的类型组合: 值类型=%d, 和类型=%d",
             this, value.attr_type(), sum_.attr_type());
    return RC::INVALID_ARGUMENT;
  }

  count_++;
  LOG_DEBUG("DEBUG_LOG: AvgAggregator累加完成 - 聚合器=%p, 新计数=%d", this, count_);
  return RC::SUCCESS;
}

RC AvgAggregator::evaluate(Value &result)
{
  avg_aggregator_evaluate_count++;

  if (count_ == 0) {
    result.set_null();
    LOG_INFO("DATA_FLOW: AvgAggregator::evaluate[%d] - 聚合器=%p, 计数为0，返回NULL, 总调用=%d",
              avg_aggregator_evaluate_count, this, avg_aggregator_total_calls);
    return RC::SUCCESS;
  }

  float s = 0.0f;
  if (sum_.attr_type() == AttrType::INTS) {
    s = static_cast<float>(sum_.get_int());
    LOG_INFO("DATA_FLOW: AvgAggregator评估[%d] - 聚合器=%p, INT和转FLOAT: 和=%d -> %f",
              avg_aggregator_evaluate_count, this, sum_.get_int(), s);
  } else if (sum_.attr_type() == AttrType::FLOATS) {
    s = sum_.get_float();
    LOG_INFO("DATA_FLOW: AvgAggregator评估[%d] - 聚合器=%p, FLOAT和: 和=%f",
              avg_aggregator_evaluate_count, this, s);
  }

  result.set_float(s / static_cast<float>(count_));
  LOG_INFO("DATA_FLOW: AvgAggregator::evaluate[%d] - 聚合器=%p, 最终结果: 和=%f / 计数=%d = %f, 总调用=%d",
            avg_aggregator_evaluate_count, this, s, count_, s / static_cast<float>(count_), avg_aggregator_total_calls);

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
