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
// Minimal Create View Stmt
//

#include "sql/stmt/create_view_stmt.h"
#include "common/log/log.h"

RC CreateViewStmt::create(Db *db, CreateViewSqlNode &create_view, Stmt *&stmt)
{
  if (!create_view.select_node || create_view.select_node->flag != SCF_SELECT) {
    return RC::INVALID_ARGUMENT;
  }
  // Transfer ownership of the parsed SELECT node into the statement
  stmt = new CreateViewStmt(create_view.view_name, std::unique_ptr<ParsedSqlNode>(create_view.select_node.release()));
  return RC::SUCCESS;
}
