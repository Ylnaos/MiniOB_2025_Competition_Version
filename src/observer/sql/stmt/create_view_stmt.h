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
// Minimal Create View Stmt
//

#pragma once

#include "common/lang/memory.h"
#include "common/lang/string.h"
#include "sql/parser/parse_defs.h"
#include "sql/stmt/stmt.h"

class Db;

class CreateViewStmt : public Stmt {
public:
  CreateViewStmt(string view_name, std::unique_ptr<ParsedSqlNode> select_node)
    : view_name_(std::move(view_name)), select_node_(std::move(select_node)) {}
  virtual ~CreateViewStmt() = default;

  StmtType type() const override { return StmtType::CREATE_VIEW; }

  const string &view_name() const { return view_name_; }
  ParsedSqlNode *select_node() const { return select_node_.get(); }
  std::unique_ptr<ParsedSqlNode> release_select_node() { return std::move(select_node_); }

  static RC create(Db *db, CreateViewSqlNode &create_view, Stmt *&stmt);

private:
  string view_name_;
  std::unique_ptr<ParsedSqlNode> select_node_;
};
