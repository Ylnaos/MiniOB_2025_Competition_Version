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
#include "sql/stmt/select_stmt.h"

class Db;

/**
 * @brief CREATE TABLE ... AS SELECT ... statement
 */
class CreateTableAsSelectStmt : public Stmt
{
public:
  CreateTableAsSelectStmt(const string &table_name, SelectStmt *select_stmt)
      : table_name_(table_name), select_stmt_(select_stmt)
  {}

  ~CreateTableAsSelectStmt() override { delete select_stmt_; }

  StmtType type() const override { return StmtType::CREATE_TABLE_AS_SELECT; }

  const string   &table_name() const { return table_name_; }
  SelectStmt     *select_stmt() const { return select_stmt_; }

  static RC create(Db *db, const ParsedSqlNode::CreateTableAsSelectSqlNode &node, Stmt *&stmt);

private:
  string     table_name_;
  SelectStmt *select_stmt_ = nullptr;  ///< owned
};

