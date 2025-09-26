/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/type/text_type.h"
#include "common/type/char_type.h"
#include "common/lang/comparator.h"
#include "common/log/log.h"
#include "common/value.h"

int TextType::compare(const Value &left, const Value &right) const
{
  ASSERT(left.attr_type() == AttrType::TEXTS && right.attr_type() == AttrType::TEXTS, "invalid type");
  auto l = left.get_string_t();
  auto r = right.get_string_t();
  return common::compare_string((void *)l.data(), l.size(), (void *)r.data(), r.size());
}

RC TextType::set_value_from_str(Value &val, const string &data) const
{
  // TEXT 也按字符串保存
  val.set_string(data.c_str());
  // 修正类型为 TEXTS
  val.set_type(AttrType::TEXTS);
  return RC::SUCCESS;
}

RC TextType::cast_to(const Value &val, AttrType type, Value &result) const
{
  switch (type) {
    case AttrType::TEXTS: {
      result = val;
      result.set_type(AttrType::TEXTS);
      return RC::SUCCESS;
    }
    case AttrType::VECTORS: {
      // Parse TEXT into vector literal [..]
      return DataType::type_instance(AttrType::VECTORS)->set_value_from_str(result, val.get_string());
    }
    case AttrType::CHARS: {
      result = val;
      result.set_type(AttrType::CHARS);
      return RC::SUCCESS;
    }
    default: return RC::UNIMPLEMENTED;
  }
  return RC::SUCCESS;
}

int TextType::cast_cost(AttrType type)
{
  if (type == AttrType::TEXTS || type == AttrType::CHARS) {
    return 0;
  }
  return INT32_MAX;
}

RC TextType::to_string(const Value &val, string &result) const
{
  // 以 length 截断，避免必须依赖 '\0'
  if (val.length() <= 0) {
    result.clear();
    return RC::SUCCESS;
  }
  auto s = val.get_string_t();
  result.assign(s.data(), s.size());
  // 去掉尾部的连续'\0'，与 CHARS 行为保持尽量一致
  while (!result.empty() && result.back() == '\0') {
    result.pop_back();
  }
  return RC::SUCCESS;
}
