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
// Created by WangYunlai on 2022/6/27.
//

#include "sql/operator/update_physical_operator.h"
#include "common/log/log.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"
#include "sql/stmt/update_stmt.h"
#include "sql/operator/table_scan_physical_operator.h"
#include "sql/expr/tuple.h"

UpdatePhysicalOperator::UpdatePhysicalOperator(Table *table, const vector<const FieldMeta *> &field_metas,
                                               const vector<Value> &values,
                                               const vector<unique_ptr<Expression>> &value_expressions)
    : table_(table), field_metas_(field_metas), values_(values)
{
  // 复制表达式
  for (const auto& expr : value_expressions) {
    if (expr) {
      value_expressions_.push_back(expr->copy());
    } else {
      value_expressions_.push_back(nullptr);
    }
  }
}

RC UpdatePhysicalOperator::open(Trx *trx)
{
  trx_ = trx;

  std::vector<Record> old_records;

  if (children_.empty()) {
    // 没有WHERE条件，更新所有记录
    TableScanPhysicalOperator scan_oper(table_, ReadWriteMode::READ_WRITE);
    RC rc = scan_oper.open(trx);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to open table scan operator: %s", strrc(rc));
      return rc;
    }

    while (OB_SUCC(rc = scan_oper.next())) {
      Tuple *tuple = scan_oper.current_tuple();
      if (nullptr == tuple) {
        LOG_WARN("failed to get current record: %s", strrc(rc));
        return rc;
      }

      RowTuple *row_tuple = static_cast<RowTuple *>(tuple);
      Record   &record    = row_tuple->record();
      old_records.emplace_back(std::move(record));
    }

    scan_oper.close();
  } else {
    // 有WHERE条件，使用子算子获取记录
    unique_ptr<PhysicalOperator> &child = children_[0];
    RC rc = child->open(trx);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to open child operator: %s", strrc(rc));
      return rc;
    }

    while (OB_SUCC(rc = child->next())) {
      Tuple *tuple = child->current_tuple();
      if (nullptr == tuple) {
        LOG_WARN("failed to get current record: %s", strrc(rc));
        return rc;
      }

      RowTuple *row_tuple = static_cast<RowTuple *>(tuple);
      Record   &record    = row_tuple->record();
      old_records.emplace_back(std::move(record));
    }

    child->close();
  }

  // 对每条记录执行更新
  for (Record &old_record : old_records) {
    // 构造新记录
    const TableMeta &table_meta = table_->table_meta();
    int record_size = table_meta.record_size();
    char *new_record_data = (char *)malloc(record_size);

    // 复制原记录数据
    memcpy(new_record_data, old_record.data(), record_size);

    // 更新所有指定字段的值
    for (size_t i = 0; i < field_metas_.size(); ++i) {
      const FieldMeta *field_meta = field_metas_[i];
      Value value;

      // 如果有表达式，计算表达式的值
      if (i < value_expressions_.size() && value_expressions_[i] != nullptr) {
        // 创建一个RowTuple用于表达式计算
        RowTuple row_tuple;
        row_tuple.set_record(const_cast<Record*>(&old_record));
        // row_tuple.set_table(table_); // RowTuple没有set_table方法

        // 计算表达式值
        RC rc = value_expressions_[i]->get_value(row_tuple, value);
        if (rc != RC::SUCCESS) {
          LOG_WARN("failed to calculate expression value. rc=%s", strrc(rc));
          free(new_record_data);
          return rc;
        }

        // 类型转换
        if (field_meta->type() != value.attr_type()) {
          Value real_value;
          rc = Value::cast_to(value, field_meta->type(), real_value);
          if (rc != RC::SUCCESS) {
            LOG_WARN("field type mismatch. field=%s, field type=%d, value type=%d",
                field_meta->name(), field_meta->type(), value.attr_type());
            free(new_record_data);
            return RC::SCHEMA_FIELD_TYPE_MISMATCH;
          }
          value = real_value;
        }
      } else if (i < values_.size()) {
        // 使用静态值
        value = values_[i];
      } else {
        LOG_WARN("no value or expression for field. field=%s", field_meta->name());
        free(new_record_data);
        return RC::INVALID_ARGUMENT;
      }

      size_t copy_len = field_meta->len();
      const size_t data_len = value.length();

      if (field_meta->type() == AttrType::CHARS) {
        if (copy_len > data_len) {
          copy_len = data_len + 1;
        }
        // 字符串类型需要清零后再复制
        memset(new_record_data + field_meta->offset(), 0, field_meta->len());
      }
      memcpy(new_record_data + field_meta->offset(), value.data(), copy_len);
    }

    // 构造新记录对象
    Record new_record;
    new_record.set_data_owner(new_record_data, record_size);
    new_record.set_rid(old_record.rid());

    // 调用事务层执行更新
    RC rc = trx_->update_record(table_, old_record, new_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to update record: %s", strrc(rc));
      return rc;
    }
  }

  return RC::SUCCESS;
}

RC UpdatePhysicalOperator::next()
{
  return RC::RECORD_EOF;
}

RC UpdatePhysicalOperator::close()
{
  return RC::SUCCESS;
}