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
// Created by Wangyunlai on 2023/4/25.
//

#include "sql/stmt/create_index_stmt.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/index/index.h"  // for Index and IndexMeta (avoid incomplete type)

using namespace std;
using namespace common;

RC CreateIndexStmt::create(Db *db, const CreateIndexSqlNode &create_index, Stmt *&stmt)
{
  stmt = nullptr;

  const char *table_name = create_index.relation_name.c_str();
  if (is_blank(table_name) || is_blank(create_index.index_name.c_str()) ||
      create_index.attribute_names.empty()) {
    LOG_WARN("invalid argument. db=%p, table_name=%p, index name=%s, attr num=%zu",
        db, table_name, create_index.index_name.c_str(), create_index.attribute_names.size());
    return RC::INVALID_ARGUMENT;
  }

  // check whether the table exists
  Table *table = db->find_table(table_name);
  if (nullptr == table) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  vector<const FieldMeta *> field_metas;
  field_metas.reserve(create_index.attribute_names.size());
  for (const string &attr_name : create_index.attribute_names) {
    const FieldMeta *field_meta = table->table_meta().field(attr_name.c_str());
    if (nullptr == field_meta) {
      LOG_WARN("no such field in table. db=%s, table=%s, field name=%s", 
               db->name(), table_name, attr_name.c_str());
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }
    field_metas.push_back(field_meta);
  }

  // 幂等处理：若已存在同名索引，且字段列表、唯一性与本次请求一致，则视为成功
  // 否则保持与现有行为一致，返回索引名重复错误
  if (Index *exist = table->find_index(create_index.index_name.c_str()); exist != nullptr) {
    const IndexMeta            &meta          = exist->index_meta();
    const vector<string>       &exist_fields  = meta.fields();
    const vector<string>       &req_fields    = create_index.attribute_names;
    const bool                  same_unique   = (meta.unique() == create_index.unique);
    const bool                  same_field_sz = (exist_fields.size() == req_fields.size());
    bool                        same_fields   = same_field_sz;
    if (same_field_sz) {
      for (size_t i = 0; i < exist_fields.size(); i++) {
        if (0 != strcasecmp(exist_fields[i].c_str(), req_fields[i].c_str())) {
          same_fields = false;
          break;
        }
      }
    }
    if (same_fields && same_unique) {
      // 已存在完全相同定义的索引：幂等返回成功，符合官方期望
      return RC::SUCCESS;
    }
    if (create_index.if_not_exists) {
      // IF NOT EXISTS 语义：索引存在即可视为成功，避免报错
      LOG_INFO("skip creating index since it already exists. table=%s, index=%s",
               table_name, create_index.index_name.c_str());
      return RC::SUCCESS;
    }
    LOG_WARN("index with name(%s) already exists but definition differs. table=%s",
             create_index.index_name.c_str(), table_name);
    return RC::SCHEMA_INDEX_NAME_REPEAT;
  }

  stmt = new CreateIndexStmt(table,
                             std::move(field_metas),
                             create_index.index_name,
                             create_index.unique,
                             create_index.if_not_exists,
                             create_index.is_vector_index,
                             create_index.distance_type,
                             create_index.index_type,
                             create_index.lists,
                             create_index.probes);
  return RC::SUCCESS;
}
