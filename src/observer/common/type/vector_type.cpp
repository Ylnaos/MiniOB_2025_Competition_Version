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
#include "common/log/log.h"
#include "common/value.h"
#include <cctype>
#include <sstream>

using std::string;
using std::vector;

namespace {

inline string trim(const string &s)
{
  size_t i = 0, j = s.size();
  while (i < j && std::isspace(static_cast<unsigned char>(s[i]))) i++;
  while (j > i && std::isspace(static_cast<unsigned char>(s[j - 1]))) j--;
  return s.substr(i, j - i);
}

// Format with 2 decimals then trim zeros & dot
inline string format_float_2(float v)
{
  char buf[64];
  // round to 2 decimals
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  string s(buf);
  // trim trailing zeros
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  if (s.empty()) s = "0";
  return s;
}

} // namespace

// Layout: [int32_t dim][float dim_elems...]
RC VectorType::decode_vector(const Value &val, vector<float> &out) const
{
  if (val.attr_type() != AttrType::VECTORS) {
    return RC::INVALID_ARGUMENT;
  }
  if (val.length() < static_cast<int>(sizeof(int32_t))) {
    return RC::INVALID_ARGUMENT;
  }
  const char *data = val.data();
  int32_t dim      = 0;
  memcpy(&dim, data, sizeof(int32_t));
  if (dim < 0) {
    return RC::INVALID_ARGUMENT;
  }
  size_t bytes = sizeof(int32_t) + static_cast<size_t>(dim) * sizeof(float);
  if (val.length() < static_cast<int>(bytes)) {
    return RC::INVALID_ARGUMENT;
  }
  out.resize(dim);
  if (dim > 0) {
    memcpy(out.data(), data + sizeof(int32_t), static_cast<size_t>(dim) * sizeof(float));
  }
  return RC::SUCCESS;
}

RC VectorType::parse_literal_to_buffer(const string &data, vector<char> &buffer) const
{
  string s = trim(data);
  if (s.empty()) return RC::INVALID_ARGUMENT;
  if (s.front() != '[' || s.back() != ']') return RC::INVALID_ARGUMENT;
  s = s.substr(1, s.size() - 2);

  vector<float> elems;
  elems.reserve(8);
  string token;
  std::stringstream ss(s);
  while (std::getline(ss, token, ',')) {
    token = trim(token);
    if (token.empty()) continue;
    // allow int or float
    try {
      float v = std::stof(token);
      elems.push_back(v);
    } catch (...) {
      return RC::INVALID_ARGUMENT;
    }
    if (elems.size() > static_cast<size_t>(VECTOR_MAX_DIMS)) {
      return RC::INVALID_ARGUMENT;
    }
  }

  int32_t dim = static_cast<int32_t>(elems.size());
  size_t  need = sizeof(int32_t) + static_cast<size_t>(dim) * sizeof(float);
  if (need > static_cast<size_t>(VECTOR_MAX_BYTES)) {
    return RC::INVALID_ARGUMENT;
  }
  buffer.resize(need);
  memcpy(buffer.data(), &dim, sizeof(int32_t));
  if (dim > 0) {
    memcpy(buffer.data() + sizeof(int32_t), elems.data(), static_cast<size_t>(dim) * sizeof(float));
  }
  return RC::SUCCESS;
}

RC VectorType::set_value_from_str(Value &val, const string &data) const
{
  vector<char> buf;
  RC rc = parse_literal_to_buffer(data, buf);
  if (OB_FAIL(rc)) {
    return rc;
  }
  val.reset();
  val.set_type(AttrType::VECTORS);
  if (buf.empty()) {
    // Disambiguate overload resolution for nullptr
    val.set_data(static_cast<char *>(nullptr), 0);
  } else {
    // allocate and own
    char *p = new char[buf.size()];
    memcpy(p, buf.data(), buf.size());
    val.set_data(p, static_cast<int>(buf.size()));
  }
  return RC::SUCCESS;
}

// Element-wise ops; accepts right as vector or string literal
static RC resolve_any_vector(const Value &v, vector<float> &out)
{
  if (v.attr_type() == AttrType::VECTORS) {
    return static_cast<VectorType *>(DataType::type_instance(AttrType::VECTORS))->decode_vector(v, out);
  }
  if (v.attr_type() == AttrType::CHARS || v.attr_type() == AttrType::TEXTS) {
    Value tmp;
    RC rc = DataType::type_instance(v.attr_type())->cast_to(v, AttrType::VECTORS, tmp);
    if (OB_FAIL(rc)) return rc;
    return static_cast<VectorType *>(DataType::type_instance(AttrType::VECTORS))->decode_vector(tmp, out);
  }
  return RC::INVALID_ARGUMENT;
}

static RC build_vector_value(const vector<float> &vec, Value &out)
{
  int32_t dim = static_cast<int32_t>(vec.size());
  size_t  bytes = sizeof(int32_t) + static_cast<size_t>(dim) * sizeof(float);
  if (bytes > static_cast<size_t>(VectorType::VECTOR_MAX_BYTES)) {
    return RC::INVALID_ARGUMENT;
  }
  out.reset();
  out.set_type(AttrType::VECTORS);
  char *buf = new char[bytes];
  memcpy(buf, &dim, sizeof(int32_t));
  if (dim > 0) memcpy(buf + sizeof(int32_t), vec.data(), static_cast<size_t>(dim) * sizeof(float));
  out.set_data(buf, static_cast<int>(bytes));
  return RC::SUCCESS;
}

static RC elementwise_binary(const Value &left, const Value &right, Value &result, int op)
{
  vector<float> a, b;
  RC rc = resolve_any_vector(left, a);
  if (OB_FAIL(rc)) return rc;
  rc = resolve_any_vector(right, b);
  if (OB_FAIL(rc)) return rc;
  if (a.size() != b.size()) return RC::INVALID_ARGUMENT;
  vector<float> c(a.size());
  switch (op) {
    case 0: // add
      for (size_t i = 0; i < a.size(); ++i) c[i] = a[i] + b[i];
      break;
    case 1: // sub
      for (size_t i = 0; i < a.size(); ++i) c[i] = a[i] - b[i];
      break;
    case 2: // mul
      for (size_t i = 0; i < a.size(); ++i) c[i] = a[i] * b[i];
      break;
    default:
      return RC::UNSUPPORTED;
  }
  return build_vector_value(c, result);
}

RC VectorType::add(const Value &left, const Value &right, Value &result) const
{
  return elementwise_binary(left, right, result, 0);
}

RC VectorType::subtract(const Value &left, const Value &right, Value &result) const
{
  return elementwise_binary(left, right, result, 1);
}

RC VectorType::multiply(const Value &left, const Value &right, Value &result) const
{
  return elementwise_binary(left, right, result, 2);
}

RC VectorType::to_string(const Value &val, string &result) const
{
  vector<float> elems;
  RC rc = decode_vector(val, elems);
  if (OB_FAIL(rc)) {
    result.clear();
    return rc;
  }
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < elems.size(); ++i) {
    if (i > 0) os << ",";
    os << format_float_2(elems[i]);
  }
  os << "]";
  result = os.str();
  return RC::SUCCESS;
}
