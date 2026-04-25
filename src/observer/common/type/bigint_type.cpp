/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/lang/comparator.h"
#include "common/lang/sstream.h"
#include "common/log/log.h"
#include "common/type/bigint_type.h"
#include "common/value.h"
#include "storage/common/column.h"
#include <cstdint>
#include <limits>

int BigintType::compare(const Value &left, const Value &right) const
{
  ASSERT(left.attr_type() == AttrType::BIGINTS, "left type is not bigint");
  ASSERT(right.attr_type() == AttrType::BIGINTS || right.attr_type() == AttrType::INTS ||
         right.attr_type() == AttrType::FLOATS, "right type is not numeric");

  if (right.attr_type() == AttrType::BIGINTS) {
    int64_t left_val  = left.get_bigint();
    int64_t right_val = right.get_bigint();
    return (left_val < right_val) ? -1 : (left_val > right_val ? 1 : 0);
  } else if (right.attr_type() == AttrType::INTS) {
    int64_t left_val  = left.get_bigint();
    int64_t right_val = static_cast<int64_t>(right.get_int());
    return (left_val < right_val) ? -1 : (left_val > right_val ? 1 : 0);
  } else if (right.attr_type() == AttrType::FLOATS) {
    double left_val  = static_cast<double>(left.get_bigint());
    double right_val = static_cast<double>(right.get_float());
    return (left_val < right_val) ? -1 : (left_val > right_val ? 1 : 0);
  }
  return INT32_MAX;
}

int BigintType::compare(const Column &left, const Column &right, int left_idx, int right_idx) const
{
  ASSERT(left.attr_type() == AttrType::BIGINTS, "left type is not bigint");
  ASSERT(right.attr_type() == AttrType::BIGINTS, "right type is not bigint");
  int64_t left_val  = ((int64_t*)left.data())[left_idx];
  int64_t right_val = ((int64_t*)right.data())[right_idx];
  return (left_val < right_val) ? -1 : (left_val > right_val ? 1 : 0);
}

RC BigintType::cast_to(const Value &val, AttrType type, Value &result) const
{
  switch (type) {
    case AttrType::BIGINTS: {
      result = val;
      return RC::SUCCESS;
    }
    case AttrType::INTS: {
      int64_t bigint_val = val.get_bigint();
      if (bigint_val > INT32_MAX || bigint_val < INT32_MIN) {
        LOG_WARN("bigint value overflow when casting to int: %ld", bigint_val);
        return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
      result.set_int(static_cast<int32_t>(bigint_val));
      return RC::SUCCESS;
    }
    case AttrType::FLOATS: {
      double double_value = static_cast<double>(val.get_bigint());
      result.set_float(static_cast<float>(double_value));
      return RC::SUCCESS;
    }
    case AttrType::CHARS: {
      std::string s = std::to_string(val.get_bigint());
      result.set_string(s.c_str());
      return RC::SUCCESS;
    }
    case AttrType::TEXTS: {
      std::string s = std::to_string(val.get_bigint());
      result.set_string(s.c_str());
      result.set_type(AttrType::TEXTS);
      return RC::SUCCESS;
    }
    default: {
      LOG_WARN("unsupported cast from BIGINTS to type=%d", static_cast<int>(type));
      return RC::SCHEMA_FIELD_TYPE_MISMATCH;
    }
  }
}

RC BigintType::add(const Value &left, const Value &right, Value &result) const
{
  int64_t left_val  = left.get_bigint();
  int64_t right_val = right.get_bigint();

  // 检查溢出
  if ((right_val > 0 && left_val > INT64_MAX - right_val) ||
      (right_val < 0 && left_val < INT64_MIN - right_val)) {
    LOG_WARN("bigint addition overflow");
    result.set_null();
    return RC::SUCCESS;
  }

  result.set_bigint(left_val + right_val);
  return RC::SUCCESS;
}

RC BigintType::subtract(const Value &left, const Value &right, Value &result) const
{
  int64_t left_val  = left.get_bigint();
  int64_t right_val = right.get_bigint();

  // 检查溢出
  if ((right_val < 0 && left_val > INT64_MAX + right_val) ||
      (right_val > 0 && left_val < INT64_MIN + right_val)) {
    LOG_WARN("bigint subtraction overflow");
    result.set_null();
    return RC::SUCCESS;
  }

  result.set_bigint(left_val - right_val);
  return RC::SUCCESS;
}

RC BigintType::multiply(const Value &left, const Value &right, Value &result) const
{
  int64_t left_val  = left.get_bigint();
  int64_t right_val = right.get_bigint();

  // 检查溢出
  if (left_val != 0 && right_val != 0) {
    if ((left_val > 0 && right_val > 0 && left_val > INT64_MAX / right_val) ||
        (left_val > 0 && right_val < 0 && right_val < INT64_MIN / left_val) ||
        (left_val < 0 && right_val > 0 && left_val < INT64_MIN / right_val) ||
        (left_val < 0 && right_val < 0 && left_val < INT64_MAX / right_val)) {
      LOG_WARN("bigint multiplication overflow");
      result.set_null();
      return RC::SUCCESS;
    }
  }

  result.set_bigint(left_val * right_val);
  return RC::SUCCESS;
}

RC BigintType::divide(const Value &left, const Value &right, Value &result) const
{
  int64_t divisor = right.get_bigint();
  if (divisor == 0) {
    result.set_null();
  } else {
    int64_t left_val = left.get_bigint();
    // 检查特殊溢出情况: INT64_MIN / -1
    if (left_val == INT64_MIN && divisor == -1) {
      LOG_WARN("bigint division overflow");
      result.set_null();
      return RC::SUCCESS;
    }
    result.set_bigint(left_val / divisor);
  }
  return RC::SUCCESS;
}

RC BigintType::negative(const Value &val, Value &result) const
{
  int64_t value = val.get_bigint();
  if (value == INT64_MIN) {
    LOG_WARN("bigint negation overflow");
    result.set_null();
    return RC::SUCCESS;
  }
  result.set_bigint(-value);
  return RC::SUCCESS;
}

RC BigintType::set_value_from_str(Value &val, const string &data) const
{
  RC                rc = RC::SUCCESS;
  stringstream deserialize_stream;
  deserialize_stream.clear();
  deserialize_stream.str(data);
  int64_t bigint_value;
  deserialize_stream >> bigint_value;
  if (!deserialize_stream || !deserialize_stream.eof()) {
    rc = RC::SCHEMA_FIELD_TYPE_MISMATCH;
  } else {
    val.set_bigint(bigint_value);
  }
  return rc;
}

RC BigintType::to_string(const Value &val, string &result) const
{
  stringstream ss;
  ss << val.get_bigint();
  result = ss.str();
  return RC::SUCCESS;
}
