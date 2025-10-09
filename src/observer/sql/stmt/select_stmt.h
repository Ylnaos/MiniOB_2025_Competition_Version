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
// Created by Wangyunlai on 2022/6/5.
//

#pragma once

#include "common/sys/rc.h"
#include "sql/stmt/stmt.h"
#include "storage/field/field.h"

class FieldMeta;
class FilterStmt;
class Db;
class Table;

/**
 * @brief 表示select语句
 * @ingroup Statement
 */
class SelectStmt : public Stmt
{
public:
  SelectStmt() = default;
  ~SelectStmt() override;

  StmtType type() const override { return StmtType::SELECT; }

public:
  static RC create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt);

public:
  const vector<Table *> &tables() const { return tables_; }
  FilterStmt            *filter_stmt() const { return filter_stmt_; }
  unique_ptr<Expression> &where_expr() { return where_expr_; }
  // HAVING 表达式（AND 链接后的整体表达式）；为空表示无 HAVING
  unique_ptr<Expression> &having_expr() { return having_expr_; }

  vector<unique_ptr<Expression>> &query_expressions() { return query_expressions_; }
  vector<unique_ptr<Expression>> &group_by() { return group_by_; }
  vector<pair<unique_ptr<Expression>, bool>> &order_by() { return order_by_; }
  int limit() const { return limit_; }

private:
  vector<unique_ptr<Expression>> query_expressions_;
  vector<Table *>                tables_;
  FilterStmt                    *filter_stmt_ = nullptr;
  vector<unique_ptr<Expression>> group_by_;
  vector<pair<unique_ptr<Expression>, bool>> order_by_;
  unique_ptr<Expression>         where_expr_;
  unique_ptr<Expression>         having_expr_;
  int                            limit_ = -1;  // LIMIT数量，-1表示无限制
};
