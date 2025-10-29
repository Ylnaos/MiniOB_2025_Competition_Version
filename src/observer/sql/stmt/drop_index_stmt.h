/* Copyright (c) 2021 OceanBase and/or its affiliates.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Codex Agent on 2025/2/15.
//

#pragma once

#include <string>

#include "sql/stmt/stmt.h"

struct DropIndexSqlNode;
class Table;

/**
 * @brief DROP INDEX 语句
 */
class DropIndexStmt : public Stmt
{
public:
  DropIndexStmt(Table *table, const std::string &index_name, bool if_exists = false)
    : table_(table), index_name_(index_name), if_exists_(if_exists) {}
  virtual ~DropIndexStmt() = default;

  StmtType type() const override { return StmtType::DROP_INDEX; }

  Table              *table() const { return table_; }
  const std::string  &index_name() const { return index_name_; }
  bool                if_exists() const { return if_exists_; }

  static RC create(Db *db, const DropIndexSqlNode &drop_index, Stmt *&stmt);

private:
  Table             *table_ = nullptr;
  std::string        index_name_;
  bool               if_exists_ = false;
};

