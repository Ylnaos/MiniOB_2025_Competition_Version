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
#include <vector>

/**
 * @brief 向量类型
 * @ingroup DataType
 */
class VectorType : public DataType
{
public:
  VectorType() : DataType(AttrType::VECTORS) {}
  virtual ~VectorType() {}

  // For ordering comparisons of vectors, not well-defined; return INT32_MAX to force explicit handling
  int compare(const Value &left, const Value &right) const override { return INT32_MAX; }

  RC add(const Value &left, const Value &right, Value &result) const override;
  RC subtract(const Value &left, const Value &right, Value &result) const override;
  RC multiply(const Value &left, const Value &right, Value &result) const override;

  RC to_string(const Value &val, string &result) const override;

  RC set_value_from_str(Value &val, const string &data) const override;

  // Helpers
  RC decode_vector(const Value &val, std::vector<float> &out) const;

  // Max capacity config
  static constexpr int VECTOR_MAX_DIMS  = 16000;
  static constexpr int VECTOR_MAX_BYTES = static_cast<int>(sizeof(int32_t) + VECTOR_MAX_DIMS * sizeof(float));

private:
  RC parse_literal_to_buffer(const string &data, std::vector<char> &buffer) const;
};
