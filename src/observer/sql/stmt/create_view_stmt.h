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

/**
 * @brief 表示 CREATE VIEW 语句
 */
class CreateViewStmt : public Stmt
{
public:
  CreateViewStmt() = default;
  ~CreateViewStmt() override = default;

  StmtType type() const override { return StmtType::CREATE_VIEW; }

public:
  static RC create(Db *db, const CreateViewSqlNode &create_view, Stmt *&stmt);

  const std::string &view_name() const { return view_name_; }
  const std::string &view_select_sql() const { return view_select_sql_; }
  const std::vector<std::string> &view_fields() const { return view_fields_; }

private:
  std::string view_name_;
  std::string view_select_sql_;
  std::vector<std::string> view_fields_;
};

