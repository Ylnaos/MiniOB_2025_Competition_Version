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
// Created by Wangyunlai on 2022/5/22.
//

#pragma once

#include "common/sys/rc.h"
#include "sql/stmt/stmt.h"
#include "sql/stmt/filter_stmt.h"
#include "sql/expr/expression.h"

class Table;
class FieldMeta;

/**
 * @brief 更新语句
 * @ingroup Statement
 */
class UpdateStmt : public Stmt
{
public:
  UpdateStmt() = default;
  UpdateStmt(Table *table, const vector<const FieldMeta *> &field_metas,
             const vector<Value> &values, const vector<Expression *> &value_expressions,
             FilterStmt *filter_stmt);
  ~UpdateStmt() override;

public:
  static RC create(Db *db, const UpdateSqlNode &update_sql, Stmt *&stmt);

public:
  StmtType type() const override { return StmtType::UPDATE; }

  Table                       *table() const { return table_; }
  const vector<const FieldMeta *> &field_metas() const { return field_metas_; }
  const vector<Value>         &values() const { return values_; }
  const vector<Expression *>  &value_expressions() const { return value_expressions_; }
  FilterStmt                  *filter_stmt() const { return filter_stmt_; }

private:
  Table                    *table_       = nullptr;
  vector<const FieldMeta *> field_metas_;
  vector<Value>             values_;
  vector<Expression *>      value_expressions_;
  FilterStmt               *filter_stmt_ = nullptr;
};
