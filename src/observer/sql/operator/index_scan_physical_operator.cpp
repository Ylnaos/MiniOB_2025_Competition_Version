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
// Created by Wangyunlai on 2022/07/08.
//

#include "sql/operator/index_scan_physical_operator.h"
#include "storage/index/index.h"
#include "storage/trx/trx.h"
#include <limits>

IndexScanPhysicalOperator::IndexScanPhysicalOperator(Table *table, Index *index, ReadWriteMode mode, const Value *left_value,
    bool left_inclusive, const Value *right_value, bool right_inclusive)
    : table_(table),
      index_(index),
      mode_(mode),
      left_inclusive_(left_inclusive),
      right_inclusive_(right_inclusive)
{
  if (left_value) {
    left_value_ = *left_value;
  }
  if (right_value) {
    right_value_ = *right_value;
  }
}

RC IndexScanPhysicalOperator::open(Trx *trx)
{
  if (nullptr == table_ || nullptr == index_) {
    return RC::INTERNAL;
  }

  // 如果是复合索引（多列），构造前缀范围 [first=val, last=val]，其余列取最小/最大，且使用可排序编码
  if (index_->key_fields().size() > 1) {
    const auto &fields = index_->key_fields();
    int total_len = 0;
    for (const auto &fm : fields) total_len += fm.len();
    std::string left_bytes(total_len, '\0');
    std::string right_bytes(total_len, '\0');

    auto put_be32 = [](uint32_t v, char *out) {
      out[0] = static_cast<char>((v >> 24) & 0xFF);
      out[1] = static_cast<char>((v >> 16) & 0xFF);
      out[2] = static_cast<char>((v >> 8) & 0xFF);
      out[3] = static_cast<char>(v & 0xFF);
    };

    auto encode_int = [&](int32_t iv, char *out) {
      uint32_t uv = static_cast<uint32_t>(iv) ^ 0x80000000u;
      put_be32(uv, out);
    };
    auto encode_float = [&](float fv, char *out) {
      uint32_t u; memcpy(&u, &fv, sizeof(u));
      if (u & 0x80000000u) { u = ~u; } else { u ^= 0x80000000u; }
      put_be32(u, out);
    };

    int off = 0;
    // 第一列由谓词给定
    const FieldMeta &f0 = fields[0];
    switch (f0.type()) {
      case AttrType::INTS: {
        int32_t v = left_value_.get_int();
        encode_int(v, &left_bytes[off]);
        encode_int(v, &right_bytes[off]);
        off += sizeof(int32_t);
      } break;
      case AttrType::DATES: {
        // 如果传入的值是字符串类型，需要先转换为 DATE 类型
        int32_t v;
        if (left_value_.attr_type() == AttrType::CHARS || left_value_.attr_type() == AttrType::TEXTS) {
          Value date_value;
          RC rc = Value::cast_to(left_value_, AttrType::DATES, date_value);
          if (OB_FAIL(rc)) {
            LOG_WARN("failed to cast string to date for composite index scan. value=%s", left_value_.to_string().c_str());
            return rc;
          }
          v = date_value.get_date();
        } else {
          v = left_value_.get_int();
        }
        encode_int(v, &left_bytes[off]);
        encode_int(v, &right_bytes[off]);
        off += sizeof(int32_t);
      } break;
      case AttrType::FLOATS: {
        float v = left_value_.get_float();
        encode_float(v, &left_bytes[off]);
        encode_float(v, &right_bytes[off]);
        off += sizeof(float);
      } break;
      case AttrType::CHARS:
      case AttrType::TEXTS: {
        // 拷贝并填充（固定长度）
        auto s = left_value_.get_string();
        int cpy = std::min<int>(f0.len(), (int)s.size());
        memcpy(&left_bytes[off], s.data(), cpy);
        memcpy(&right_bytes[off], s.data(), cpy);
        off += f0.len();
      } break;
      case AttrType::BOOLEANS: {
        int32_t v = left_value_.get_boolean() ? 1 : 0;
        encode_int(v, &left_bytes[off]);
        encode_int(v, &right_bytes[off]);
        off += sizeof(int32_t);
      } break;
      default: {
        // 原样拷贝
        memcpy(&left_bytes[off], left_value_.data(), f0.len());
        memcpy(&right_bytes[off], left_value_.data(), f0.len());
        off += f0.len();
      } break;
    }

    // 其余列 left 取最小，right 取最大
    for (size_t i = 1; i < fields.size(); i++) {
      const FieldMeta &fm = fields[i];
      switch (fm.type()) {
        case AttrType::INTS:
        case AttrType::DATES: {
          encode_int(std::numeric_limits<int32_t>::min(), &left_bytes[off]);
          encode_int(std::numeric_limits<int32_t>::max(), &right_bytes[off]);
          off += sizeof(int32_t);
        } break;
        case AttrType::FLOATS: {
          encode_float(-std::numeric_limits<float>::max(), &left_bytes[off]);
          encode_float(std::numeric_limits<float>::max(), &right_bytes[off]);
          off += sizeof(float);
        } break;
        case AttrType::CHARS:
        case AttrType::TEXTS: {
          // left 已是 0, right 填 0xFF
          memset(&right_bytes[off], 0xFF, fm.len());
          off += fm.len();
        } break;
        case AttrType::BOOLEANS: {
          encode_int(0, &left_bytes[off]);
          encode_int(1, &right_bytes[off]);
          off += sizeof(int32_t);
        } break;
        default: {
          // 其他类型，不清楚编码，取全范围
          memset(&left_bytes[off], 0x00, fm.len());
          memset(&right_bytes[off], 0xFF, fm.len());
          off += fm.len();
        } break;
      }
    }

    // 用 CHARS 包装原始字节传入扫描器（比较按字节序）
    left_value_.set_type(AttrType::CHARS);
    left_value_.set_data(left_bytes.data(), (int)left_bytes.size());
    right_value_.set_type(AttrType::CHARS);
    right_value_.set_data(right_bytes.data(), (int)right_bytes.size());
    left_inclusive_  = true;
    right_inclusive_ = true;
  }
  else if (index_->key_fields().size() == 1) {
    const auto &fields = index_->key_fields();
    const FieldMeta &fm = fields[0];

    auto put_be32 = [](uint32_t v, char *out) {
      out[0] = static_cast<char>((v >> 24) & 0xFF);
      out[1] = static_cast<char>((v >> 16) & 0xFF);
      out[2] = static_cast<char>((v >> 8) & 0xFF);
      out[3] = static_cast<char>(v & 0xFF);
    };
    auto encode_int = [&](int32_t iv, char *out) {
      uint32_t uv = static_cast<uint32_t>(iv) ^ 0x80000000u;
      put_be32(uv, out);
    };
    auto encode_float = [&](float fv, char *out) {
      uint32_t u; memcpy(&u, &fv, sizeof(u));
      if (u & 0x80000000u) { u = ~u; } else { u ^= 0x80000000u; }
      put_be32(u, out);
    };

    std::string left_bytes(fm.len(), '\0');
    std::string right_bytes(fm.len(), '\0');

    switch (fm.type()) {
      case AttrType::INTS: {
        int32_t vL = left_value_.get_int();
        int32_t vR = (right_value_.attr_type() == AttrType::UNDEFINED) ? vL : right_value_.get_int();
        encode_int(vL, &left_bytes[0]);
        encode_int(vR, &right_bytes[0]);
      } break;
      case AttrType::DATES: {
        // 如果传入的值是字符串类型，需要先转换为 DATE 类型
        Value left_date_value, right_date_value;
        int32_t vL, vR;

        if (left_value_.attr_type() == AttrType::CHARS || left_value_.attr_type() == AttrType::TEXTS) {
          RC rc = Value::cast_to(left_value_, AttrType::DATES, left_date_value);
          if (OB_FAIL(rc)) {
            LOG_WARN("failed to cast string to date for index scan. value=%s", left_value_.to_string().c_str());
            return rc;
          }
          vL = left_date_value.get_date();
        } else {
          vL = left_value_.get_int();
        }

        if (right_value_.attr_type() == AttrType::UNDEFINED) {
          vR = vL;
        } else if (right_value_.attr_type() == AttrType::CHARS || right_value_.attr_type() == AttrType::TEXTS) {
          RC rc = Value::cast_to(right_value_, AttrType::DATES, right_date_value);
          if (OB_FAIL(rc)) {
            LOG_WARN("failed to cast string to date for index scan. value=%s", right_value_.to_string().c_str());
            return rc;
          }
          vR = right_date_value.get_date();
        } else {
          vR = right_value_.get_int();
        }

        encode_int(vL, &left_bytes[0]);
        encode_int(vR, &right_bytes[0]);
      } break;
      case AttrType::FLOATS: {
        float vL = left_value_.get_float();
        float vR = (right_value_.attr_type() == AttrType::UNDEFINED) ? vL : right_value_.get_float();
        encode_float(vL, &left_bytes[0]);
        encode_float(vR, &right_bytes[0]);
      } break;
      case AttrType::CHARS:
      case AttrType::TEXTS: {
        auto sL = left_value_.get_string();
        auto sR = (right_value_.attr_type() == AttrType::UNDEFINED) ? sL : right_value_.get_string();
        int  cL = std::min<int>(fm.len(), (int)sL.size());
        int  cR = std::min<int>(fm.len(), (int)sR.size());
        memcpy(&left_bytes[0], sL.data(), cL);
        memcpy(&right_bytes[0], sR.data(), cR);
      } break;
      case AttrType::BOOLEANS: {
        int32_t vL = left_value_.get_boolean() ? 1 : 0;
        int32_t vR = (right_value_.attr_type() == AttrType::UNDEFINED ? vL : (right_value_.get_boolean() ? 1 : 0));
        encode_int(vL, &left_bytes[0]);
        encode_int(vR, &right_bytes[0]);
      } break;
      default: {
        memcpy(&left_bytes[0], left_value_.data(), std::min(fm.len(), left_value_.length()));
        if (right_value_.attr_type() == AttrType::UNDEFINED) {
          memcpy(&right_bytes[0], left_value_.data(), std::min(fm.len(), left_value_.length()));
        } else {
          memcpy(&right_bytes[0], right_value_.data(), std::min(fm.len(), right_value_.length()));
        }
      } break;
    }

    left_value_.set_type(AttrType::CHARS);
    left_value_.set_data(left_bytes.data(), (int)left_bytes.size());
    right_value_.set_type(AttrType::CHARS);
    right_value_.set_data(right_bytes.data(), (int)right_bytes.size());
  }

  IndexScanner *index_scanner = index_->create_scanner(left_value_.data(),
      left_value_.length(),
      left_inclusive_,
      right_value_.data(),
      right_value_.length(),
      right_inclusive_);
  if (nullptr == index_scanner) {
    LOG_WARN("failed to create index scanner");
    return RC::INTERNAL;
  }
  index_scanner_ = index_scanner;

  tuple_.set_schema(table_, table_->table_meta().field_metas(), alias_);

  trx_ = trx;
  return RC::SUCCESS;
}

