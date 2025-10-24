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
#include "common/lang/unordered_map.h"
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
  const vector<string> &table_aliases() const { return table_aliases_; }
  FilterStmt            *filter_stmt() const { return filter_stmt_; }
  unique_ptr<Expression> &where_expr() { return where_expr_; }
  // HAVING 表达式（AND 链接后的整体表达式）；为空表示无 HAVING
  unique_ptr<Expression> &having_expr() { return having_expr_; }

  vector<unique_ptr<Expression>> &query_expressions() { return query_expressions_; }
  vector<unique_ptr<Expression>> &group_by() { return group_by_; }
  vector<pair<unique_ptr<Expression>, bool>> &order_by() { return order_by_; }
  int limit() const { return limit_; }
  struct SetOperation
  {
    SetOperatorType type = SetOperatorType::UNION;
    unique_ptr<SelectStmt> stmt;
  };
  const vector<SetOperation> &set_operations() const { return set_operations_; }

  // 视图相关:当FROM是一个包含聚合的视图时,inner_view_stmt_保存视图的查询
  SelectStmt *inner_view_stmt() const { return inner_view_stmt_; }
  void set_inner_view_stmt(SelectStmt *stmt) { inner_view_stmt_ = stmt; }

  // 派生表（视图作为表使用）：别名 -> SelectStmt（拥有所有权）
  void add_derived_table_stmt(const string &alias, SelectStmt *stmt) {
    if (!alias.empty() && stmt != nullptr) {
      derived_table_stmts_[alias].reset(stmt);
    }
  }

  const unordered_map<string, unique_ptr<SelectStmt>>& derived_table_stmts() const { return derived_table_stmts_; }

private:
  vector<unique_ptr<Expression>> query_expressions_;
  vector<Table *>                tables_;
  vector<string>                 table_aliases_;
  FilterStmt                    *filter_stmt_ = nullptr;
  vector<unique_ptr<Expression>> group_by_;
  vector<pair<unique_ptr<Expression>, bool>> order_by_;
  int                            limit_ = -1;  ///< LIMIT限制（-1表示无限制）
  unique_ptr<Expression>         where_expr_;
  unique_ptr<Expression>         having_expr_;
  SelectStmt                    *inner_view_stmt_ = nullptr;  // 内层视图查询(包含聚合时使用)
  vector<SetOperation>           set_operations_;
  unordered_map<string, unique_ptr<SelectStmt>> derived_table_stmts_;  // 派生表（视图作为表）
};
