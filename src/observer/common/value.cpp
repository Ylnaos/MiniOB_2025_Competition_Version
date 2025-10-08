/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by WangYunlai on 2023/06/28.
//

#include "common/value.h"

#include "common/lang/comparator.h"
#include "common/lang/exception.h"
#include "common/lang/sstream.h"
#include "common/lang/string.h"
#include "common/log/log.h"

Value::Value(int val) { set_int(val); }

Value::Value(float val) { set_float(val); }

Value::Value(bool val) { set_boolean(val); }

Value::Value(const char *s, int len /*= 0*/) { set_string(s, len); }

Value::Value(const string_t& s) { set_string(s.data(), s.size()); }


Value::Value(const Value &other)
{
  this->attr_type_ = other.attr_type_;
  this->length_    = other.length_;
  this->own_data_  = other.own_data_;
  switch (this->attr_type_) {
    case AttrType::CHARS:
    case AttrType::TEXTS: {
      set_string_from_other(other);
    } break;

    default: {
      this->value_ = other.value_;
    } break;
  }
}

Value::Value(Value &&other)
{
  this->attr_type_ = other.attr_type_;
  this->length_    = other.length_;
  this->own_data_  = other.own_data_;
  this->value_     = other.value_;
  other.own_data_  = false;
  other.length_    = 0;
}

Value &Value::operator=(const Value &other)
{
  if (this == &other) {
    return *this;
  }
  reset();
  this->attr_type_ = other.attr_type_;
  this->length_    = other.length_;
  this->own_data_  = other.own_data_;
  switch (this->attr_type_) {
    case AttrType::CHARS:
    case AttrType::TEXTS: {
      set_string_from_other(other);
    } break;

    default: {
      this->value_ = other.value_;
    } break;
  }
  return *this;
}

Value &Value::operator=(Value &&other)
{
  if (this == &other) {
    return *this;
  }
  reset();
  this->attr_type_ = other.attr_type_;
  this->length_    = other.length_;
  this->own_data_  = other.own_data_;
  this->value_     = other.value_;
  other.own_data_  = false;
  other.length_    = 0;
  return *this;
}

void Value::reset()
{
  switch (attr_type_) {
    case AttrType::CHARS:
    case AttrType::TEXTS:
    case AttrType::VECTORS:
      if (own_data_ && value_.pointer_value_ != nullptr) {
        delete[] value_.pointer_value_;
        value_.pointer_value_ = nullptr;
      }
      break;
    default: break;
  }

  attr_type_ = AttrType::UNDEFINED;
  length_    = 0;
  own_data_  = false;
}

void Value::set_data(char *data, int length)
{
  switch (attr_type_) {
    case AttrType::CHARS:
    case AttrType::TEXTS: {
      // 保持原有类型（CHARS/TEXTS），避免 set_string 将类型强制为 CHARS
      AttrType old_type = attr_type_;
      set_string(data, length);
      attr_type_ = old_type;
    } break;
    case AttrType::VECTORS: {
      reset();
      attr_type_ = AttrType::VECTORS;
      own_data_ = true;
      length_   = length;
      if (length_ > 0) {
        value_.pointer_value_ = new char[length_];
        memcpy(value_.pointer_value_, data, length_);
      } else {
        value_.pointer_value_ = nullptr;
      }
    } break;
    case AttrType::INTS: {
      value_.int_value_ = *(int *)data;
      length_           = length;
    } break;
    case AttrType::FLOATS: {
      value_.float_value_ = *(float *)data;
      length_             = length;
    } break;
    case AttrType::BOOLEANS: {
      value_.bool_value_ = *(int *)data != 0;
      length_            = length;
    } break;
    case AttrType::DATES: {
      value_.int_value_ = *(int32_t *)data;
      length_           = length;
    } break;
    default: {
      LOG_WARN("unknown data type: %d", attr_type_);
    } break;
  }
}

void Value::set_int(int val)
{
  reset();
  attr_type_        = AttrType::INTS;
  value_.int_value_ = val;
  length_           = sizeof(val);
}

