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
#include "common/lang/sstream.h"
#include "common/log/log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <vector>

struct VectorData {
  int32_t dimension;
  float *data;

  VectorData(int dim) : dimension(dim) {
    data = new float[dim];
    memset(data, 0, dim * sizeof(float));
  }

  VectorData(const VectorData &other) : dimension(other.dimension) {
    data = new float[dimension];
    memcpy(data, other.data, dimension * sizeof(float));
  }

  ~VectorData() {
    delete[] data;
  }
};

static VectorData* parse_vector(const char *str) {
  if (!str) return nullptr;

  string s(str);
  // Remove whitespace
  s.erase(remove_if(s.begin(), s.end(), [](char ch) { return std::isspace(static_cast<unsigned char>(ch)); }), s.end());

  // Check for '[' and ']' or just numbers
  if (!s.empty() && s[0] == '[') {
    if (s.back() != ']') {
      LOG_WARN("Invalid vector format: missing closing bracket");
      return nullptr;
    }
    s = s.substr(1, s.length() - 2);
  }

  if (s.empty()) {
    LOG_WARN("Empty vector string");
    return nullptr;
  }

  // Parse comma-separated floats
  vector<float> values;
  stringstream ss(s);
  string item;

  while (getline(ss, item, ',')) {
    try {
      float val = stof(item);
      values.push_back(val);
    } catch (...) {
      LOG_WARN("Failed to parse vector element: %s", item.c_str());
      return nullptr;
    }
  }

  if (values.empty() || values.size() > 16000) {
    LOG_WARN("Invalid vector dimension: %zu", values.size());
    return nullptr;
  }

  VectorData *vec = new VectorData(values.size());
  for (size_t i = 0; i < values.size(); i++) {
    vec->data[i] = values[i];
  }

  return vec;
}

RC VectorType::set_value_from_str(Value &val, const string &data) const {
  VectorData *vec = parse_vector(data.c_str());
  if (!vec) {
    return RC::INVALID_ARGUMENT;
  }

  // Store vector data in Value
  int total_size = sizeof(int32_t) + vec->dimension * sizeof(float);
  char *buffer = new char[total_size];

  // Store dimension first
  memcpy(buffer, &vec->dimension, sizeof(int32_t));
  // Then store data
  memcpy(buffer + sizeof(int32_t), vec->data, vec->dimension * sizeof(float));

  val.set_type(AttrType::VECTORS);
  val.set_data(buffer, total_size);

  delete[] buffer;
  delete vec;
  return RC::SUCCESS;
}

RC VectorType::to_string(const Value &val, string &result) const {
  if (val.attr_type() != AttrType::VECTORS) {
    return RC::INVALID_ARGUMENT;
  }

  const char *data = val.data();
  if (!data) {
    result = "NULL";
    return RC::SUCCESS;
  }

  int32_t dimension = *(int32_t*)data;
  const float *vec_data = (const float*)(data + sizeof(int32_t));

  stringstream ss;
  ss << "[";
  for (int i = 0; i < dimension; i++) {
    if (i > 0) ss << ",";
    ss << fixed << setprecision(6) << vec_data[i];
  }
  ss << "]";

  result = ss.str();
  return RC::SUCCESS;
}

int VectorType::compare(const Value &left, const Value &right) const {
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return INT32_MAX;
  }

  const char *left_data = left.data();
  const char *right_data = right.data();

  if (!left_data && !right_data) return 0;
  if (!left_data) return -1;
  if (!right_data) return 1;

  int32_t left_dim = *(int32_t*)left_data;
  int32_t right_dim = *(int32_t*)right_data;

  if (left_dim != right_dim) {
    return left_dim < right_dim ? -1 : 1;
  }

  const float *left_vec = (const float*)(left_data + sizeof(int32_t));
  const float *right_vec = (const float*)(right_data + sizeof(int32_t));

  // Compare element by element
  for (int i = 0; i < left_dim; i++) {
    if (left_vec[i] < right_vec[i]) return -1;
    if (left_vec[i] > right_vec[i]) return 1;
  }

  return 0;
}

