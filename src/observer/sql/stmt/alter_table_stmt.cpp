/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/stmt/alter_table_stmt.h"

#include <strings.h>

#include "common/lang/string.h"
#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"

static bool contains_column(const vector<string> &columns, const string &target)
{
  for (const string &col : columns) {
    if (0 == strcasecmp(col.c_str(), target.c_str())) {
      return true;
    }
  }
  return false;
}

static AttrInfoSqlNode build_attr_info_from_field(const FieldMeta &field, const string &new_name)
{
  AttrInfoSqlNode info;
  info.name     = new_name;
  info.type     = field.type();
  info.length   = (field.type() == AttrType::VECTORS) ? field.vector_length() : static_cast<size_t>(field.len());
  info.nullable = field.nullable();
  return info;
}

RC AlterTableStmt::create(Db *db, const AlterTableSqlNode &alter_table, Stmt *&stmt)
{
  stmt = nullptr;
  if (db == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  Table *table = db->find_table(alter_table.table_name.c_str());
  if (table == nullptr) {
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  const TableMeta &table_meta = table->table_meta();
  AlterType alter_type =
      (alter_table.action == AlterTableSqlNode::Action::ADD_COLUMN) ? AlterType::ADD_COLUMN :
      (alter_table.action == AlterTableSqlNode::Action::DROP_COLUMN) ? AlterType::DROP_COLUMN :
      (alter_table.action == AlterTableSqlNode::Action::CHANGE_COLUMN) ? AlterType::CHANGE_COLUMN :
      AlterType::RENAME_TABLE;

  AttrInfoSqlNode column_info;
  string          target_column;
  string          new_column_name;
  string          new_table_name;

  switch (alter_table.action) {
    case AlterTableSqlNode::Action::ADD_COLUMN: {
      column_info = alter_table.column_info;
      if (common::is_blank(column_info.name.c_str())) {
        return RC::INVALID_ARGUMENT;
      }
      if (column_info.type != AttrType::INTS) {
        LOG_WARN("only INT columns are supported in ADD COLUMN for now");
        return RC::UNSUPPORTED;
      }
      if (column_info.length == 0) {
        column_info.length = 4;
      }
      if (table_meta.field(column_info.name.c_str()) != nullptr) {
        LOG_WARN("column already exists. table=%s column=%s", table_meta.name(), column_info.name.c_str());
        return RC::EXIST;
      }
      column_info.nullable = true;
    } break;

    case AlterTableSqlNode::Action::DROP_COLUMN: {
      target_column = alter_table.target_column;
      if (common::is_blank(target_column.c_str())) {
        return RC::INVALID_ARGUMENT;
      }
      const FieldMeta *field_meta = table_meta.field(target_column.c_str());
      if (field_meta == nullptr || !field_meta->visible()) {
        LOG_WARN("column not found or invisible. table=%s column=%s", table_meta.name(), target_column.c_str());
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }
      if (contains_column(table_meta.primary_keys(), target_column)) {
        LOG_WARN("cannot drop primary key column. table=%s column=%s", table_meta.name(), target_column.c_str());
        return RC::UNSUPPORTED;
      }
    } break;

    case AlterTableSqlNode::Action::CHANGE_COLUMN: {
      target_column   = alter_table.target_column;
      new_column_name = alter_table.new_column_name.empty() ? target_column : alter_table.new_column_name;
      if (common::is_blank(target_column.c_str()) || common::is_blank(new_column_name.c_str())) {
        return RC::INVALID_ARGUMENT;
      }

      const FieldMeta *field_meta = table_meta.field(target_column.c_str());
      if (field_meta == nullptr || !field_meta->visible()) {
        LOG_WARN("column not found or invisible. table=%s column=%s", table_meta.name(), target_column.c_str());
        return RC::SCHEMA_FIELD_NOT_EXIST;
      }

      if (0 != strcasecmp(target_column.c_str(), new_column_name.c_str()) &&
          table_meta.field(new_column_name.c_str()) != nullptr) {
        LOG_WARN("new column name already exists. table=%s column=%s", table_meta.name(), new_column_name.c_str());
        return RC::EXIST;
      }

      AttrType expect_type = alter_table.column_info.type;
      if (expect_type != AttrType::INTS) {
        LOG_WARN("only INT columns are supported in CHANGE COLUMN for now");
        return RC::UNSUPPORTED;
      }
      if (field_meta->type() != expect_type) {
        LOG_WARN("column type mismatch when changing column. table=%s column=%s", table_meta.name(), target_column.c_str());
        return RC::SCHEMA_FIELD_TYPE_MISMATCH;
      }

      column_info = build_attr_info_from_field(*field_meta, new_column_name);
    } break;

    case AlterTableSqlNode::Action::RENAME_TABLE: {
      new_table_name = alter_table.new_table_name;
      if (common::is_blank(new_table_name.c_str())) {
        return RC::INVALID_ARGUMENT;
      }
      if (0 != strcasecmp(alter_table.table_name.c_str(), new_table_name.c_str()) &&
          db->find_table(new_table_name.c_str()) != nullptr) {
        LOG_WARN("table already exists with name %s", new_table_name.c_str());
        return RC::SCHEMA_TABLE_EXIST;
      }
    } break;
  }

  stmt = new AlterTableStmt(alter_table.table_name,
                            alter_type,
                            column_info,
                            target_column,
                            new_column_name,
                            new_table_name);
  return RC::SUCCESS;
}
