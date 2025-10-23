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
// Created by Wangyunlai on 2021/5/14.
//

#pragma once

#include "common/log/log.h"
#include "common/lang/string.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple_cell.h"
#include "sql/parser/parse.h"
#include "common/value.h"
#include <memory>
#include <cstring>
#include <cstdint>
#include <string>
#include "storage/record/record.h"
#include "storage/record/lob_ref.h"
#include "storage/table/table.h"

class Table;

/**
 * @defgroup Tuple
 * @brief Tuple 元组，表示一行数据，当前返回客户端时使用
 * @details
 * tuple是一种可以嵌套的数据结构。
 * 比如select t1.a+t2.b from t1, t2;
 * 需要使用下面的结构表示：
 * @code {.cpp}
 *  Project(t1.a+t2.b)
 *        |
 *      Joined
 *      /     \
 *   Row(t1) Row(t2)
 * @endcode
 * TODO 一个类拆分成一个文件，并放到单独的目录中
 */

/**
 * @brief 元组的结构，包含哪些字段(这里成为Cell)，每个字段的说明
 * @ingroup Tuple
 */
class TupleSchema
{
public:
  void append_cell(const TupleCellSpec &cell) { cells_.push_back(cell); }
  void append_cell(const char *table, const char *field) { append_cell(TupleCellSpec(table, field)); }
  void append_cell(const char *alias) { append_cell(TupleCellSpec(alias)); }
  int  cell_num() const { return static_cast<int>(cells_.size()); }

  const TupleCellSpec &cell_at(int i) const { return cells_[i]; }

private:
  vector<TupleCellSpec> cells_;
};

/**
 * @brief 元组的抽象描述
 * @ingroup Tuple
 */
class Tuple
{
public:
  Tuple()          = default;
  virtual ~Tuple() = default;

  /**
   * @brief 获取元组中的Cell的个数
   * @details 个数应该与tuple_schema一致
   */
  virtual int cell_num() const = 0;

  /**
   * @brief 获取指定位置的Cell
   *
   * @param index 位置
   * @param[out] cell  返回的Cell
   */
  virtual RC cell_at(int index, Value &cell) const = 0;

  virtual RC spec_at(int index, TupleCellSpec &spec) const = 0;

  /**
   * @brief 根据cell的描述，获取cell的值
   *
   * @param spec cell的描述
   * @param[out] cell 返回的cell
   */
  virtual RC find_cell(const TupleCellSpec &spec, Value &cell) const = 0;

  virtual string to_string() const
  {
    string    str;
    const int cell_num = this->cell_num();
    for (int i = 0; i < cell_num - 1; i++) {
      Value cell;
      cell_at(i, cell);
      str += cell.to_string();
      str += ", ";
    }

    if (cell_num > 0) {
      Value cell;
      cell_at(cell_num - 1, cell);
      str += cell.to_string();
    }
    return str;
  }

  virtual RC compare(const Tuple &other, int &result) const
  {
    RC rc = RC::SUCCESS;

    const int this_cell_num  = this->cell_num();
    const int other_cell_num = other.cell_num();
    if (this_cell_num < other_cell_num) {
      result = -1;
      return rc;
    }
    if (this_cell_num > other_cell_num) {
      result = 1;
      return rc;
    }

    Value this_value;
    Value other_value;
    for (int i = 0; i < this_cell_num; i++) {
      rc = this->cell_at(i, this_value);
      if (OB_FAIL(rc)) {
        return rc;
      }

      rc = other.cell_at(i, other_value);
      if (OB_FAIL(rc)) {
        return rc;
      }

      result = this_value.compare(other_value);
      if (0 != result) {
        return rc;
      }
    }

    result = 0;
    return rc;
  }
};

/**
 * @brief 一行数据的元组
 * @ingroup Tuple
 * @details 直接就是获取表中的一条记录
 */
