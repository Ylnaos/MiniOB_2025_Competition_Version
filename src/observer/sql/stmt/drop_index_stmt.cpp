/* Copyright (c) 2021 OceanBase and/or its affiliates.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Codex Agent on 2025/2/15.
//

#include "sql/stmt/drop_index_stmt.h"

#include "common/lang/string.h"
#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/index/index.h"

using namespace std;
using namespace common;

RC DropIndexStmt::create(Db *db, const DropIndexSqlNode &drop_index, Stmt *&stmt)
{
  stmt = nullptr;

  if (db == nullptr) {
    LOG_WARN("null database pointer while creating drop index stmt");
    return RC::INVALID_ARGUMENT;
  }

  const char *table_name = drop_index.relation_name.c_str();
  const char *index_name = drop_index.index_name.c_str();

  if (is_blank(table_name) || is_blank(index_name)) {
    LOG_WARN("invalid arguments for drop index. table=%s index=%s", table_name, index_name);
    return RC::INVALID_ARGUMENT;
  }

  Table *table = db->find_table(table_name);
  if (nullptr == table) {
    if (drop_index.if_exists) {
      // IF EXISTS: 表不存在时也视为成功（创建一个空的 stmt）
      stmt = new DropIndexStmt(nullptr, drop_index.index_name, true);
      return RC::SUCCESS;
    }
    LOG_WARN("no such table. db=%s table=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  Index *index = table->find_index(index_name);
  if (nullptr == index) {
    if (drop_index.if_exists) {
      // IF EXISTS: 索引不存在时也视为成功
      stmt = new DropIndexStmt(table, drop_index.index_name, true);
      return RC::SUCCESS;
    }
    LOG_WARN("index not exists on table. table=%s index=%s", table_name, index_name);
    return RC::NOT_EXIST;
  }

  stmt = new DropIndexStmt(table, drop_index.index_name, drop_index.if_exists);
  return RC::SUCCESS;
}