RC VectorType::add(const Value &left, const Value &right, Value &result) const {
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return RC::INVALID_ARGUMENT;
  }

  const char *left_data = left.data();
  const char *right_data = right.data();

  if (!left_data || !right_data) {
    return RC::INVALID_ARGUMENT;
  }

  int32_t left_dim = *(int32_t*)left_data;
  int32_t right_dim = *(int32_t*)right_data;

  if (left_dim != right_dim) {
    LOG_WARN("Vector dimensions don't match: %d vs %d", left_dim, right_dim);
    return RC::INVALID_ARGUMENT;
  }

  const float *left_vec = (const float*)(left_data + sizeof(int32_t));
  const float *right_vec = (const float*)(right_data + sizeof(int32_t));

  // Create result vector
  int total_size = sizeof(int32_t) + left_dim * sizeof(float);
  char *buffer = new char[total_size];

  *(int32_t*)buffer = left_dim;
  float *result_vec = (float*)(buffer + sizeof(int32_t));

  for (int i = 0; i < left_dim; i++) {
    result_vec[i] = left_vec[i] + right_vec[i];
  }

  result.set_type(AttrType::VECTORS);
  result.set_data(buffer, total_size);

  delete[] buffer;
  return RC::SUCCESS;
}

RC VectorType::subtract(const Value &left, const Value &right, Value &result) const {
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return RC::INVALID_ARGUMENT;
  }

  const char *left_data = left.data();
  const char *right_data = right.data();

  if (!left_data || !right_data) {
    return RC::INVALID_ARGUMENT;
  }

  int32_t left_dim = *(int32_t*)left_data;
  int32_t right_dim = *(int32_t*)right_data;

  if (left_dim != right_dim) {
    LOG_WARN("Vector dimensions don't match: %d vs %d", left_dim, right_dim);
    return RC::INVALID_ARGUMENT;
  }

  const float *left_vec = (const float*)(left_data + sizeof(int32_t));
  const float *right_vec = (const float*)(right_data + sizeof(int32_t));

  // Create result vector
  int total_size = sizeof(int32_t) + left_dim * sizeof(float);
  char *buffer = new char[total_size];

  *(int32_t*)buffer = left_dim;
  float *result_vec = (float*)(buffer + sizeof(int32_t));

  for (int i = 0; i < left_dim; i++) {
    result_vec[i] = left_vec[i] - right_vec[i];
  }

  result.set_type(AttrType::VECTORS);
  result.set_data(buffer, total_size);

  delete[] buffer;
  return RC::SUCCESS;
}

RC VectorType::multiply(const Value &left, const Value &right, Value &result) const {
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return RC::INVALID_ARGUMENT;
  }

  const char *left_data = left.data();
  const char *right_data = right.data();

  if (!left_data || !right_data) {
    return RC::INVALID_ARGUMENT;
  }

  int32_t left_dim = *(int32_t*)left_data;
  int32_t right_dim = *(int32_t*)right_data;

  if (left_dim != right_dim) {
    LOG_WARN("Vector dimensions don't match: %d vs %d", left_dim, right_dim);
    return RC::INVALID_ARGUMENT;
  }

  const float *left_vec = (const float*)(left_data + sizeof(int32_t));
  const float *right_vec = (const float*)(right_data + sizeof(int32_t));

  // Element-wise multiplication
  int total_size = sizeof(int32_t) + left_dim * sizeof(float);
  char *buffer = new char[total_size];

  *(int32_t*)buffer = left_dim;
  float *result_vec = (float*)(buffer + sizeof(int32_t));

  for (int i = 0; i < left_dim; i++) {
    result_vec[i] = left_vec[i] * right_vec[i];
  }

  result.set_type(AttrType::VECTORS);
  result.set_data(buffer, total_size);

  delete[] buffer;
  return RC::SUCCESS;
}

