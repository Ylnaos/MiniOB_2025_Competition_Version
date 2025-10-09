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

  // 生成语句对象
  auto *view_stmt = new CreateViewStmt();
  view_stmt->view_name_       = create_view.view_name;
  view_stmt->view_select_sql_ = create_view.view_select_sql;
  view_stmt->view_fields_     = create_view.view_fields;
  stmt                        = view_stmt;
  return RC::SUCCESS;
}

