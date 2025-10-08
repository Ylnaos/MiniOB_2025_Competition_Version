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
// Created by Wangyunlai on 2023/4/25.
//

#pragma once

#include "sql/stmt/stmt.h"

struct CreateIndexSqlNode;
class Table;
class FieldMeta;

/**
 * @brief 创建索引的语句
 * @ingroup Statement
 */
class CreateIndexStmt : public Stmt
{
public:
  CreateIndexStmt(Table *table, vector<const FieldMeta *> field_metas, const string &index_name, bool unique,
                  bool is_vector_index = false, const string &distance_type = "", const string &index_type = "",
                  int lists = 0, int probes = 0)
      : table_(table), field_metas_(std::move(field_metas)), index_name_(index_name), unique_(unique),
        is_vector_index_(is_vector_index), distance_type_(distance_type), index_type_(index_type),
        lists_(lists), probes_(probes)
  {}

  virtual ~CreateIndexStmt() = default;

  StmtType type() const override { return StmtType::CREATE_INDEX; }

  Table                          *table() const { return table_; }
  const vector<const FieldMeta *> &field_metas() const { return field_metas_; }
  const string                   &index_name() const { return index_name_; }
  bool                            unique() const { return unique_; }

  // Vector index specific accessors
  bool                            is_vector_index() const { return is_vector_index_; }
  const string                   &distance_type() const { return distance_type_; }
  const string                   &index_type() const { return index_type_; }
  int                             lists() const { return lists_; }
  int                             probes() const { return probes_; }

public:
  static RC create(Db *db, const CreateIndexSqlNode &create_index, Stmt *&stmt);

private:
  Table                          *table_ = nullptr;
  vector<const FieldMeta *>       field_metas_;
  string                          index_name_;
  bool                            unique_ = false;

  // Vector index specific members
  bool                            is_vector_index_ = false;
  string                          distance_type_;
  string                          index_type_;
  int                             lists_ = 0;
  int                             probes_ = 0;
};