class RowTuple : public Tuple
{
public:
  RowTuple() = default;
  virtual ~RowTuple()
  {
    for (FieldExpr *spec : speces_) {
      delete spec;
    }
    speces_.clear();
  }

  void set_record(Record *record) { this->record_ = record; }

  void set_schema(const Table *table, const vector<FieldMeta> *fields, const std::string &alias = std::string())
  {
    table_ = table;
    alias_ = alias;
    if (!alias_.empty()) {
      common::str_to_upper(alias_);
    }
    // fix:join当中会多次调用右表的open,open当中会调用set_scheme，从而导致tuple当中会存储
    // 很多无意义的field和value，因此需要先clear掉
    for (FieldExpr *spec : speces_) {
      delete spec;
    }
    this->speces_.clear();
    this->speces_.reserve(fields->size());
    for (const FieldMeta &field : *fields) {
      speces_.push_back(new FieldExpr(table, &field));
    }
  }

  int cell_num() const override { return speces_.size(); }

  RC cell_at(int index, Value &cell) const override
  {
    if (index < 0 || index >= static_cast<int>(speces_.size())) {
      LOG_WARN("invalid argument. index=%d", index);
      return RC::INVALID_ARGUMENT;
    }

    FieldExpr       *field_expr = speces_[index];
    const FieldMeta *field_meta = field_expr->field().meta();
    // NULL check for visible(user) fields using table's NULL bitmap
    if (field_meta->visible()) {
      const TableMeta &tbl_meta = table_->table_meta();
      int nb_off = tbl_meta.null_bitmap_offset();
      int fid    = field_meta->field_id();
      if (nb_off >= 0 && fid >= 0) {
        const unsigned char *nb = reinterpret_cast<const unsigned char *>(this->record_->data() + nb_off);
        unsigned char mask = 1U << (fid % 8);
        if (nb[fid / 8] & mask) {
          cell.set_null();
          return RC::SUCCESS;
        }
      }
    }
    // TEXTS are stored as LobRef in record; need to fetch from LOB file
    if (field_meta->type() == AttrType::TEXTS) {
      // Decode LobRef in a layout-safe way to avoid padding issues
      const char *p = this->record_->data() + field_meta->offset();
      LobRef ref;
      static_assert(sizeof(LobRef) == sizeof(int32_t) + sizeof(int64_t) || sizeof(LobRef) == 16,
                    "Unexpected LobRef size");
      memcpy(&ref, p, sizeof(LobRef));

      const int32_t length = ref.length;
      const int64_t offset = ref.offset;

      if (length <= 0) {
        // empty text
        cell.set_type(AttrType::TEXTS);
        cell.set_data("", 0);
        return RC::SUCCESS;
      }
      if (table_ == nullptr || table_->lob_handler() == nullptr) {
        return RC::INTERNAL;
      }
      std::vector<char> buf;
      buf.resize(static_cast<size_t>(length));
      RC rc = table_->lob_handler()->get_data(offset, length, buf.data());
      if (rc != RC::SUCCESS) {
        return rc;
      }
      cell.set_type(AttrType::TEXTS);
      cell.set_data(buf.data(), length);
      return RC::SUCCESS;
    }
    // 向量字段：高维或历史 LobRef 存储需要回表取数据
    else if (field_meta->type() == AttrType::VECTORS) {
      const int  schema_dim = field_meta->vector_length();
      const bool vector_lob =
          (schema_dim > 1000) ||
          (schema_dim <= 0 && field_meta->len() == static_cast<int>(sizeof(LobRef)));
      if (vector_lob) {
        // Decode LobRef
        const char *p = this->record_->data() + field_meta->offset();
        LobRef ref;
        memcpy(&ref, p, sizeof(LobRef));

        const int32_t length = ref.length;
        const int64_t offset = ref.offset;

        if (length <= 0 || (length % sizeof(float)) != 0) {
          // empty or invalid vector
          cell.set_type(AttrType::VECTORS);
          cell.set_data("", 0);
          return RC::SUCCESS;
        }
        if (table_ == nullptr || table_->lob_handler() == nullptr) {
          return RC::INTERNAL;
        }
        std::vector<char> buf;
        buf.resize(static_cast<size_t>(length));
        RC rc = table_->lob_handler()->get_data(offset, length, buf.data());
        if (rc != RC::SUCCESS) {
          return rc;
        }
        cell.set_type(AttrType::VECTORS);
        cell.set_data(buf.data(), length);
        return RC::SUCCESS;
      }
      // 向量字段也可能以内联方式存储，此时直接从记录里取定长数据
      int data_len = field_meta->len();
      if (data_len <= 0 && schema_dim > 0) {
        data_len = schema_dim * sizeof(float);
      }
      if (data_len <= 0 || (data_len % static_cast<int>(sizeof(float))) != 0) {
        cell.set_type(AttrType::VECTORS);
        cell.set_data("", 0);
        return RC::SUCCESS;
      }
      cell.set_type(AttrType::VECTORS);
      cell.set_data(this->record_->data() + field_meta->offset(), data_len);
      return RC::SUCCESS;
    } else {
      cell.set_type(field_meta->type());
      cell.set_data(this->record_->data() + field_meta->offset(), field_meta->len());
      return RC::SUCCESS;
    }
  }

