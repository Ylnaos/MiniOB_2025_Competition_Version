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
  ASSERT(left.attr_type() == AttrType::CHARS, "left type is not char");

  // 字符串与数字比较：走数值语义（与 MySQL 行为保持一致）
  if (right.attr_type() == AttrType::INTS || right.attr_type() == AttrType::FLOATS ||
      right.attr_type() == AttrType::BOOLEANS) {
    float lv = left.get_float();
    float rv = right.get_float();
    return common::compare_float((void *)&lv, (void *)&rv);
  }

  // 字符串与字符串比较：按字典序比较（支持 CHARS <-> CHARS/TEXTS）
  if (right.attr_type() == AttrType::CHARS || right.attr_type() == AttrType::TEXTS) {
    // 左侧为 CHARS，直接使用其指针与长度；右侧统一取 string_t 获取精确长度
    auto r = right.get_string_t();
    return common::compare_string(
        (void *)left.value_.pointer_value_, left.length_, (void *)r.data(), static_cast<int>(r.size()));
  }

  // 其他类型暂不支持
  LOG_WARN("unsupported compare between CHARS and type=%d", right.attr_type());
  return INT32_MAX;
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
    case AttrType::VECTORS: {
      // 从字符串解析成向量字节序列，形如 "[1,2,3]"
      std::string s = val.get_string();
      // 去除首尾空白
      auto l = s.find_first_not_of(" \t\r\n");
      auto r = s.find_last_not_of(" \t\r\n");
      if (l == std::string::npos) return RC::INVALID_ARGUMENT;
      s = s.substr(l, r - l + 1);
      if (s.size() < 2 || s.front() != '[' || s.back() != ']') {
        return RC::INVALID_ARGUMENT;
      }

      // 检查向量字符串内部是否包含空格（拒绝带空格的格式如 '[1, 2, 3]'）
      for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == ' ' || s[i] == '\t') {
          // 包含空格，拒绝解析
          return RC::INVALID_ARGUMENT;
        }
      }

      // 严格解析（不跳过空格）
      std::vector<float> elems;
      std::string inner = s.substr(1, s.size() - 2);
      if (inner.empty()) {
        // 空向量 "[]"
        result.set_type(AttrType::VECTORS);
        result.set_data(nullptr, 0);
        return RC::SUCCESS;
      }

      // 按逗号分割并解析，不trim（因为已经验证无空格）
      const char *p = inner.c_str();
      while (*p) {
        char *endp = nullptr;
        float v = static_cast<float>(strtod(p, &endp));
        if (endp == p) {
          // 解析失败
          return RC::INVALID_ARGUMENT;
        }
        elems.push_back(v);
        p = endp;
        if (*p == ',') {
          p++;
        } else if (*p != '\0') {
          // 非逗号非结束符，格式错误
          return RC::INVALID_ARGUMENT;
        }
      }

      // 组装结果
      result.set_type(AttrType::VECTORS);
      result.set_data(reinterpret_cast<const char *>(elems.data()), static_cast<int>(elems.size() * sizeof(float)));
      return RC::SUCCESS;
    }
    case AttrType::TEXTS: {
      result = val;
      result.set_type(AttrType::TEXTS);
      return RC::SUCCESS;
    }
    case AttrType::INTS:
    {
      // 宽松解析：与比较语义保持一致，无法解析时按前缀数字转换
      result.set_int(val.get_int());
      return RC::SUCCESS;
    }
    case AttrType::FLOATS:
    {
      // 同上，使用 Value::get_float 处理非数字时回退为 0.0
      result.set_float(val.get_float());
      return RC::SUCCESS;
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
