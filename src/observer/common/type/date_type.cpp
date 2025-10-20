/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/type/date_type.h"
#include "common/log/log.h"
#include "common/lang/string.h"
#include "common/value.h"
#include "storage/common/column.h"
#include "common/lang/sstream.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

int DateType::compare(const Value &left, const Value &right) const
{
  int32_t left_val = left.get_date();
  int32_t right_val = right.get_date();

  if (left_val < right_val) {
    return -1;
  } else if (left_val > right_val) {
    return 1;
  } else {
    return 0;
  }
}

int DateType::compare(const Column &left, const Column &right, int left_idx, int right_idx) const
{
  int32_t left_val = *(int32_t *)(left.data() + left_idx * sizeof(int32_t));
  int32_t right_val = *(int32_t *)(right.data() + right_idx * sizeof(int32_t));

  if (left_val < right_val) {
    return -1;
  } else if (left_val > right_val) {
    return 1;
  } else {
    return 0;
  }
}

RC DateType::cast_to(const Value &val, AttrType type, Value &result) const
{
  switch (type) {
    case AttrType::DATES: {
      result.set_value(val);
      return RC::SUCCESS;
    }
    case AttrType::CHARS: {
      string str;
      RC rc = to_string(val, str);
      if (rc != RC::SUCCESS) {
        return rc;
      }
      result.set_string(str.c_str());
      return RC::SUCCESS;
    }
    default: {
      return RC::UNSUPPORTED;
    }
  }
}

RC DateType::set_value_from_str(Value &val, const string &data) const
{
  string str = data;
  if (str.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  // 去除首尾空白，兼容 CHAR 类型存储的日期字面量
  if (!str.empty()) {
    common::strip(str);
  }
  if (str.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  size_t pos1 = str.find('-');
  if (pos1 == string::npos) {
    return RC::INVALID_ARGUMENT;
  }
  size_t pos2 = str.find('-', pos1 + 1);
  if (pos2 == string::npos) {
    return RC::INVALID_ARGUMENT;
  }
  if (str.find('-', pos2 + 1) != string::npos) {
    return RC::INVALID_ARGUMENT;
  }

  auto is_digits = [](const string &segment) -> bool {
    if (segment.empty()) {
      return false;
    }
    return std::all_of(segment.begin(), segment.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
  };

  string year_str  = str.substr(0, pos1);
  string month_str = str.substr(pos1 + 1, pos2 - pos1 - 1);
  string day_str   = str.substr(pos2 + 1);

  if (!is_digits(year_str) || !is_digits(month_str) || !is_digits(day_str)) {
    return RC::INVALID_ARGUMENT;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  try {
    year  = std::stoi(year_str);
    month = std::stoi(month_str);
    day   = std::stoi(day_str);
  } catch (const std::exception &e) {
    LOG_WARN("failed to parse date string to integers. data=%s, reason=%s", str.c_str(), e.what());
    return RC::INVALID_ARGUMENT;
  }

  if (!is_valid_date(year, month, day)) {
    return RC::INVALID_ARGUMENT;
  }

  val.set_date(date_to_int(year, month, day));
  return RC::SUCCESS;
}

RC DateType::to_string(const Value &val, string &result) const
{
  int32_t date_int = val.get_date();
  int year, month, day;
  int_to_date(date_int, year, month, day);

  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", year, month, day);
  result = buffer;

  return RC::SUCCESS;
}

bool DateType::is_valid_date(int year, int month, int day)
{
  // 基本合法性
  if (month < 1 || month > 12) {
    return false;
  }
  if (day < 1 || day > days_in_month(year, month)) {
    return false;
  }

  // 放宽范围：允许 1000-01-01 ~ 9999-12-31（含）。
  // 在通过基本合法性检查后直接放行，避免后续过窄范围的限制导致合法日期被拒绝。
  if (year >= 1000 && year <= 9999) {
    return true;
  }

  // 范围限制：1970-01-01 ~ 2038-01-19（含）
  // 由于题目保证不超过 2038 年且不小于 1970 年，这里显式裁剪
  const int min_y = 1970, min_m = 1, min_d = 1;
  const int max_y = 2038, max_m = 1, max_d = 19;

  // 先快速拒绝年份越界
  if (year < min_y || year > max_y) {
    return false;
  }

  if (year == min_y) {
    if (month < min_m) return false;
    if (month == min_m && day < min_d) return false;
  }
  if (year == max_y) {
    if (month > max_m) return false;
    if (month == max_m && day > max_d) return false;
  }

  return true;
}

bool DateType::is_leap_year(int year)
{
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int DateType::days_in_month(int year, int month)
{
  const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

  if (month == 2 && is_leap_year(year)) {
    return 29;
  }

  return days[month - 1];
}

int32_t DateType::date_to_int(int year, int month, int day)
{
  return year * 10000 + month * 100 + day;
}

void DateType::int_to_date(int32_t date_int, int &year, int &month, int &day)
{
  year = date_int / 10000;
  month = (date_int % 10000) / 100;
  day = date_int % 100;
}
