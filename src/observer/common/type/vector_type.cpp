/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/type/vector_type.h"
#include "common/value.h"
#include "common/lang/comparator.h"
#include "common/log/log.h"
#include <vector>
#include <functional>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace {
inline int dim_from_len(int len) { return len <= 0 ? 0 : (len / static_cast<int>(sizeof(float))); }

RC vector_elementwise(const Value &left, const Value &right, Value &result,
                      const std::function<float(float, float)> &op)
{
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return RC::INVALID_ARGUMENT;
  }
  const int l_len = left.length();
  const int r_len = right.length();
  if (l_len <= 0 || r_len <= 0 || (l_len % sizeof(float)) != 0 || (r_len % sizeof(float)) != 0) {
    result.set_null();
    return RC::SUCCESS;
  }
  if (l_len != r_len) {
    // 维度不一致：返回 NULL
    LOG_WARN("vector length mismatch: %d vs %d", l_len, r_len);
    result.set_null();
    return RC::SUCCESS;
  }

  const int dim = dim_from_len(l_len);
  const float *la = reinterpret_cast<const float *>(left.data());
  const float *ra = reinterpret_cast<const float *>(right.data());
  std::vector<float> out(static_cast<size_t>(dim));
  for (int i = 0; i < dim; ++i) {
    out[i] = op(la[i], ra[i]);
  }
  result.set_type(AttrType::VECTORS);
  result.set_data(reinterpret_cast<const char *>(out.data()), static_cast<int>(out.size() * sizeof(float)));
  return RC::SUCCESS;
}
} // namespace

int VectorType::compare(const Value &left, const Value &right) const
{
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return INT32_MAX;
  }
  const int l_len = left.length();
  const int r_len = right.length();
  const int l_dim = dim_from_len(l_len);
  const int r_dim = dim_from_len(r_len);
  const int dim   = std::min(l_dim, r_dim);
  const float *la = reinterpret_cast<const float *>(left.data());
  const float *ra = reinterpret_cast<const float *>(right.data());
  for (int i = 0; i < dim; ++i) {
    // 使用 compare_float 进行稳定比较
    int c = common::compare_float((void *)&la[i], (void *)&ra[i]);
    if (c != 0) return c;
  }
  // 前缀相等，用长度判定
  if (l_dim < r_dim) return -1;
  if (l_dim > r_dim) return 1;
  return 0;
}

RC VectorType::add(const Value &left, const Value &right, Value &result) const
{
  return vector_elementwise(left, right, result, [](float a, float b) { return a + b; });
}

RC VectorType::subtract(const Value &left, const Value &right, Value &result) const
{
  return vector_elementwise(left, right, result, [](float a, float b) { return a - b; });
}

RC VectorType::multiply(const Value &left, const Value &right, Value &result) const
{
  return vector_elementwise(left, right, result, [](float a, float b) { return a * b; });
}

RC VectorType::to_string(const Value &val, string &result) const
{
  if (val.attr_type() != AttrType::VECTORS) return RC::INVALID_ARGUMENT;
  const int len = val.length();
  if (len <= 0) { result = "[]"; return RC::SUCCESS; }
  const int dim = len / static_cast<int>(sizeof(float));
  const float *data = reinterpret_cast<const float *>(val.data());

  result = "[";
  for (int i = 0; i < dim; ++i) {
    if (i > 0) result += ",";
    float value = data[i];

    // 检查是否为整数值
    if (std::floor(value) == value && std::abs(value) < 1e9) {
      // 整数显示，不带小数点
      result += std::to_string(static_cast<int>(value));
    } else {
      // 小数显示，使用固定精度格式化
      char buf[64];
      snprintf(buf, sizeof(buf), "%.6f", value);
      std::string temp(buf);
      // 去掉尾部的0
      size_t dot_pos = temp.find('.');
      if (dot_pos != std::string::npos) {
        size_t last_non_zero = temp.find_last_not_of('0');
        if (last_non_zero >= dot_pos) {
          temp = temp.substr(0, last_non_zero + 1);
          // 如果最后一个字符是小数点，保留至少一位小数
          if (temp.back() == '.') {
            temp += "0";
          }
        }
      }
      result += temp;
    }
  }
  result += "]";
  return RC::SUCCESS;
}
