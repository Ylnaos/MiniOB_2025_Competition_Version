/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/type/null_type.h"
#include "common/value.h"

int NullType::compare(const Value &left, const Value &right) const
{
  // NULL values are considered equal to each other in sorting
  if (left.is_null() && right.is_null()) {
    return 0;
  }
  // NULL is less than any non-NULL value
  if (left.is_null()) {
    return -1;
  }
  return 1;
}

RC NullType::add(const Value &left, const Value &right, Value &result) const
{
  result.set_null();
  return RC::SUCCESS;
}

RC NullType::subtract(const Value &left, const Value &right, Value &result) const
{
  result.set_null();
  return RC::SUCCESS;
}

RC NullType::multiply(const Value &left, const Value &right, Value &result) const
{
  result.set_null();
  return RC::SUCCESS;
}

RC NullType::divide(const Value &left, const Value &right, Value &result) const
{
  result.set_null();
  return RC::SUCCESS;
}

RC NullType::negative(const Value &val, Value &result) const
{
  result.set_null();
  return RC::SUCCESS;
}

RC NullType::cast_to(const Value &val, AttrType type, Value &result) const
{
  if (type == AttrType::NULLS) {
    result.set_null();
    return RC::SUCCESS;
  }
  // Cannot cast NULL to other types
  return RC::INVALID_ARGUMENT;
}

RC NullType::set_value_from_str(Value &val, const string &data) const
{
  if (data == "NULL" || data == "null") {
    val.set_null();
    return RC::SUCCESS;
  }
  return RC::INVALID_ARGUMENT;
}

RC NullType::to_string(const Value &val, string &result) const
{
  result = "NULL";
  return RC::SUCCESS;
}