  RC spec_at(int index, TupleCellSpec &spec) const override
  {
    const Field &field = speces_[index]->field();
    const char *qualifier = alias_.empty() ? table_->name() : alias_.c_str();
    spec                  = TupleCellSpec(qualifier, field.field_name());
    return RC::SUCCESS;
  }

  RC find_cell(const TupleCellSpec &spec, Value &cell) const override
  {
    const char *table_name = spec.table_name();
    const char *field_name = spec.field_name();
    auto matches_physical = [&]() {
      return table_name != nullptr && 0 == strcasecmp(table_name, table_->name());
    };
    auto matches_alias = [&]() {
      return !alias_.empty() && table_name != nullptr && 0 == strcasecmp(table_name, alias_.c_str());
    };

    // 首先按表名或别名匹配
    if (matches_physical() || matches_alias()) {
      std::string target_name;
      if (field_name != nullptr && field_name[0] != '\0') {
        target_name = field_name;
      } else {
        const char *alias = spec.alias();
        if (alias != nullptr && alias[0] != '\0') {
          const char *dot = strrchr(alias, '.');
          if (dot != nullptr && *(dot + 1) != '\0') {
            target_name.assign(dot + 1);
          } else {
            target_name.assign(alias);
          }
        }
      }

      if (target_name.empty()) {
        return RC::NOTFOUND;
      }

      for (size_t i = 0; i < speces_.size(); ++i) {
        const FieldExpr *field_expr = speces_[i];
        const Field     &field      = field_expr->field();
        if (0 == strcasecmp(target_name.c_str(), field.field_name())) {
          return cell_at(i, cell);
        }
      }
      return RC::NOTFOUND;
    }

    // 无限定表名时的兜底逻辑，允许通过列名匹配
    if (table_name == nullptr || table_name[0] == '\0') {
      std::string fld;
      if (field_name != nullptr && field_name[0] != '\0') {
        fld = field_name;
      } else {
        const char *alias = spec.alias();
        if (alias != nullptr && alias[0] != '\0') {
          const char *dot = strrchr(alias, '.');
          if (dot != nullptr && *(dot + 1) != '\0') {
            fld.assign(dot + 1);
          } else {
            fld.assign(alias);
          }
        }
      }

      if (!fld.empty()) {
        int match_index = -1;
        for (size_t i = 0; i < speces_.size(); ++i) {
          const FieldExpr *field_expr = speces_[i];
          const Field     &field      = field_expr->field();
          if (0 == strcasecmp(fld.c_str(), field.field_name())) {
            if (match_index == -1) {
              match_index = static_cast<int>(i);
            } else {
              // 多个同名列，无法唯一定位
              return RC::NOTFOUND;
            }
          }
        }
        if (match_index != -1) {
          return cell_at(match_index, cell);
        }
      }
    }

    return RC::NOTFOUND;
  }

#if 0
  RC cell_spec_at(int index, const TupleCellSpec *&spec) const override
  {
    if (index < 0 || index >= static_cast<int>(speces_.size())) {
      LOG_WARN("invalid argument. index=%d", index);
      return RC::INVALID_ARGUMENT;
    }
    spec = speces_[index];
    return RC::SUCCESS;
  }
#endif

