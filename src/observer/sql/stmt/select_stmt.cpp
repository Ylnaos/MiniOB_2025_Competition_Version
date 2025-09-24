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
// Created by Wangyunlai on 2022/6/6.
//

#include "sql/stmt/select_stmt.h"
#include <sstream>
#include "common/lang/string.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/parser/expression_binder.h"

using namespace std;
using namespace common;

SelectStmt::~SelectStmt()
{
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

RC SelectStmt::create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }

  BinderContext binder_context;

  // collect tables in `from` statement
  vector<Table *>                tables;
  unordered_map<string, Table *> table_map;
  for (size_t i = 0; i < select_sql.relations.size(); i++) {
    // 支持 FROM 表别名：relation 可能为 "table" 或 "table alias"
    const std::string &relation_item = select_sql.relations[i];
    std::string        base_table_name;
    std::string        alias_name;
    {
      std::istringstream iss(relation_item);
      iss >> base_table_name;
      std::string maybe_alias;
      if (iss >> maybe_alias) {
        alias_name = maybe_alias; // 仅支持简单的 "table alias" 形式
      }
    }
    const char *table_name = base_table_name.c_str();
    if (base_table_name.empty()) {
      LOG_WARN("invalid argument. relation name is null. index=%d", i);
      return RC::INVALID_ARGUMENT;
    }

    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    binder_context.add_table(table);
    if (!alias_name.empty()) {
      binder_context.add_table_alias(alias_name, table);
    }
    tables.push_back(table);
    table_map.insert({base_table_name, table});
    if (!alias_name.empty()) {
      table_map.insert({alias_name, table});
    }
  }

  // collect query fields in `select` statement
  vector<unique_ptr<Expression>> bound_expressions;
  ExpressionBinder expression_binder(binder_context);
  
  for (unique_ptr<Expression> &expression : select_sql.expressions) {
    RC rc = expression_binder.bind_expression(expression, bound_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  vector<unique_ptr<Expression>> group_by_expressions;
  for (unique_ptr<Expression> &expression : select_sql.group_by) {
    RC rc = expression_binder.bind_expression(expression, group_by_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  // bind order by expressions
  vector<pair<unique_ptr<Expression>, bool>> order_by_items;
  for (auto &item : select_sql.order_by) {
    vector<unique_ptr<Expression>> bound;
    RC rc = expression_binder.bind_expression(item.expression, bound);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind order by expression failed. rc=%s", strrc(rc));
      return rc;
    }
    if (bound.size() != 1) {
      LOG_WARN("invalid order by expression size: %d", bound.size());
      return RC::INVALID_ARGUMENT;
    }
    order_by_items.emplace_back(std::move(bound[0]), item.asc);
  }

  Table *default_table = nullptr;
  if (tables.size() == 1) {
    default_table = tables[0];
  }

  // create filter statement in `where` statement
  FilterStmt *filter_stmt = nullptr;
  RC          rc          = FilterStmt::create(db,
      default_table,
      &table_map,
      select_sql.conditions.data(),
      static_cast<int>(select_sql.conditions.size()),
      filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("cannot construct filter stmt");
    return rc;
  }

  // everything alright
  SelectStmt *select_stmt = new SelectStmt();

  select_stmt->tables_.swap(tables);
  select_stmt->query_expressions_.swap(bound_expressions);
  select_stmt->filter_stmt_ = filter_stmt;
  select_stmt->group_by_.swap(group_by_expressions);
  select_stmt->order_by_.swap(order_by_items);
  stmt                      = select_stmt;
  return RC::SUCCESS;
}
