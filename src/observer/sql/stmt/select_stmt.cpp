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
#include "common/lang/string.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/parser/expression_binder.h"
#include "sql/parser/parse.h"
#include "storage/view/view.h"

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

  // 简单的视图展开：仅支持单视图、且外层为 `SELECT * FROM view` 无其它 WHERE/ORDER/GROUP 的场景
  if (select_sql.relations.size() == 1) {
    const RelationSqlNode &rel = select_sql.relations[0];
    const char *rel_name = rel.relation_name.c_str();
    if (db->find_table(rel_name) == nullptr) {
      View *view = db->find_view(rel_name);
      if (view != nullptr) {
        bool only_star = (select_sql.expressions.size() == 1) &&
                         (select_sql.expressions[0] != nullptr) &&
                         (select_sql.expressions[0]->type() == ExprType::STAR);
        bool no_outer_filters = select_sql.conditions.empty() &&
                                select_sql.group_by.empty() &&
                                select_sql.order_by.empty() &&
                                rel.alias.empty();
        if (!only_star || !no_outer_filters) {
          LOG_WARN("current view usage is limited to: SELECT * FROM view_name");
          return RC::UNSUPPORTED;
        }

        ParsedSqlResult parsed;
        RC parse_rc = parse(view->select_sql(), &parsed);
        if (OB_FAIL(parse_rc) || parsed.sql_nodes().empty()) {
          LOG_WARN("parse view select failed. view=%s", view->name());
          return RC::SQL_SYNTAX;
        }
        ParsedSqlNode *node = parsed.sql_nodes()[0].get();
        if (node->flag != SCF_SELECT) {
          LOG_WARN("view definition is not a SELECT. view=%s", view->name());
          return RC::SQL_SYNTAX;
        }

        // 用视图的SELECT定义替换外层SELECT
        select_sql.expressions.swap(node->selection.expressions);
        select_sql.relations.swap(node->selection.relations);
        select_sql.conditions.swap(node->selection.conditions);
        select_sql.group_by.swap(node->selection.group_by);
        select_sql.order_by.swap(node->selection.order_by);
      }
    }
  }

  BinderContext binder_context;

  // collect tables in `from` statement
  vector<Table *>                tables;
  unordered_map<string, Table *> table_map;
  for (size_t i = 0; i < select_sql.relations.size(); i++) {
    const RelationSqlNode &rel = select_sql.relations[i];
    const char *table_name = rel.relation_name.c_str();
    if (nullptr == table_name) {
      LOG_WARN("invalid argument. relation name is null. index=%d", i);
      return RC::INVALID_ARGUMENT;
    }

    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    binder_context.add_table(table);
    tables.push_back(table);
    table_map.insert({table_name, table});
    // 别名检查：同层不重复
    if (!rel.alias.empty()) {
      if (table_map.find(rel.alias) != table_map.end()) {
        LOG_WARN("duplicate table alias in same scope: %s", rel.alias.c_str());
        return RC::INVALID_ARGUMENT;
      }
      table_map.insert({rel.alias, table});
      binder_context.add_alias(rel.alias, table);
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
  // 传递 LIMIT
  select_stmt->set_limit(select_sql.limit);
  stmt                      = select_stmt;
  return RC::SUCCESS;
}