  Record &record() { return *record_; }

  const Record &record() const { return *record_; }

  const Table *table() const { return table_; }

private:
  Record             *record_ = nullptr;
  const Table        *table_  = nullptr;
  vector<FieldExpr *> speces_;
  std::string         alias_;
};

/**
 * @brief 从一行数据中，选择部分字段组成的元组，也就是投影操作
 * @ingroup Tuple
 * @details 一般在select语句中使用。
 * 投影也可以是很复杂的操作，比如某些字段需要做类型转换、重命名、表达式运算、函数计算等。
 */
class ProjectTuple : public Tuple
{
public:
  ProjectTuple()          = default;
  virtual ~ProjectTuple() = default;

  void set_expressions(vector<unique_ptr<Expression>> &&expressions) { expressions_ = std::move(expressions); }

  auto get_expressions() const -> const vector<unique_ptr<Expression>> & { return expressions_; }

  void set_tuple(Tuple *tuple) { this->tuple_ = tuple; }

  Tuple *tuple() { return tuple_; }
  const Tuple *tuple() const { return tuple_; }

  int cell_num() const override { return static_cast<int>(expressions_.size()); }

  RC cell_at(int index, Value &cell) const override
  {
    if (index < 0 || index >= cell_num()) {
      return RC::INTERNAL;
    }
    if (tuple_ == nullptr) {
      return RC::INTERNAL;
    }

    Expression *expr = expressions_[index].get();
    return expr->get_value(*tuple_, cell);
  }

  RC spec_at(int index, TupleCellSpec &spec) const override
  {
    spec = TupleCellSpec(expressions_[index]->name());
    return RC::SUCCESS;
  }

  RC find_cell(const TupleCellSpec &spec, Value &cell) const override { return tuple_->find_cell(spec, cell); }

#if 0
  RC cell_spec_at(int index, const TupleCellSpec *&spec) const override
  {
    if (index < 0 || index >= static_cast<int>(speces_.size())) {
      return RC::NOTFOUND;
    }
    spec = speces_[index];
    return RC::SUCCESS;
  }
#endif
private:
  vector<unique_ptr<Expression>> expressions_;
  Tuple                         *tuple_ = nullptr;
};

/**
 * @brief 一些常量值组成的Tuple
 * @ingroup Tuple
 * TODO 使用单独文件
 */
class ValueListTuple : public Tuple
{
public:
  ValueListTuple()          = default;
  virtual ~ValueListTuple() = default;

  void set_names(const vector<TupleCellSpec> &specs) { specs_ = specs; shared_specs_.reset(); }

  void set_shared_specs(const std::shared_ptr<vector<TupleCellSpec>> &specs)
  {
    shared_specs_ = specs;
    specs_.clear();
  }

  const std::shared_ptr<vector<TupleCellSpec>> &shared_specs() const { return shared_specs_; }
  void set_cells(const vector<Value> &cells) { cells_ = cells; }
  void set_cells(vector<Value> &&cells) { cells_ = std::move(cells); }

  virtual int cell_num() const override { return static_cast<int>(cells_.size()); }

  virtual RC cell_at(int index, Value &cell) const override
  {
    if (index < 0 || index >= cell_num()) {
      return RC::NOTFOUND;
    }

    cell = cells_[index];
    return RC::SUCCESS;
  }

