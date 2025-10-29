/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/executor/create_materialized_view_executor.h"
#include "common/log/log.h"
#include "common/lang/unordered_set.h"
#include "event/sql_event.h"
#include "event/session_event.h"
#include "session/session.h"
#include "sql/stmt/create_materialized_view_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/operator/physical_operator.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/trx/trx.h"

RC CreateMaterializedViewExecutor::execute(SQLStageEvent *sql_event)
{
  Session *session = sql_event->session_event()->session();
  Db      *db      = session->get_current_db();

  auto *stmt = static_cast<CreateMaterializedViewStmt *>(sql_event->stmt());
  if (nullptr == stmt) {
    return RC::INVALID_ARGUMENT;
  }

  const char *view_name = stmt->view_name().c_str();
  SelectStmt *select_stmt = stmt->select_stmt();

  if (nullptr == select_stmt) {
    LOG_WARN("materialized view select statement is null");
    return RC::INVALID_ARGUMENT;
  }

  // 检查名称冲突
  if (db->find_table(view_name) != nullptr) {
    LOG_WARN("table with same name already exists: %s", view_name);
    return RC::SCHEMA_TABLE_EXIST;
  }
  if (db->find_view(view_name) != nullptr) {
    LOG_WARN("view with same name already exists: %s", view_name);
    return RC::EXIST;
  }

  // 1. 从 SelectStmt 推导出结果的 schema (列名、类型)
  vector<AttrInfoSqlNode> attrs;
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
    info.nullable = true; // Materialized view columns default nullable
    attrs.emplace_back(std::move(info));
  }

  // 2. 创建物理表来存储查询结果
  RC rc = db->create_table(view_name, attrs, {} /*no primary keys*/, StorageFormat::ROW_FORMAT);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create table for materialized view: %s, rc=%s", view_name, strrc(rc));
    return rc;
  }

  // 3. 执行查询并将结果插入表中
  Table *table = db->find_table(view_name);
  if (table == nullptr) {
    LOG_WARN("newly created materialized view table not found: %s", view_name);
    return RC::INTERNAL;
  }

  // Build logical & physical plan
  std::unique_ptr<LogicalOperator>  logical_oper;
  LogicalPlanGenerator              logical_gen;
  rc = logical_gen.create(select_stmt, logical_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create logical plan for materialized view. rc=%s", strrc(rc));
    return rc;
  }

  std::unique_ptr<PhysicalOperator> physical_oper;
  PhysicalPlanGenerator             physical_gen;
  rc = physical_gen.create(*logical_oper, physical_oper, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create physical plan for materialized view. rc=%s", strrc(rc));
    return rc;
  }

  Trx *trx = session->current_trx();
  // Ensure DML inside materialized view creation runs in a proper transaction
  trx->start_if_need();
  rc = physical_oper->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open materialized view physical operator. rc=%s", strrc(rc));
    return rc;
  }

  vector<RID> inserted_rids;

  Tuple *out_tuple = nullptr;
  while (RC::SUCCESS == (rc = physical_oper->next())) {
    out_tuple = physical_oper->current_tuple();
    if (out_tuple == nullptr) {
      rc = RC::INTERNAL;
      break;
    }

    const int cols = out_tuple->cell_num();
    vector<Value> values(cols);
    for (int i = 0; i < cols; ++i) {
      RC rc2 = out_tuple->cell_at(i, values[i]);
      if (rc2 != RC::SUCCESS) {
        rc = rc2;
        break;
      }
    }
    if (rc != RC::SUCCESS) break;

    Record record;
    rc = table->make_record(cols, values.data(), record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to make materialized view record. rc=%s", strrc(rc));
      break;
    }
    rc = session->current_trx()->insert_record(table, record);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to insert materialized view record. rc=%s", strrc(rc));
      break;
    }
    inserted_rids.push_back(record.rid());
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }

  physical_oper->close();

  if (rc != RC::SUCCESS) {
    // rollback inserted rows in this statement
    for (auto it = inserted_rids.rbegin(); it != inserted_rids.rend(); ++it) {
      const RID &rid = *it;
      Record rec;
      RC rc_get = table->get_record(rid, rec);
      if (rc_get != RC::SUCCESS) {
        LOG_WARN("failed to get record for materialized view rollback. rid=%s rc=%s", rid.to_string().c_str(), strrc(rc_get));
        continue;
      }
      RC rc_del = session->current_trx()->delete_record(table, rec);
      if (rc_del != RC::SUCCESS) {
        LOG_WARN("failed to delete record for materialized view rollback. rid=%s rc=%s", rid.to_string().c_str(), strrc(rc_del));
      }
    }
    // rollback transaction
    trx->rollback();
  } else {
    // commit the inserts for materialized view immediately (DDL auto-commit semantics)
    RC rc_commit = trx->commit();
    if (rc_commit != RC::SUCCESS) {
      LOG_WARN("failed to commit materialized view inserts. rc=%s", strrc(rc_commit));
      return rc_commit;
    }
  }

  LOG_INFO("Create materialized view success. name=%s", view_name);
  return rc;
}
