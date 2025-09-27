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
#include "common/log/log.h"
#include "common/type/char_type.h"
#include "common/type/data_type.h"
#include <cstdlib>
#include "common/value.h"

int CharType::compare(const Value &left, const Value &right) const
{
  ASSERT(left.attr_type() == AttrType::CHARS && right.attr_type() == AttrType::CHARS, "invalid type");
  return common::compare_string(
      (void *)left.value_.pointer_value_, left.length_, (void *)right.value_.pointer_value_, right.length_);
}

RC CharType::set_value_from_str(Value &val, const string &data) const
{
  val.set_string(data.c_str());
  return RC::SUCCESS;
}

RC CharType::cast_to(const Value &val, AttrType type, Value &result) const
{
  switch (type) {
    case AttrType::CHARS: {
      result = val;
      return RC::SUCCESS;
    }
    case AttrType::TEXTS: {
      result = val;
      result.set_type(AttrType::TEXTS);
      return RC::SUCCESS;
    }
    case AttrType::INTS: {
      // 允许将可解析的字符串转换为整数
      // 使用 Value::get_int 的语义做兜底解析，和比较路径保持一致
      // 无法解析时返回类型不匹配错误
      try {
        int v = std::stoi(val.get_string());
        result.set_int(v);
        return RC::SUCCESS;
      } catch (...) {
        return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
    }
    case AttrType::FLOATS: {
      // 同上，支持字符串到浮点
      try {
        float v = std::stof(val.get_string());
        result.set_float(v);
        return RC::SUCCESS;
      } catch (...) {
        return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }
    }
    case AttrType::DATES: {
      // 从字符串转换为日期类型
      return DataType::type_instance(AttrType::DATES)->set_value_from_str(result, val.get_string());
    }
    default: return RC::UNIMPLEMENTED;
  }
  return RC::SUCCESS;
}

int CharType::cast_cost(AttrType type)
{
  if (type == AttrType::CHARS) {
    return 0;
  }
  if (type == AttrType::INTS || type == AttrType::FLOATS) {
    // 支持字符串到数值的隐式转换，用于比较与赋值
    return 1;
  }
  if (type == AttrType::DATES) {
    // 支持从字符串到日期的隐式转换
    return 1;
  }
  return INT32_MAX;
}

RC CharType::to_string(const Value &val, string &result) const
{
  stringstream ss;
  ss << val.value_.pointer_value_;
  result = ss.str();
  return RC::SUCCESS;
}