RC VectorType::divide(const Value &left, const Value &right, Value &result) const {
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return RC::INVALID_ARGUMENT;
  }

  const char *left_data = left.data();
  const char *right_data = right.data();

  if (!left_data || !right_data) {
    return RC::INVALID_ARGUMENT;
  }

  int32_t left_dim = *(int32_t*)left_data;
  int32_t right_dim = *(int32_t*)right_data;

  if (left_dim != right_dim) {
    LOG_WARN("Vector dimensions don't match: %d vs %d", left_dim, right_dim);
    return RC::INVALID_ARGUMENT;
  }

  const float *left_vec = (const float*)(left_data + sizeof(int32_t));
  const float *right_vec = (const float*)(right_data + sizeof(int32_t));

  // Element-wise division
  int total_size = sizeof(int32_t) + left_dim * sizeof(float);
  char *buffer = new char[total_size];

  *(int32_t*)buffer = left_dim;
  float *result_vec = (float*)(buffer + sizeof(int32_t));

  for (int i = 0; i < left_dim; i++) {
    if (right_vec[i] == 0) {
      delete[] buffer;
      return RC::INVALID_ARGUMENT;
    }
    result_vec[i] = left_vec[i] / right_vec[i];
  }

  result.set_type(AttrType::VECTORS);
  result.set_data(buffer, total_size);

  delete[] buffer;
  return RC::SUCCESS;
}

// Helper function to calculate L2 distance
float VectorType::l2_distance(const Value &left, const Value &right) {
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return -1.0f;
  }

  const char *left_data = left.data();
  const char *right_data = right.data();

  if (!left_data || !right_data) {
    return -1.0f;
  }

  int32_t left_dim = *(int32_t*)left_data;
  int32_t right_dim = *(int32_t*)right_data;

  if (left_dim != right_dim) {
    return -1.0f;
  }

  const float *left_vec = (const float*)(left_data + sizeof(int32_t));
  const float *right_vec = (const float*)(right_data + sizeof(int32_t));

  float sum = 0.0f;
  for (int i = 0; i < left_dim; i++) {
    float diff = left_vec[i] - right_vec[i];
    sum += diff * diff;
  }

  return sqrt(sum);
}

// Helper function to calculate cosine distance
float VectorType::cosine_distance(const Value &left, const Value &right) {
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return -1.0f;
  }

  const char *left_data = left.data();
  const char *right_data = right.data();

  if (!left_data || !right_data) {
    return -1.0f;
  }

  int32_t left_dim = *(int32_t*)left_data;
  int32_t right_dim = *(int32_t*)right_data;

  if (left_dim != right_dim) {
    return -1.0f;
  }

  const float *left_vec = (const float*)(left_data + sizeof(int32_t));
  const float *right_vec = (const float*)(right_data + sizeof(int32_t));

  float dot_product = 0.0f;
  float left_norm = 0.0f;
  float right_norm = 0.0f;

  for (int i = 0; i < left_dim; i++) {
    dot_product += left_vec[i] * right_vec[i];
    left_norm += left_vec[i] * left_vec[i];
    right_norm += right_vec[i] * right_vec[i];
  }

  left_norm = sqrt(left_norm);
  right_norm = sqrt(right_norm);

  if (left_norm == 0.0f || right_norm == 0.0f) {
    return 1.0f; // Maximum distance
  }

  float cosine_similarity = dot_product / (left_norm * right_norm);
  return 1.0f - cosine_similarity;
}

// Helper function to calculate inner product
float VectorType::inner_product(const Value &left, const Value &right) {
  if (left.attr_type() != AttrType::VECTORS || right.attr_type() != AttrType::VECTORS) {
    return 0.0f;
  }

  const char *left_data = left.data();
  const char *right_data = right.data();

  if (!left_data || !right_data) {
    return 0.0f;
  }

  int32_t left_dim = *(int32_t*)left_data;
  int32_t right_dim = *(int32_t*)right_data;

  if (left_dim != right_dim) {
    return 0.0f;
  }

  const float *left_vec = (const float*)(left_data + sizeof(int32_t));
  const float *right_vec = (const float*)(right_data + sizeof(int32_t));

  float sum = 0.0f;
  for (int i = 0; i < left_dim; i++) {
    sum += left_vec[i] * right_vec[i];
  }

  return sum;
}
