/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/stmt/create_view_stmt.h"
#include "common/log/log.h"
#include "storage/db/db.h"
#include "sql/parser/parse.h"

RC CreateViewStmt::create(Db *db, const CreateViewSqlNode &create_view, Stmt *&stmt)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }
  if (create_view.view_name.empty() || create_view.view_select_sql.empty()) {
    LOG_WARN("invalid create view args");
    return RC::INVALID_ARGUMENT;
  }

  // 先做简单冲突检测：不允许与已有表同名
  if (db->find_table(create_view.view_name.c_str()) != nullptr) {
    LOG_WARN("view name conflicts with existing table: %s", create_view.view_name.c_str());
    return RC::SCHEMA_TABLE_EXIST;
  }

  // 验证视图定义的SQL语法是否正确
  ParsedSqlResult parsed;
  RC parse_rc = parse(create_view.view_select_sql.c_str(), &parsed);
  if (OB_FAIL(parse_rc) || parsed.sql_nodes().empty()) {
    LOG_WARN("parse view select failed. view=%s, sql=%s",
             create_view.view_name.c_str(), create_view.view_select_sql.c_str());
    return RC::SQL_SYNTAX;
  }

  ParsedSqlNode *node = parsed.sql_nodes()[0].get();
  if (node->flag != SCF_SELECT) {
    LOG_WARN("view definition is not a SELECT. view=%s", create_view.view_name.c_str());
    return RC::SQL_SYNTAX;
  }

  // 注意:这里不做语义验证(不调用SelectStmt::create),因为:
  // 1. 视图可能包含聚合函数,而聚合视图的展开逻辑比较复杂
  // 2. 表可能还不存在(创建视图时允许引用尚不存在的表,延迟到查询时检查)
  // 3. 语义验证应该在查询视图时进行

  // 生成语句对象
  auto *view_stmt = new CreateViewStmt();
  view_stmt->view_name_       = create_view.view_name;
  view_stmt->view_select_sql_ = create_view.view_select_sql;
  view_stmt->view_fields_     = create_view.view_fields;
  stmt                        = view_stmt;
  return RC::SUCCESS;
}

