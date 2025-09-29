/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "common/type/data_type.h"

/**
 * @brief 日期类型
 * @ingroup DataType
 */
class DateType : public DataType
{
public:
  DateType() : DataType(AttrType::DATES) {}
  virtual ~DateType() {}

  int compare(const Value &left, const Value &right) const override;
  int compare(const Column &left, const Column &right, int left_idx, int right_idx) const override;

  RC cast_to(const Value &val, AttrType type, Value &result) const override;

  int cast_cost(const AttrType type) override
  {
    if (type == AttrType::DATES) {
      return 0;
    } else if (type == AttrType::CHARS) {
      // 将日期转为字符串的代价略高，便于比较时优先把字符串转为日期
      return 2;
    }
    return INT32_MAX;
  }

  RC set_value_from_str(Value &val, const string &data) const override;

  RC to_string(const Value &val, string &result) const override;

private:
  /**
   * @brief 验证日期是否合法
   * @param year 年份
   * @param month 月份 (1-12)
   * @param day 日期 (1-31)
   * @return true 合法, false 不合法
   */
  static bool is_valid_date(int year, int month, int day);

  /**
   * @brief 判断是否为闰年
   * @param year 年份
   * @return true 闰年, false 平年
   */
  static bool is_leap_year(int year);

  /**
   * @brief 获取某月的天数
   * @param year 年份
   * @param month 月份 (1-12)
   * @return 该月的天数
   */
  static int days_in_month(int year, int month);

  /**
   * @brief 将日期转换为内部存储格式 YYYYMMDD
   * @param year 年份
   * @param month 月份 (1-12)
   * @param day 日期 (1-31)
   * @return 内部存储格式的整数
   */
  static int32_t date_to_int(int year, int month, int day);

  /**
   * @brief 将内部存储格式转换为年月日
   * @param date_int 内部存储格式的整数
   * @param year 输出年份
   * @param month 输出月份
   * @param day 输出日期
   */
  static void int_to_date(int32_t date_int, int &year, int &month, int &day);
};
