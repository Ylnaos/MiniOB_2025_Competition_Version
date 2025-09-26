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
#include "common/log/log.h"
#include <cctype>
#include <cmath>
#include <sstream>

namespace {
// 保留最多两位小数（四舍五入），并规整 -0.00 -> 0.0
inline float round2(float x)
{
  float y = std::round(x * 100.0f) / 100.0f;
  if (std::fabs(y) < 0.0005f) y = 0.0f;
  return y;
}

// 去掉前后空白
inline std::string trim(const std::string &s)
{
  size_t i = 0, j = s.size();
  while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) i++;
  while (j > i && std::isspace(static_cast<unsigned char>(s[j-1]))) j--;
  return s.substr(i, j-i);
}

// 从 Value 中获取向量数据。如果是字符类型，尝试解析，例如 "[1, 2.3, 3]"
RC get_vector_data(const Value &v, std::vector<float> &out)
{
  out.clear();
  if (v.attr_type() == AttrType::VECTORS) {
    int len = v.length();
    if (len % static_cast<int>(sizeof(float)) != 0) {
      LOG_WARN("invalid vector length: %d", len);
      return RC::INVALID_ARGUMENT;
    }
    int dim = len / static_cast<int>(sizeof(float));
    const float *ptr = reinterpret_cast<const float *>(v.data());
    out.assign(ptr, ptr + dim);
    return RC::SUCCESS;
  }
  if (v.attr_type() == AttrType::CHARS || v.attr_type() == AttrType::TEXTS) {
    std::string s = v.get_string();
    s = trim(s);
    if (s.size() < 2 || s.front() != '[' || s.back() != ']') {
      LOG_WARN("invalid vector literal: %s", s.c_str());
      return RC::INVALID_ARGUMENT;
    }
    std::string inner = s.substr(1, s.size()-2);
    std::stringstream ss(inner);
    std::string token;
    while (std::getline(ss, token, ',')) {
      token = trim(token);
      if (token.empty()) continue;
      try {
        float val = std::stof(token);
        out.push_back(round2(val));
      } catch (...) {
        LOG_WARN("invalid vector element: %s", token.c_str());
        return RC::INVALID_ARGUMENT;
      }
    }
    return RC::SUCCESS;
  }
  LOG_WARN("unsupported type for vector op: %d", static_cast<int>(v.attr_type()));
  return RC::INVALID_ARGUMENT;
}

// 设置 Value 为向量并拷贝数据
void set_vector_value(Value &val, const std::vector<float> &data)
{
  const int dim   = static_cast<int>(data.size());
  const int bytes = dim * static_cast<int>(sizeof(float));
  val.reset();
  val.set_type(AttrType::VECTORS);
  if (bytes > 0) {
    // set_data 会深拷贝并拥有内存（见 Value::set_data VECTORS 分支）
    val.set_data(reinterpret_cast<const char *>(data.data()), bytes);
  } else {
    // 避免在重载(char*, const char*)之间产生 nullptr 的二义性
    val.set_data(static_cast<const char *>(nullptr), 0);
  }
}

// 词典序比较
int lexicographical_compare(const std::vector<float> &a, const std::vector<float> &b)
{
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    if (a[i] < b[i]) return -1;
    if (a[i] > b[i]) return 1;
  }
  if (a.size() < b.size()) return -1;
  if (a.size() > b.size()) return 1;
  return 0;
}
}

int VectorType::compare(const Value &left, const Value &right) const
{
  std::vector<float> lv, rv;
  if (OB_FAIL(get_vector_data(left, lv))) {
    return INT32_MAX;
  }
  if (OB_FAIL(get_vector_data(right, rv))) {
    return INT32_MAX;
  }
  return lexicographical_compare(lv, rv);
}

RC VectorType::add(const Value &left, const Value &right, Value &result) const
{
  std::vector<float> lv, rv;
  RC rc = get_vector_data(left, lv);
  if (OB_FAIL(rc)) return rc;
  rc = get_vector_data(right, rv);
  if (OB_FAIL(rc)) return rc;
  if (lv.size() != rv.size()) {
    LOG_WARN("vector dim mismatch: %zu vs %zu", lv.size(), rv.size());
    return RC::INVALID_ARGUMENT;
  }
  std::vector<float> out(lv.size());
  for (size_t i = 0; i < lv.size(); ++i) out[i] = round2(lv[i] + rv[i]);
  set_vector_value(result, out);
  return RC::SUCCESS;
}

