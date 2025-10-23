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
// Created by Wangyunlai on 2022/5/22.
//

#include "sql/stmt/delete_stmt.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/view/view.h"
#include "sql/parser/parse.h"

DeleteStmt::DeleteStmt(Table *table, FilterStmt *filter_stmt) : table_(table), filter_stmt_(filter_stmt) {}

DeleteStmt::~DeleteStmt()
{
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

RC DeleteStmt::create(Db *db, const DeleteSqlNode &delete_sql, Stmt *&stmt)
{
  const char *table_name = delete_sql.relation_name.c_str();
  if (nullptr == db || nullptr == table_name) {
    LOG_WARN("invalid argument. db=%p, table_name=%p", db, table_name);
    return RC::INVALID_ARGUMENT;
  }

  // check whether the table exists
  Table *table = db->find_table(table_name);
  if (nullptr == table) {
    // 尝试查找视图
    View *view = db->find_view(table_name);
    if (view != nullptr) {
      // 解析视图定义获取底层表
      ParsedSqlResult parsed;
      RC parse_rc = parse(view->select_sql(), &parsed);
      if (OB_FAIL(parse_rc) || parsed.sql_nodes().empty()) {
        LOG_WARN("failed to parse view definition for delete. view=%s", table_name);
        return RC::SQL_SYNTAX;
      }

      ParsedSqlNode *node = parsed.sql_nodes()[0].get();
      if (node->flag != SCF_SELECT) {
        LOG_WARN("view definition is not SELECT for delete. view=%s", table_name);
        return RC::UNSUPPORTED;
      }

      SelectSqlNode &select_node = node->selection;

      // 只支持单表简单视图的删除
      if (select_node.relations.size() != 1) {
        LOG_WARN("delete from multi-table view not supported. view=%s", table_name);
        return RC::UNSUPPORTED;
      }

      // 获取底层物理表
      const char *base_table_name = select_node.relations[0].relation_name.c_str();
      table = db->find_table(base_table_name);
      if (table == nullptr) {
        LOG_WARN("base table not found for view. view=%s, base_table=%s",
                 table_name, base_table_name);
        return RC::SCHEMA_TABLE_NOT_EXIST;
      }

      LOG_INFO("rewrite delete on view(%s) to base table(%s)", table_name, table->name());
    }

    if (table == nullptr) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
  }

  unordered_map<string, Table *> table_map;
  table_map.insert(pair<string, Table *>(string(table_name), table));

  FilterStmt *filter_stmt = nullptr;
  RC          rc          = FilterStmt::create(
      db, table, &table_map, delete_sql.conditions.data(), static_cast<int>(delete_sql.conditions.size()), filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create filter statement. rc=%d:%s", rc, strrc(rc));
    return rc;
  }

  stmt = new DeleteStmt(table, filter_stmt);
  return rc;
}
