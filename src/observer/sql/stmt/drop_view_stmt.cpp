/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/stmt/drop_view_stmt.h"
#include "storage/db/db.h"
#include "storage/view/view.h"

RC DropViewStmt::create(Db *db, const DropViewSqlNode &drop_view, Stmt *&stmt)
{
  // 验证视图是否存在
  View *view = db->find_view(drop_view.view_name.c_str());
  if (view == nullptr) {
    if (drop_view.if_exists) {
      // IF EXISTS: 视图不存在时也视为成功
      stmt = new DropViewStmt(drop_view.view_name, true);
      return RC::SUCCESS;
    }
    return RC::SCHEMA_TABLE_NOT_EXIST;  // 使用通用的不存在错误码
  }

  stmt = new DropViewStmt(drop_view.view_name, drop_view.if_exists);
  return RC::SUCCESS;
}