RC VectorType::subtract(const Value &left, const Value &right, Value &result) const
{
  std::vector<float> lv, rv;
  RC rc = get_vector_data(left, lv);
  if (OB_FAIL(rc)) return rc;
  rc = get_vector_data(right, rv);
  if (OB_FAIL(rc)) return rc;
  if (lv.size() != rv.size()) {
    LOG_WARN("vector dim mismatch: %zu vs %zu", lv.size(), rv.size());
    return RC::INVALID_ARGUMENT;
  }
  std::vector<float> out(lv.size());
  for (size_t i = 0; i < lv.size(); ++i) out[i] = round2(lv[i] - rv[i]);
  set_vector_value(result, out);
  return RC::SUCCESS;
}

RC VectorType::multiply(const Value &left, const Value &right, Value &result) const
{
  std::vector<float> lv, rv;
  RC rc = get_vector_data(left, lv);
  if (OB_FAIL(rc)) return rc;
  rc = get_vector_data(right, rv);
  if (OB_FAIL(rc)) return rc;
  if (lv.size() != rv.size()) {
    LOG_WARN("vector dim mismatch: %zu vs %zu", lv.size(), rv.size());
    return RC::INVALID_ARGUMENT;
  }
  std::vector<float> out(lv.size());
  for (size_t i = 0; i < lv.size(); ++i) out[i] = round2(lv[i] * rv[i]);
  set_vector_value(result, out);
  return RC::SUCCESS;
}

RC VectorType::negative(const Value &val, Value &result) const
{
  std::vector<float> v;
  RC rc = get_vector_data(val, v);
  if (OB_FAIL(rc)) return rc;
  for (auto &x : v) x = round2(-x);
  set_vector_value(result, v);
  return RC::SUCCESS;
}

RC VectorType::to_string(const Value &val, string &result) const
{
  if (val.attr_type() != AttrType::VECTORS) return RC::INVALID_ARGUMENT;
  const int len = val.length();
  if (len == 0) { result = "[]"; return RC::SUCCESS; }
  if (len % static_cast<int>(sizeof(float)) != 0) return RC::INVALID_ARGUMENT;
  int dim = len / static_cast<int>(sizeof(float));
  const float *ptr = reinterpret_cast<const float *>(val.data());
  std::ostringstream oss;
  oss << "[";
  for (int i = 0; i < dim; ++i) {
    float n = round2(ptr[i]);
    // 格式化，最多两位小数，去掉多余0和点
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", n);
    std::string s(buf);
    // 去掉多余的0和点
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    if (s.empty()) s = "0";
    if (i > 0) oss << ",";
    oss << s;
  }
  oss << "]";
  result = oss.str();
  return RC::SUCCESS;
}

RC VectorType::set_value_from_str(Value &val, const string &data) const
{
  std::vector<float> tmp;
  RC rc = get_vector_data(Value(data.c_str()), tmp);
  if (OB_FAIL(rc)) return rc;
  // 维度限制：题一最大1000维；题三需求 16000 维
  const size_t dim = tmp.size();
  if (dim > 16000) {
    LOG_WARN("vector dimension too large: %zu", dim);
    return RC::INVALID_ARGUMENT;
  }
  set_vector_value(val, tmp);
  return RC::SUCCESS;
}

RC VectorType::cast_to(const Value &val, AttrType type, Value &result) const
{
  if (type == AttrType::VECTORS) { result = val; return RC::SUCCESS; }
  if (type == AttrType::CHARS || type == AttrType::TEXTS) {
    std::string s;
    RC rc = to_string(val, s);
    if (OB_FAIL(rc)) return rc;
    if (type == AttrType::CHARS) {
      result.set_string(s.c_str());
    } else {
      // TEXTS
      result.set_string(s.c_str());
      result.set_type(AttrType::TEXTS);
    }
    return RC::SUCCESS;
  }
  return RC::UNSUPPORTED;
}