void Value::set_float(float val)
{
  reset();
  attr_type_          = AttrType::FLOATS;
  value_.float_value_ = val;
  length_             = sizeof(val);
}
void Value::set_boolean(bool val)
{
  reset();
  attr_type_         = AttrType::BOOLEANS;
  value_.bool_value_ = val;
  length_            = sizeof(val);
}

void Value::set_date(int32_t val)
{
  reset();
  attr_type_        = AttrType::DATES;
  value_.int_value_ = val;
  length_           = sizeof(val);
}

void Value::set_null()
{
  reset();
  attr_type_ = AttrType::NULLS;
  length_    = 0;
}

void Value::set_string(const char *s, int len /*= 0*/)
{
  reset();
  attr_type_ = AttrType::CHARS;
  if (s == nullptr) {
    value_.pointer_value_ = nullptr;
    length_               = 0;
  } else {
    own_data_ = true;
    if (len > 0) {
      len = strnlen(s, len);
    } else {
      len = strlen(s);
    }
    value_.pointer_value_ = new char[len + 1];
    length_               = len;
    memcpy(value_.pointer_value_, s, len);
    value_.pointer_value_[len] = '\0';
  }
}

void Value::set_empty_string(int len)
{
  reset();
  attr_type_ = AttrType::CHARS;

  own_data_ = true;
  value_.pointer_value_ = new char[len + 1];
  length_               = len;
  memset(value_.pointer_value_, 0, len);
  value_.pointer_value_[len] = '\0';
  
}

void Value::set_value(const Value &value)
{
  switch (value.attr_type_) {
    case AttrType::INTS: {
      set_int(value.get_int());
    } break;
    case AttrType::FLOATS: {
      set_float(value.get_float());
    } break;
    case AttrType::CHARS: {
      set_string(value.get_string().c_str());
    } break;
    case AttrType::BOOLEANS: {
      set_boolean(value.get_boolean());
    } break;
    case AttrType::DATES: {
      set_date(value.get_date());
    } break;
    case AttrType::NULLS: {
      set_null();
    } break;
    default: {
      ASSERT(false, "got an invalid value type");
    } break;
  }
}

void Value::set_string_from_other(const Value &other)
{
  ASSERT(attr_type_ == AttrType::CHARS || attr_type_ == AttrType::TEXTS, "attr type is not string type");
  if (own_data_ && other.value_.pointer_value_ != nullptr && length_ != 0) {
    this->value_.pointer_value_ = new char[this->length_ + 1];
    memcpy(this->value_.pointer_value_, other.value_.pointer_value_, this->length_);
    this->value_.pointer_value_[this->length_] = '\0';
  }
}

char *Value::data() const
{
  switch (attr_type_) {
    case AttrType::CHARS: {
      return value_.pointer_value_;
    } break;
    case AttrType::TEXTS: {
      return value_.pointer_value_;
    } break;
    case AttrType::VECTORS: {
      return value_.pointer_value_;
    } break;
    default: {
      return (char *)&value_;
    } break;
  }
}

string Value::to_string() const
{
  string res;
  RC     rc = DataType::type_instance(this->attr_type_)->to_string(*this, res);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to convert value to string. type=%s", attr_type_to_string(this->attr_type_));
    return "";
  }
  return res;
}

int Value::compare(const Value &other) const
{
  // 统一处理 NULL 的比较，避免具体类型比较中断言或崩溃
  const bool lhs_null = (this->attr_type_ == AttrType::NULLS);
  const bool rhs_null = (other.attr_type() == AttrType::NULLS);
  if (lhs_null || rhs_null) {
    if (lhs_null && rhs_null) return 0;  // NULL == NULL
    if (lhs_null) return -1;             // NULL < 非 NULL
    return 1;                            // 非 NULL > NULL
  }

  auto is_string = [](AttrType t) { return t == AttrType::CHARS || t == AttrType::TEXTS; };
  auto is_number = [](AttrType t) { return t == AttrType::INTS || t == AttrType::FLOATS; };

  // 字符串与数字比较：采用数值语义（如 '16a' -> 16，与 13.5 比较）
  if ((is_string(this->attr_type_) && is_number(other.attr_type())) ||
      (is_number(this->attr_type_) && is_string(other.attr_type()))) {
    float lv = this->get_float();
    float rv = other.get_float();
    return common::compare_float((void *)&lv, (void *)&rv);
  }

  // 字符串与字符串比较：统一使用字典序比较（支持 CHARS/TEXTS 混用）
  if (is_string(this->attr_type_) && is_string(other.attr_type())) {
    auto l = this->get_string_t();
    auto r = other.get_string_t();
    return common::compare_string((void *)l.data(), static_cast<int>(l.size()), (void *)r.data(), static_cast<int>(r.size()));
  }

  return DataType::type_instance(this->attr_type_)->compare(*this, other);
}

