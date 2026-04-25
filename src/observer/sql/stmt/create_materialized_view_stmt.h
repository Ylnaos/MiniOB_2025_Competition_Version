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
 * @brief 表示 CREATE MATERIALIZED VIEW 语句
 */
class CreateMaterializedViewStmt : public Stmt
{
public:
  CreateMaterializedViewStmt() = default;
  ~CreateMaterializedViewStmt() override = default;

  StmtType type() const override { return StmtType::CREATE_MATERIALIZED_VIEW; }

public:
  static RC create(Db *db, const CreateMaterializedViewSqlNode &create_view, Stmt *&stmt);

  const std::string &view_name() const { return view_name_; }
  SelectStmt *select_stmt() const { return select_stmt_.get(); }

private:
  std::string view_name_;
  std::unique_ptr<SelectStmt> select_stmt_;
};
