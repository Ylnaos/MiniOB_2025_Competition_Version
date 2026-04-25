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
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/stmt.h"
#include "sql/expr/expression.h"
#include "common/lang/unordered_set.h"
#include "event/sql_debug.h"
#include "storage/db/db.h"
#include "storage/table/table.h"

RC CreateTableStmt::create(Db *db, const CreateTableSqlNode &create_table, Stmt *&stmt)
{
  // 检查表是否已存在
  Table *existing_table = db->find_table(create_table.relation_name.c_str());
  if (existing_table != nullptr) {
    if (create_table.if_not_exists) {
      // IF NOT EXISTS: 表已存在时也视为成功
      // 创建一个虚拟的 stmt（executor 会检查并跳过实际创建）
      stmt = new CreateTableStmt(create_table.relation_name, create_table.attr_infos,
                                 create_table.primary_keys, StorageFormat::ROW_FORMAT, true);
      return RC::SUCCESS;
    }
    return RC::SCHEMA_TABLE_EXIST;
  }

  StorageFormat storage_format = get_storage_format(create_table.storage_format.c_str());
  if (storage_format == StorageFormat::UNKNOWN_FORMAT) {
    return RC::INVALID_ARGUMENT;
  }

  // CTAS path: when a SELECT subquery is present after CREATE TABLE.
  // Behavior:
  // - If user provided column definitions (attr_infos not empty), keep them.
  // - Else, derive schema from SELECT expressions.
  if (create_table.as_select) {
    Stmt *child = nullptr;
    RC rc       = Stmt::create_stmt(db, *create_table.as_select, child);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to create select stmt for CTAS. rc=%s", strrc(rc));
      return rc;
    }
    auto *select_stmt = dynamic_cast<SelectStmt *>(child);
    if (select_stmt == nullptr) {
      LOG_WARN("CTAS sub-statement is not a select stmt");
      delete child;
      return RC::INVALID_ARGUMENT;
    }
    vector<AttrInfoSqlNode> attrs;
    if (!create_table.attr_infos.empty()) {
      // Use user-specified columns in CREATE TABLE (col defs) SELECT ...
      attrs = create_table.attr_infos;
    } else {
      // Derive attributes from select expressions for CREATE TABLE AS SELECT ...
      attrs.reserve(select_stmt->query_expressions().size());

      // Track used names for deduplication
      unordered_set<string> used;

      auto make_unique_name = [&](const string &base) -> string {
        string name = base;
        if (name.empty()) name = "COL";
        // sanitize: strip table prefix t.col -> col
        size_t pos = name.rfind('.');
        if (pos != string::npos && pos + 1 < name.size()) {
          name = name.substr(pos + 1);
        }
        // normalize: column identifiers在本项目中按小写存储，便于大小写不敏感匹配
        for (auto &ch : name) {
          ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
        }
        // ensure uniqueness
        if (!used.count(name)) {
          used.insert(name);
          return name;
        }
        int suffix = 1;
        string cand;
        do {
          cand = name + "_" + to_string(suffix++);
        } while (used.count(cand));
        used.insert(cand);
        return cand;
      };

      for (const auto &uptr : select_stmt->query_expressions()) {
        const Expression *expr = uptr.get();
        AttrInfoSqlNode   info;

        const char *alias = expr->alias();
        string      base  = alias ? string(alias) : string(expr->name());
        info.name         = make_unique_name(base);

        AttrType type = expr->value_type();
        info.type      = type;
        int vlen       = expr->value_length();

        // choose sensible defaults when unknown
        size_t len = 4;
        switch (type) {
          case AttrType::INTS:
          case AttrType::FLOATS:
          case AttrType::DATES: {
            len = 4;
          } break;
          case AttrType::CHARS: {
            len = (vlen > 0) ? static_cast<size_t>(vlen) : static_cast<size_t>(128);
          } break;
          case AttrType::TEXTS: {
            // TableMeta will set to TEXT_MAX_LENGTH
            len = 0;
          } break;
          case AttrType::VECTORS: {
            len = (vlen > 0) ? static_cast<size_t>(vlen) : static_cast<size_t>(16);
          } break;
          default: {
            len = (vlen > 0) ? static_cast<size_t>(vlen) : static_cast<size_t>(4);
          } break;
        }
        info.length   = len;
        info.nullable = true; // CTAS columns default nullable
        attrs.emplace_back(std::move(info));
      }
    }

    auto *create_stmt = new CreateTableStmt(create_table.relation_name, attrs, {} /*pks*/, storage_format, create_table.if_not_exists);
    // attach select stmt (owned)
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    create_stmt->as_select_stmt_ = select_stmt;
    stmt                         = create_stmt;
    sql_debug("create table as select: table name %s", create_table.relation_name.c_str());
    return RC::SUCCESS;
  }

  // Normal CREATE TABLE
  stmt = new CreateTableStmt(create_table.relation_name, create_table.attr_infos, create_table.primary_keys, storage_format, create_table.if_not_exists);
  sql_debug("create table statement: table name %s", create_table.relation_name.c_str());
  return RC::SUCCESS;
}

CreateTableStmt::~CreateTableStmt()
{
  if (as_select_stmt_ != nullptr) {
    delete as_select_stmt_;
    as_select_stmt_ = nullptr;
  }
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
