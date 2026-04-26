/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/stmt/create_materialized_view_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "common/log/log.h"
#include "storage/db/db.h"

RC CreateMaterializedViewStmt::create(Db *db, const CreateMaterializedViewSqlNode &create_view, Stmt *&stmt)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }
  if (create_view.view_name.empty() || !create_view.select_node) {
    LOG_WARN("invalid create materialized view args");
    return RC::INVALID_ARGUMENT;
  }

  // 普通表不允许被物化视图覆盖；已有物化视图会在执行阶段重建。
  if (db->find_table(create_view.view_name.c_str()) != nullptr &&
      db->find_view(create_view.view_name.c_str()) == nullptr) {
    LOG_WARN("materialized view name conflicts with existing table: %s", create_view.view_name.c_str());
    return RC::SCHEMA_TABLE_EXIST;
  }

  // 解析 SELECT 子查询，创建 SelectStmt
  Stmt *select_stmt = nullptr;
  RC rc = SelectStmt::create(db, create_view.select_node->selection, select_stmt);
  if (rc != RC::SUCCESS || nullptr == select_stmt) {
    LOG_WARN("failed to create select stmt for materialized view. rc=%s", strrc(rc));
    return rc;
  }

  // 创建物化视图语句对象
  auto *view_stmt = new CreateMaterializedViewStmt();
  view_stmt->view_name_ = create_view.view_name;
  view_stmt->select_stmt_.reset(static_cast<SelectStmt *>(select_stmt));
  stmt = view_stmt;
  return RC::SUCCESS;
}
