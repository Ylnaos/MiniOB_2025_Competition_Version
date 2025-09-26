/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/stmt/create_table_as_select_stmt.h"
#include "common/log/log.h"
#include "storage/db/db.h"

RC CreateTableAsSelectStmt::create(Db *db, const ParsedSqlNode::CreateTableAsSelectSqlNode &node, Stmt *&stmt)
{
  stmt = nullptr;
  if (db == nullptr || node.select_node == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  if (node.select_node->flag != SCF_SELECT) {
    LOG_WARN("CTAS inner node is not SELECT");
    return RC::SQL_SYNTAX;
  }

  // Build a SelectStmt from parsed SELECT node
  Stmt *inner_stmt = nullptr;
  RC rc = SelectStmt::create(db, node.select_node->selection, inner_stmt);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create inner select stmt for CTAS: %s", strrc(rc));
    return rc;
  }

  stmt = new CreateTableAsSelectStmt(node.table_name, static_cast<SelectStmt *>(inner_stmt));
  return RC::SUCCESS;
}

