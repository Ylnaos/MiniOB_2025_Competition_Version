/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/type/boolean_type.h"
#include "common/value.h"
#include "storage/common/column.h"
#include "common/lang/string.h"

int BooleanType::compare(const Value &left, const Value &right) const
{
  const bool l = left.get_boolean();
  const bool r = right.get_boolean();
  if (l == r) {
    return 0;
  }
  return l ? 1 : -1;
}

int BooleanType::compare(const Column &left, const Column &right, int left_idx, int right_idx) const
{
  const bool l = left.get_value(left_idx).get_boolean();
  const bool r = right.get_value(right_idx).get_boolean();
  if (l == r) {
    return 0;
  }
  return l ? 1 : -1;
}

RC BooleanType::cast_to(const Value &val, AttrType type, Value &result) const
{
  const bool b = val.get_boolean();
  switch (type) {
    case AttrType::BOOLEANS: {
      result.set_boolean(b);
      return RC::SUCCESS;
    }
    case AttrType::INTS: {
      result.set_int(b ? 1 : 0);
      return RC::SUCCESS;
    }
    case AttrType::FLOATS: {
      result.set_float(b ? 1.0f : 0.0f);
      return RC::SUCCESS;
    }
    case AttrType::CHARS:
    case AttrType::TEXTS: {
      result.set_string(b ? "1" : "0");
      return RC::SUCCESS;
    }
    case AttrType::NULLS: {
      result.set_null();
      return RC::SUCCESS;
    }
    default: {
      return RC::UNSUPPORTED;
    }
  }
}

int BooleanType::cast_cost(AttrType type)
{
  switch (type) {
    case AttrType::BOOLEANS:
      return 0;
    case AttrType::INTS:
    case AttrType::FLOATS:
      return 1;
    case AttrType::CHARS:
    case AttrType::TEXTS:
      return 2;
    default:
      return INT32_MAX;
  }
}

RC BooleanType::to_string(const Value &val, string &result) const
{
  result = val.get_boolean() ? "TRUE" : "FALSE";
  return RC::SUCCESS;
}

RC BooleanType::set_value_from_str(Value &val, const string &data) const
{
  string trimmed = data;
  common::str_to_upper(trimmed);
  if (trimmed == "TRUE" || trimmed == "1") {
    val.set_boolean(true);
    return RC::SUCCESS;
  }
  if (trimmed == "FALSE" || trimmed == "0") {
    val.set_boolean(false);
    return RC::SUCCESS;
  }
  return RC::INVALID_ARGUMENT;
}
