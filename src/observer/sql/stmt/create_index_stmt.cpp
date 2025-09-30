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

  bool vector_index = create_index.vector_index;
  if (vector_index) {
    if (create_index.unique) {
      LOG_WARN("vector index cannot be unique. table=%s index=%s", table_name, create_index.index_name.c_str());
      return RC::INVALID_ARGUMENT;
    }
    if (field_metas.size() != 1) {
      LOG_WARN("vector index must reference exactly one column. table=%s index=%s", table_name, create_index.index_name.c_str());
      return RC::INVALID_ARGUMENT;
    }
    if (field_metas[0]->type() != AttrType::VECTORS) {
      LOG_WARN("vector index column must be vector type. table=%s index=%s", table_name, create_index.index_name.c_str());
      return RC::INVALID_ARGUMENT;
    }
  }

  string distance_func = create_index.distance_func;
  string index_type    = create_index.vector_index_type;
  int    lists         = create_index.lists;
  int    probes        = create_index.probes;

  if (vector_index) {
    if (distance_func.empty()) {
      distance_func = "l2_distance";
    }
    if (index_type.empty()) {
      index_type = "ivfflat";
    }
    common::str_to_lower(distance_func);
    common::str_to_lower(index_type);

    if (lists <= 0) {
      lists = 1;
    }
    if (probes <= 0) {
      probes = 1;
    }
    if (probes > lists) {
      probes = lists;
    }
  } else {
    distance_func.clear();
    index_type.clear();
    lists  = 0;
    probes = 0;
  }

  // 幂等处理：若已存在同名索引，且字段列表、唯一性与本次请求一致，则视为成功
  // 否则保持与现有行为一致，返回索引名重复错误
  if (Index *exist = table->find_index(create_index.index_name.c_str()); exist != nullptr) {
    const IndexMeta            &meta          = exist->index_meta();
    const vector<string>       &exist_fields  = meta.fields();
    const vector<string>       &req_fields    = create_index.attribute_names;
    const bool                  same_unique   = (meta.unique() == create_index.unique);
    const bool                  same_vector   = (meta.is_vector_index() == vector_index);
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
    bool vector_params_same = true;
    if (same_fields && same_unique && same_vector && vector_index) {
      vector_params_same = (0 == strcasecmp(meta.distance_func().c_str(), distance_func.c_str()) &&
                            0 == strcasecmp(meta.vector_index_type().c_str(), index_type.c_str()) &&
                            meta.lists() == lists &&
                            meta.probes() == probes);
    }

    if (same_fields && same_unique && same_vector && vector_params_same) {
      // 已存在完全相同定义的索引：幂等返回成功，符合官方期望
      return RC::SUCCESS;
    }
    LOG_WARN("index with name(%s) already exists but definition differs. table=%s",
             create_index.index_name.c_str(), table_name);
    return RC::SCHEMA_INDEX_NAME_REPEAT;
  }

  stmt = new CreateIndexStmt(table, std::move(field_metas), create_index.index_name, create_index.unique,
      vector_index, std::move(distance_func), std::move(index_type), lists, probes);
  return RC::SUCCESS;
}
