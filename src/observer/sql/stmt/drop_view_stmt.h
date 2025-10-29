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

#include "common/lang/string.h"
#include "sql/stmt/stmt.h"

class Db;

/**
 * @brief 表示删除视图的语句
 * @ingroup Statement
 */
class DropViewStmt : public Stmt
{
public:
  DropViewStmt(const string &view_name, bool if_exists = false)
    : view_name_(view_name), if_exists_(if_exists) {}
  virtual ~DropViewStmt() = default;

  StmtType type() const override { return StmtType::DROP_VIEW; }

  const string &view_name() const { return view_name_; }
  bool          if_exists() const { return if_exists_; }

  static RC create(Db *db, const DropViewSqlNode &drop_view, Stmt *&stmt);

private:
  string view_name_;
  bool   if_exists_;
};
