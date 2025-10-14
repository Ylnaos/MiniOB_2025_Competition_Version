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
// Created by Wangyunlai.wyl on 2021/5/18.
//

#include "storage/index/index_meta.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "storage/field/field_meta.h"
#include "storage/table/table_meta.h"
#include "json/json.h"
#include <algorithm>

const static Json::StaticString FIELD_NAME("name");
const static Json::StaticString FIELD_FIELD_NAMES("field_names");
const static Json::StaticString FIELD_UNIQUE("unique");
const static Json::StaticString FIELD_IS_VECTOR_INDEX("is_vector_index");
const static Json::StaticString FIELD_INDEX_TYPE("index_type");
const static Json::StaticString FIELD_DISTANCE_TYPE("distance_type");
const static Json::StaticString FIELD_LISTS("lists");
const static Json::StaticString FIELD_PROBES("probes");

RC IndexMeta::init(const char *name, span<const FieldMeta> fields, bool unique)
{
  if (common::is_blank(name)) {
    LOG_ERROR("Failed to init index, name is empty.");
    return RC::INVALID_ARGUMENT;
  }

  if (fields.empty()) {
    LOG_ERROR("Failed to init index, fields is empty.");
    return RC::INVALID_ARGUMENT;
  }

  name_   = name;
  fields_.clear();
  fields_.reserve(fields.size());
  for (const FieldMeta &f : fields) {
    fields_.push_back(f.name());
  }
  unique_            = unique;
  is_vector_index_   = false;
  index_type_.clear();
  distance_type_.clear();
  lists_  = 0;
  probes_ = 0;
  return RC::SUCCESS;
}

void IndexMeta::set_vector_options(bool is_vector_index,
                                   const string &index_type,
                                   const string &distance_type,
                                   int lists,
                                   int probes)
{
  if (!is_vector_index) {
    is_vector_index_ = false;
    index_type_.clear();
    distance_type_.clear();
    lists_  = 0;
    probes_ = 0;
    return;
  }

  is_vector_index_ = true;
  index_type_      = index_type;
  distance_type_   = distance_type;
  lists_           = std::max(0, lists);
  probes_          = std::max(0, probes);
}

void IndexMeta::to_json(Json::Value &json_value) const
{
  json_value[FIELD_NAME]       = name_;
  Json::Value field_list(Json::arrayValue);
  for (const string &fn : fields_) {
    field_list.append(fn);
  }
  json_value[FIELD_FIELD_NAMES] = field_list;
  json_value[FIELD_UNIQUE]      = unique_;
  if (is_vector_index_) {
    json_value[FIELD_IS_VECTOR_INDEX] = true;
    if (!index_type_.empty()) {
      json_value[FIELD_INDEX_TYPE] = index_type_;
    }
    if (!distance_type_.empty()) {
      json_value[FIELD_DISTANCE_TYPE] = distance_type_;
    }
    json_value[FIELD_LISTS]  = lists_;
    json_value[FIELD_PROBES] = probes_;
  }
}

RC IndexMeta::from_json(const TableMeta &table, const Json::Value &json_value, IndexMeta &index)
{
  const Json::Value &name_value  = json_value[FIELD_NAME];
  const Json::Value &field_list  = json_value[FIELD_FIELD_NAMES];
  const Json::Value &unique_value = json_value[FIELD_UNIQUE];
  if (!name_value.isString()) {
    LOG_ERROR("Index name is not a string. json value=%s", name_value.toStyledString().c_str());
    return RC::INTERNAL;
  }

  if (!field_list.isArray() || field_list.size() < 1) {
    LOG_ERROR("Field names of index [%s] is not array or empty. json value=%s",
        name_value.asCString(), field_list.toStyledString().c_str());
    return RC::INTERNAL;
  }

  vector<FieldMeta> fields;
  fields.reserve(field_list.size());
  for (int i = 0; i < (int)field_list.size(); i++) {
    const Json::Value &field_value = field_list[i];
    if (!field_value.isString()) {
      LOG_ERROR("Field name of index [%s] is not a string. json value=%s",
          name_value.asCString(), field_value.toStyledString().c_str());
      return RC::INTERNAL;
    }
    const FieldMeta *field = table.field(field_value.asCString());
    if (nullptr == field) {
      LOG_ERROR("Deserialize index [%s]: no such field: %s", name_value.asCString(), field_value.asCString());
      return RC::SCHEMA_FIELD_MISSING;
    }
    fields.push_back(*field);
  }

  bool unique = false;
  if (unique_value.isBool()) {
    unique = unique_value.asBool();
  }

  RC rc = index.init(name_value.asCString(), span<const FieldMeta>(fields.data(), fields.size()), unique);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  bool is_vector_index = false;
  const Json::Value &vector_flag_value = json_value[FIELD_IS_VECTOR_INDEX];
  if (vector_flag_value.isBool()) {
    is_vector_index = vector_flag_value.asBool();
  }

  string index_type;
  string distance_type;
  int    lists  = 0;
  int    probes = 0;

  if (is_vector_index) {
    const Json::Value &index_type_value = json_value[FIELD_INDEX_TYPE];
    if (index_type_value.isString()) {
      index_type = index_type_value.asString();
    }
    const Json::Value &distance_type_value = json_value[FIELD_DISTANCE_TYPE];
    if (distance_type_value.isString()) {
      distance_type = distance_type_value.asString();
    }
    const Json::Value &lists_value = json_value[FIELD_LISTS];
    if (lists_value.isInt()) {
      lists = lists_value.asInt();
    }
    const Json::Value &probes_value = json_value[FIELD_PROBES];
    if (probes_value.isInt()) {
      probes = probes_value.asInt();
    }
  }

  index.set_vector_options(is_vector_index, index_type, distance_type, lists, probes);
  return RC::SUCCESS;
}

const char *IndexMeta::name() const { return name_.c_str(); }

const char *IndexMeta::field() const { return fields_.empty() ? "" : fields_[0].c_str(); }

void IndexMeta::desc(ostream &os) const {
  os << "index name=" << name_ << ", fields=[";
  for (size_t i = 0; i < fields_.size(); i++) {
    if (i != 0) os << ",";
    os << fields_[i];
  }
  os << "], unique=" << (unique_ ? 1 : 0);
  if (is_vector_index_) {
    os << ", vector(index_type=" << index_type_
       << ", distance_type=" << distance_type_
       << ", lists=" << lists_
       << ", probes=" << probes_ << ")";
  }
}
