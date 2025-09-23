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

#include "sql/stmt/update_stmt.h"
#include "common/log/log.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/stmt/filter_stmt.h"
#include "sql/expr/expression.h"
#include "common/value.h"

UpdateStmt::UpdateStmt(Table *table, const vector<const FieldMeta *> &field_metas,
                       const vector<Value> &values, const vector<Expression *> &value_expressions,
                       FilterStmt *filter_stmt)
    : table_(table), field_metas_(field_metas), values_(values),
      value_expressions_(value_expressions), filter_stmt_(filter_stmt)
{}

UpdateStmt::~UpdateStmt()
{
  if (filter_stmt_ != nullptr) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
  for (auto expr : value_expressions_) {
    delete expr;
  }
}

RC UpdateStmt::create(Db *db, const UpdateSqlNode &update, Stmt *&stmt)
{
  const char *table_name = update.relation_name.c_str();
  if (nullptr == db || nullptr == table_name) {
    LOG_WARN("invalid argument. db=%p, table_name=%p", db, table_name);
    return RC::INVALID_ARGUMENT;
  }

  // 查找表
  Table *table = db->find_table(table_name);
  if (nullptr == table) {
    LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
    return RC::SCHEMA_TABLE_NOT_EXIST;
  }

  // 对于兼容性：如果没有指定字段（空的更新），返回错误
  if (update.attribute_names.empty()) {
    LOG_WARN("no fields to update");
    return RC::INVALID_ARGUMENT;
  }

  vector<const FieldMeta *> field_metas;
  vector<Value> values;
  vector<Expression *> value_expressions;

  // 处理每个字段-表达式对
  for (size_t i = 0; i < update.attribute_names.size(); ++i) {
    // 查找要更新的字段
    const FieldMeta *field_meta = table->table_meta().field(update.attribute_names[i].c_str());
    if (nullptr == field_meta) {
      LOG_WARN("no such field. field=%s.%s", table_name, update.attribute_names[i].c_str());
      return RC::SCHEMA_FIELD_NOT_EXIST;
    }

    field_metas.push_back(field_meta);

    // 保存表达式供后续使用
    if (i < update.value_expressions.size() && update.value_expressions[i] != nullptr) {
      value_expressions.push_back(update.value_expressions[i]);
      values.push_back(Value());  // 占位符，实际值在执行时计算
    } else if (i < update.values.size()) {
      // 兼容旧的值列表
      Value value = update.values[i];
      if (field_meta->type() != value.attr_type()) {
        // 尝试类型转换
        Value real_value;
        RC rc = Value::cast_to(value, field_meta->type(), real_value);
        if (RC::SUCCESS != rc) {
          LOG_WARN("field type mismatch. table=%s, field=%s, field type=%d, value type=%d",
              table_name, field_meta->name(), field_meta->type(), value.attr_type());
          return RC::SCHEMA_FIELD_TYPE_MISMATCH;
        }
        value = real_value;
      }
      values.push_back(value);
      value_expressions.push_back(nullptr);
    } else {
      LOG_WARN("no value or expression for field. field=%s", field_meta->name());
      return RC::INVALID_ARGUMENT;
    }
  }

  // 创建过滤条件
  FilterStmt *filter_stmt = nullptr;
  if (!update.conditions.empty()) {
    unordered_map<string, Table *> table_map;
    table_map[table->name()] = table;
    RC rc = FilterStmt::create(db, table, &table_map, update.conditions.data(),
                                update.conditions.size(), filter_stmt);
    if (RC::SUCCESS != rc) {
      LOG_WARN("failed to create filter statement. rc=%s", strrc(rc));
      return rc;
    }
  }

  // 创建更新语句
  stmt = new UpdateStmt(table, field_metas, values, value_expressions, filter_stmt);
  return RC::SUCCESS;
}