  RC spec_at(int index, TupleCellSpec &spec) const override
  {
    if (index < 0 || index >= cell_num()) {
      return RC::NOTFOUND;
    }

    if (shared_specs_) {
      spec = (*shared_specs_)[index];
    } else {
      spec = specs_[index];
    }
    return RC::SUCCESS;
  }

  virtual RC find_cell(const TupleCellSpec &spec, Value &cell) const override
  {
    const vector<TupleCellSpec> *specs_holder = nullptr;
    if (shared_specs_) {
      specs_holder = shared_specs_.get();
    } else {
      specs_holder = &specs_;
    }

    ASSERT(cells_.size() == specs_holder->size(), "cells_.size()=%d, specs_.size()=%d", cells_.size(), specs_holder->size());

    const int size = static_cast<int>(specs_holder->size());
    for (int i = 0; i < size; i++) {
      if ((*specs_holder)[i].equals(spec)) {
        cell = cells_[i];
        return RC::SUCCESS;
      }
    }
    return RC::NOTFOUND;
  }

  static RC make(const Tuple &tuple, ValueListTuple &value_list)
  {
    value_list.cells_.clear();
    const bool use_shared_specs = static_cast<bool>(value_list.shared_specs_);
    if (!use_shared_specs) {
      value_list.specs_.clear();
    }

    const int cell_num = tuple.cell_num();
    value_list.cells_.reserve(cell_num);
    if (!use_shared_specs) {
      value_list.specs_.reserve(cell_num);
    }
    for (int i = 0; i < cell_num; i++) {
      Value cell;
      RC    rc = tuple.cell_at(i, cell);
      if (OB_FAIL(rc)) {
        return rc;
      }

      value_list.cells_.push_back(cell);
      if (!use_shared_specs) {
        // 仅在没有共享列定义时才复制元信息，避免大量重复的字符串分配
        TupleCellSpec spec;
        rc = tuple.spec_at(i, spec);
        if (OB_FAIL(rc)) {
          return rc;
        }
        value_list.specs_.push_back(std::move(spec));
      }
    }
    return RC::SUCCESS;
  }

private:
  vector<Value>                         cells_;
  vector<TupleCellSpec>                 specs_;
  std::shared_ptr<vector<TupleCellSpec>> shared_specs_;
};

class JoinedTuple : public Tuple
{
public:
  JoinedTuple()          = default;
  virtual ~JoinedTuple() = default;

  void set_left(Tuple *left) { left_ = left; }
  void set_right(Tuple *right) { right_ = right; }

  int cell_num() const override { return left_->cell_num() + right_->cell_num(); }

  RC cell_at(int index, Value &value) const override
  {
    const int left_cell_num = left_->cell_num();
    if (index >= 0 && index < left_cell_num) {
      return left_->cell_at(index, value);
    }

    if (index >= left_cell_num && index < left_cell_num + right_->cell_num()) {
      return right_->cell_at(index - left_cell_num, value);
    }

    return RC::NOTFOUND;
  }

  RC spec_at(int index, TupleCellSpec &spec) const override
  {
    const int left_cell_num = left_->cell_num();
    if (index >= 0 && index < left_cell_num) {
      return left_->spec_at(index, spec);
    }

    if (index >= left_cell_num && index < left_cell_num + right_->cell_num()) {
      return right_->spec_at(index - left_cell_num, spec);
    }

    return RC::NOTFOUND;
  }

  RC find_cell(const TupleCellSpec &spec, Value &value) const override
  {
    RC rc = left_->find_cell(spec, value);
    if (rc == RC::SUCCESS || rc != RC::NOTFOUND) {
      return rc;
    }

    return right_->find_cell(spec, value);
  }

private:
  Tuple *left_  = nullptr;
  Tuple *right_ = nullptr;
};
