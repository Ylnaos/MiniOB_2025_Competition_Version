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
// Created by Wangyunlai on 2021/5/12.
//

#pragma once

#include "common/sys/rc.h"
#include "common/lang/string.h"
#include "common/lang/span.h"

class TableMeta;
class FieldMeta;

namespace Json {
class Value;
}  // namespace Json

/**
 * @brief 描述一个索引
 * @ingroup Index
 * @details 一个索引包含了表的哪些字段，索引的名称等。
 * 如果以后实现了多种类型的索引，还需要记录索引的类型，对应类型的一些元数据等
 */
class IndexMeta
{
public:
  IndexMeta() = default;

  RC init(const char *name, span<const FieldMeta> fields, bool unique = false);

public:
  const char          *name() const;
  // 返回第一个字段名（兼容旧接口）
  const char          *field() const;
  const vector<string> &fields() const { return fields_; }
  bool                  unique() const { return unique_; }
  bool                  is_vector_index() const { return is_vector_index_; }
  bool                  is_full_text_index() const { return is_full_text_index_; }
  const string         &index_type() const { return index_type_; }
  const string         &distance_type() const { return distance_type_; }
  int                   lists() const { return lists_; }
  int                   probes() const { return probes_; }
  const string         &full_text_parser() const { return full_text_parser_; }

  void set_vector_options(bool is_vector_index,
                          const string &index_type,
                          const string &distance_type,
                          int lists,
                          int probes);
  void set_full_text_options(bool is_full_text_index, const string &parser_name);

  void desc(ostream &os) const;

public:
  void      to_json(Json::Value &json_value) const;
  static RC from_json(const TableMeta &table, const Json::Value &json_value, IndexMeta &index);

protected:
  string         name_;    // index's name
  vector<string> fields_;  // field names (ordered)
  bool           unique_ = false; // unique index flag
  bool           is_vector_index_ = false;
  bool           is_full_text_index_ = false;
  string         index_type_;
  string         distance_type_;
  string         full_text_parser_;
  int            lists_  = 0;
  int            probes_ = 0;
};
