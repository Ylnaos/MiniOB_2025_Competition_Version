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

#include "sql/stmt/stmt.h"

class Db;

class AlterTableStmt : public Stmt
{
public:
  enum class AlterType
  {
    ADD_COLUMN,
    DROP_COLUMN,
    CHANGE_COLUMN,
    RENAME_TABLE
  };

  AlterTableStmt(string table_name,
                 AlterType alter_type,
                 AttrInfoSqlNode column_info,
                 string target_column,
                 string new_column_name,
                 string new_table_name)
    : table_name_(std::move(table_name)),
      alter_type_(alter_type),
      column_info_(std::move(column_info)),
      target_column_(std::move(target_column)),
      new_column_name_(std::move(new_column_name)),
      new_table_name_(std::move(new_table_name))
  {}

  StmtType type() const override { return StmtType::ALTER_TABLE; }

  AlterType alter_type() const { return alter_type_; }
  const string &table_name() const { return table_name_; }
  const AttrInfoSqlNode &column_info() const { return column_info_; }
  const string &target_column() const { return target_column_; }
  const string &new_column_name() const { return new_column_name_; }
  const string &new_table_name() const { return new_table_name_; }

  static RC create(Db *db, const AlterTableSqlNode &alter_table, Stmt *&stmt);

private:
  string           table_name_;
  AlterType        alter_type_;
  AttrInfoSqlNode  column_info_;
  string           target_column_;
  string           new_column_name_;
  string           new_table_name_;
};