RC IndexScanPhysicalOperator::next()
{
  // TODO: 需要适配 lsm-tree 引擎
  RID rid;
  RC  rc = RC::SUCCESS;

  bool filter_result = false;
  while (RC::SUCCESS == (rc = index_scanner_->next_entry(&rid))) {
    rc = table_->get_record(rid, current_record_);
    if (OB_FAIL(rc)) {
      LOG_TRACE("failed to get record. rid=%s, rc=%s", rid.to_string().c_str(), strrc(rc));
      return rc;
    }

    LOG_TRACE("got a record. rid=%s", rid.to_string().c_str());

    tuple_.set_record(&current_record_);
    rc = filter(tuple_, filter_result);
    if (OB_FAIL(rc)) {
      LOG_TRACE("failed to filter record. rc=%s", strrc(rc));
      return rc;
    }

    if (!filter_result) {
      LOG_TRACE("record filtered");
      continue;
    }

    rc = trx_->visit_record(table_, current_record_, mode_);
    if (rc == RC::RECORD_INVISIBLE) {
      LOG_TRACE("record invisible");
      continue;
    } else {
      return rc;
    }
  }

  return rc;
}

RC IndexScanPhysicalOperator::close()
{
  index_scanner_->destroy();
  index_scanner_ = nullptr;
  return RC::SUCCESS;
}

Tuple *IndexScanPhysicalOperator::current_tuple()
{
  tuple_.set_record(&current_record_);
  return &tuple_;
}

void IndexScanPhysicalOperator::set_predicates(vector<unique_ptr<Expression>> &&exprs)
{
  predicates_ = std::move(exprs);
}

RC IndexScanPhysicalOperator::filter(RowTuple &tuple, bool &result)
{
  RC    rc = RC::SUCCESS;
  Value value;
  for (unique_ptr<Expression> &expr : predicates_) {
    rc = expr->get_value(tuple, value);
    if (rc != RC::SUCCESS) {
      return rc;
    }

    bool tmp_result = value.get_boolean();
    if (!tmp_result) {
      result = false;
      return rc;
    }
  }

  result = true;
  return rc;
}

string IndexScanPhysicalOperator::param() const
{
  return string(index_->index_meta().name()) + " ON " + table_->name();
}
