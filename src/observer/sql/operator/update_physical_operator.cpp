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
#include "sql/expr/expression_iterator.h"
#include "storage/record/record.h"
#include <functional>

UpdatePhysicalOperator::UpdatePhysicalOperator(Table *table, const vector<const FieldMeta *> &field_metas,
                                               const vector<Value> &values,
                                               const vector<unique_ptr<Expression>> &value_expressions)
    : table_(table), field_metas_(field_metas), values_(values)
{
  // 复制表达式
  for (const auto &expr : value_expressions) {
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
  RC rc = RC::SUCCESS;

  // Pre-validate subqueries in SET expressions so schema errors are surfaced
  // even when no rows match the WHERE clause (e.g., subquery references a non-existent table).
  // Use a dummy tuple for correlated subqueries; non-correlated ones will be cached via execute_once().
  if (!value_expressions_.empty()) {
    const TableMeta &table_meta  = table_->table_meta();
    const int        record_size = table_meta.record_size();
    // prepare a zero-filled dummy record of current table schema
    char  *dummy_buf = static_cast<char *>(calloc(1, record_size));
    Record dummy_rec;
    if (dummy_buf != nullptr) {
      dummy_rec.set_data_owner(dummy_buf, record_size);
    }
    RowTuple dummy_tuple;
    dummy_tuple.set_record(&dummy_rec);
    dummy_tuple.set_schema(table_, table_meta.field_metas());

    std::function<RC(Expression *)> precheck_expr;
    precheck_expr = [&](Expression *expr) -> RC {
      if (expr == nullptr) return RC::SUCCESS;
      if (expr->type() == ExprType::SUBQUERY) {
        auto *subq = static_cast<SubqueryExpr *>(expr);
        return subq->execute_with_context(&dummy_tuple);
      }
      RC inner_rc = ExpressionIterator::iterate_child_expr(*expr, [&](std::unique_ptr<Expression> &child) -> RC {
        return precheck_expr(child.get());
      });
      return inner_rc;
    };

    for (const auto &uptr : value_expressions_) {
      if (!uptr) continue;
      rc = precheck_expr(uptr.get());
      if (rc != RC::SUCCESS) {
        return rc;
      }
    }
  }

  // 逐条更新的封装：根据旧记录计算新值，做严格类型校验并写回
  auto update_one = [&](Record &old_record) -> RC {
    const TableMeta &table_meta   = table_->table_meta();
    const int        record_size  = table_meta.record_size();
    char            *new_rec_data = static_cast<char *>(malloc(record_size));
    if (new_rec_data == nullptr) {
      LOG_WARN("failed to allocate memory for new record");
      return RC::NOMEM;
    }

    memcpy(new_rec_data, old_record.data(), record_size);

    for (size_t i = 0; i < field_metas_.size(); ++i) {
      const FieldMeta *field_meta = field_metas_[i];
      Value            value;
      if (i < value_expressions_.size() && value_expressions_[i] != nullptr) {
        RowTuple row_tuple;
        row_tuple.set_record(&old_record);
        row_tuple.set_schema(table_, table_->table_meta().field_metas());
        rc = value_expressions_[i]->get_value(row_tuple, value);
        if (rc != RC::SUCCESS) {
          LOG_WARN("failed to calculate expression value. rc=%s", strrc(rc));
          free(new_rec_data);
          return rc;
        }
        // 非 NULL 严格类型匹配（必要时尝试显式转换）
        if (!value.is_null() && field_meta->type() != value.attr_type()) {
          Value real_value;
          rc = Value::cast_to(value, field_meta->type(), real_value);
          if (rc != RC::SUCCESS) {
            LOG_WARN("field type mismatch. field=%s, field type=%d, value type=%d",
                     field_meta->name(), field_meta->type(), value.attr_type());
            free(new_rec_data);
            return RC::SCHEMA_FIELD_TYPE_MISMATCH;
          }
          value = real_value;
        }
      } else if (i < values_.size()) {
        value = values_[i];
      } else {
        LOG_WARN("no value or expression for field. field=%s", field_meta->name());
        free(new_rec_data);
        return RC::INVALID_ARGUMENT;
      }

      rc = table_->set_value_to_record(new_rec_data, value, field_meta);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to set value to record. field=%s, rc=%s", field_meta->name(), strrc(rc));
        free(new_rec_data);
        return rc;
      }
    }

    Record new_record;
    new_record.set_data_owner(new_rec_data, record_size);
    new_record.set_rid(old_record.rid());

    rc = trx_->update_record(table_, old_record, new_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to update record: %s", strrc(rc));
      return rc;
    }
    return RC::SUCCESS;
  };

  // 统一策略：无论是否含 WHERE，都先收集 RID，关闭扫描器后逐条更新，避免迭代器失效
  std::vector<RID> rids;
  if (children_.empty()) {
    TableScanPhysicalOperator scan_oper(table_, ReadWriteMode::READ_WRITE);
    rc = scan_oper.open(trx);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to open table scan operator: %s", strrc(rc));
      return rc;
    }
    while (OB_SUCC(rc = scan_oper.next())) {
      Tuple *tuple = scan_oper.current_tuple();
      if (nullptr == tuple) {
        LOG_WARN("failed to get current record: %s", strrc(rc));
        scan_oper.close();
        return rc;
      }
      RowTuple *row_tuple = static_cast<RowTuple *>(tuple);
      rids.emplace_back(row_tuple->record().rid());
    }
    if (rc == RC::RECORD_EOF) {
      rc = RC::SUCCESS;
    }
    scan_oper.close();
    if (rc != RC::SUCCESS) {
      return rc;
    }
  } else {
    unique_ptr<PhysicalOperator> &child = children_[0];
    rc = child->open(trx);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to open child operator: %s", strrc(rc));
      return rc;
    }
    while (OB_SUCC(rc = child->next())) {
      Tuple *tuple = child->current_tuple();
      if (nullptr == tuple) {
        LOG_WARN("failed to get current record: %s", strrc(rc));
        child->close();
        return rc;
      }
      RowTuple *row_tuple = static_cast<RowTuple *>(tuple);
      rids.emplace_back(row_tuple->record().rid());
    }
    if (rc == RC::RECORD_EOF) {
      rc = RC::SUCCESS;
    }
    child->close();
    if (rc != RC::SUCCESS) {
      return rc;
    }
  }

  for (const RID &rid : rids) {
    Record old_record;
    rc = table_->get_record(rid, old_record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to refetch record by rid=%s. rc=%s", rid.to_string().c_str(), strrc(rc));
      return rc;
    }
    rc = update_one(old_record);
    if (rc != RC::SUCCESS) {
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