int Value::get_int() const
{
  switch (attr_type_) {
    case AttrType::CHARS:
    case AttrType::TEXTS: {
      if (value_.pointer_value_ == nullptr) {
        return 0;
      }
      // 宽松解析：按前缀数字解析（兼容 '16a' -> 16）
      char *endptr = nullptr;
      long  v      = strtol(value_.pointer_value_, &endptr, 10);
      if (endptr == value_.pointer_value_) {
        // 非数字开头，按 0 处理
        return 0;
      }
      return static_cast<int>(v);
    }
    case AttrType::INTS: {
      return value_.int_value_;
    }
    case AttrType::FLOATS: {
      return (int)(value_.float_value_);
    }
    case AttrType::BOOLEANS: {
      return (int)(value_.bool_value_);
    }
    case AttrType::DATES: {
      return value_.int_value_;
    }
    default: {
      LOG_WARN("unknown data type. type=%d", attr_type_);
      return 0;
    }
  }
  return 0;
}

float Value::get_float() const
{
  switch (attr_type_) {
    case AttrType::CHARS:
    case AttrType::TEXTS: {
      if (value_.pointer_value_ == nullptr) {
        return 0.0f;
      }
      // 宽松解析：按前缀数字解析（兼容 '16a' -> 16.0）
      char  *endptr = nullptr;
      double v      = strtod(value_.pointer_value_, &endptr);
      if (endptr == value_.pointer_value_) {
        // 非数字开头，按 0.0 处理
        return 0.0f;
      }
      return static_cast<float>(v);
    } break;
    case AttrType::INTS: {
      return float(value_.int_value_);
    } break;
    case AttrType::FLOATS: {
      return value_.float_value_;
    } break;
    case AttrType::BOOLEANS: {
      return float(value_.bool_value_);
    } break;
    default: {
      LOG_WARN("unknown data type. type=%d", attr_type_);
      return 0;
    }
  }
  return 0;
}

string Value::get_string() const { return this->to_string(); }

string_t Value::get_string_t() const
{
  ASSERT(attr_type_ == AttrType::CHARS || attr_type_ == AttrType::TEXTS, "attr type is not string type");
  return string_t(value_.pointer_value_, length_);
}

bool Value::get_boolean() const
{
  switch (attr_type_) {
    case AttrType::CHARS:
    case AttrType::TEXTS: {
      try {
        float val = stof(value_.pointer_value_);
        if (val >= EPSILON || val <= -EPSILON) {
          return true;
        }

        int int_val = stol(value_.pointer_value_);
        if (int_val != 0) {
          return true;
        }

        return value_.pointer_value_ != nullptr;
      } catch (exception const &ex) {
        LOG_TRACE("failed to convert string to float or integer. s=%s, ex=%s", value_.pointer_value_, ex.what());
        return value_.pointer_value_ != nullptr;
      }
    } break;
    case AttrType::INTS: {
      return value_.int_value_ != 0;
    } break;
    case AttrType::FLOATS: {
      float val = value_.float_value_;
      return val >= EPSILON || val <= -EPSILON;
    } break;
    case AttrType::BOOLEANS: {
      return value_.bool_value_;
    } break;
    default: {
      LOG_WARN("unknown data type. type=%d", attr_type_);
      return false;
    }
  }
  return false;
}

int32_t Value::get_date() const
{
  switch (attr_type_) {
    case AttrType::DATES: {
      return value_.int_value_;
    } break;
    default: {
      LOG_WARN("unknown data type for date. type=%d", attr_type_);
      return 0;
    }
  }
  return 0;
}
