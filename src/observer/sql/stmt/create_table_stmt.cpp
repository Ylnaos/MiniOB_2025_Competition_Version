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
// Created by Wangyunlai on 2023/6/13.
//

#include "common/log/log.h"
#include "common/types.h"
#include "sql/stmt/create_table_stmt.h"
#include "event/sql_debug.h"
#include "sql/stmt/select_stmt.h"
#include "sql/expr/expression.h"

RC CreateTableStmt::create(Db *db, const CreateTableSqlNode &create_table, Stmt *&stmt)
{
  StorageFormat storage_format = get_storage_format(create_table.storage_format.c_str());
  if (storage_format == StorageFormat::UNKNOWN_FORMAT) {
    return RC::INVALID_ARGUMENT;
  }
  // 普通建表
  auto *ct_stmt = new CreateTableStmt(create_table.relation_name, create_table.attr_infos, create_table.primary_keys, storage_format);
  // 如携带 CTAS 子句（兼容从 yacc_sql.cpp 直接解析的情况），在此构建 SelectStmt 并推导列结构
  if (create_table.select_node) {
    if (create_table.select_node->flag != SCF_SELECT) {
      delete ct_stmt;
      return RC::INVALID_ARGUMENT;
    }
    // 基于 select 生成 SelectStmt
    Stmt *sel_stmt_base = nullptr;
    RC rc = SelectStmt::create(db, create_table.select_node->selection, sel_stmt_base);
    if (OB_FAIL(rc)) {
      delete ct_stmt;
      return rc;
    }
    auto sel_stmt = static_cast<SelectStmt *>(sel_stmt_base);

    // 依据 select 的投影表达式推导列定义
    vector<AttrInfoSqlNode> inferred_attrs;
    auto &exprs = sel_stmt->query_expressions();
    inferred_attrs.reserve(exprs.size());
    for (size_t i = 0; i < exprs.size(); i++) {
      Expression *e = exprs[i].get();
      AttrInfoSqlNode col;
      col.type = e->value_type();
      int vlen = e->value_length();
      // 名称优先使用别名，否则用 name()，含点号时仅取字段名部分
      string name = e->alias() ? string(e->alias()) : string(e->name());
      if (name.empty()) {
        name = string("C") + std::to_string(i + 1);
      } else {
        size_t dot = name.rfind('.');
        if (dot != string::npos) {
          name = name.substr(dot + 1);
        }
      }
      col.name = name;

      // 长度：字符类型尽量使用表达式长度；否则用类型默认长度
      if (col.type == AttrType::CHARS) {
        col.length = (vlen > 0 ? static_cast<size_t>(vlen) : 32);
      } else if (col.type == AttrType::TEXTS) {
        col.length = 0; // 由 TableMeta 内部扩展为 TEXT_MAX_LENGTH
      } else if (col.type == AttrType::VECTORS) {
        col.length = (vlen > 0 ? static_cast<size_t>(vlen) : 0);
      } else {
        col.length = 4; // INT/FLOAT/DATE 等默认4字节
      }
      // 允许 NULL，以兼容 SELECT 可能产生的 NULL 值
      col.nullable = true;
      inferred_attrs.emplace_back(col);
    }

    // 用推导列覆盖原列定义
    ct_stmt->attr_infos_ = std::move(inferred_attrs);
    ct_stmt->is_ctas_    = true;
    ct_stmt->select_stmt_.reset(sel_stmt);
  }
  stmt = ct_stmt;
  sql_debug("create table statement: table name %s", create_table.relation_name.c_str());
  return RC::SUCCESS;
}

StorageFormat CreateTableStmt::get_storage_format(const char *format_str) {
  StorageFormat format = StorageFormat::UNKNOWN_FORMAT;
  if (strlen(format_str) == 0) {
    format = StorageFormat::ROW_FORMAT;
  } else if (0 == strcasecmp(format_str, "ROW")) {
    format = StorageFormat::ROW_FORMAT;
  } else if (0 == strcasecmp(format_str, "PAX")) {
    format = StorageFormat::PAX_FORMAT;
  } else {
    format = StorageFormat::UNKNOWN_FORMAT;
  }
  return format;
}

RC CreateTableStmt::create(Db *db, const CreateTableAsSelectSqlNode &ctas, Stmt *&stmt)
{
  // CTAS 语句未显式提供存储格式，这里默认 ROW
  StorageFormat storage_format = StorageFormat::ROW_FORMAT;

  // 先构建 SelectStmt
  if (!ctas.select_node || ctas.select_node->flag != SCF_SELECT) {
    return RC::INVALID_ARGUMENT;
  }

  Stmt *sel_stmt_base = nullptr;
  RC rc = SelectStmt::create(db, ctas.select_node->selection, sel_stmt_base);
  if (OB_FAIL(rc)) {
    return rc;
  }
  auto sel_stmt = static_cast<SelectStmt *>(sel_stmt_base);

  // 推导列定义
  vector<AttrInfoSqlNode> inferred_attrs;
  auto &exprs = sel_stmt->query_expressions();
  inferred_attrs.reserve(exprs.size());
  for (size_t i = 0; i < exprs.size(); i++) {
    Expression *e = exprs[i].get();
    AttrInfoSqlNode col;
    col.type = e->value_type();
    int vlen = e->value_length();
    string name = e->alias() ? string(e->alias()) : string(e->name());
    if (name.empty()) {
      name = string("C") + std::to_string(i + 1);
    } else {
      size_t dot = name.rfind('.');
      if (dot != string::npos) {
        name = name.substr(dot + 1);
      }
    }
    col.name = name;
    if (col.type == AttrType::CHARS) {
      col.length = (vlen > 0 ? static_cast<size_t>(vlen) : 32);
    } else if (col.type == AttrType::TEXTS) {
      col.length = 0;
    } else if (col.type == AttrType::VECTORS) {
      col.length = (vlen > 0 ? static_cast<size_t>(vlen) : 0);
    } else {
      col.length = 4;
    }
    col.nullable = true;
    inferred_attrs.emplace_back(col);
  }

  auto *ct_stmt = new CreateTableStmt(ctas.table_name, inferred_attrs, /*pks*/ {}, storage_format);
  ct_stmt->is_ctas_ = true;
  ct_stmt->select_stmt_.reset(sel_stmt);
  stmt = ct_stmt;
  sql_debug("create table as select statement: table name %s", ctas.table_name.c_str());
  return RC::SUCCESS;
}
