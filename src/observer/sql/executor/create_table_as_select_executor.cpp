/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/executor/create_table_as_select_executor.h"

#include "common/log/log.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "session/session.h"
#include "sql/stmt/create_table_as_select_stmt.h"
#include "sql/stmt/select_stmt.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "sql/operator/physical_operator.h"
#include "sql/expr/expression.h"
#include "storage/db/db.h"
#include "common/types.h"
#include "storage/table/table.h"
#include "common/type/attr_type.h"
#include "common/type/vector_type.h"

using namespace std;
using namespace common;

static void ensure_unique_name(vector<string> &names, string &name)
{
  if (name.empty()) name = "c1"; // basic default
  // sanitize: replace spaces and special chars with '_'
  for (char &ch : name) {
    if (!( (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' )) {
      ch = '_';
    }
  }
  string base = name;
  int    seq  = 1;
  auto   exists = [&](const string &n) {
    for (auto &x : names) {
      if (0 == strcasecmp(x.c_str(), n.c_str())) {
        return true;
      }
    }
    return false;
  };
  while (exists(name)) {
    name = base + string("_") + to_string(++seq);
  }
}

RC CreateTableAsSelectExecutor::execute(SQLStageEvent *sql_event)
{
  Stmt    *stmt    = sql_event->stmt();
  Session *session = sql_event->session_event()->session();
  ASSERT(stmt->type() == StmtType::CREATE_TABLE_AS_SELECT,
      "ctas executor can not run this command: %d",
      static_cast<int>(stmt->type()));

  auto *ctas = static_cast<CreateTableAsSelectStmt *>(stmt);
  Db   *db   = session->get_current_db();
  if (nullptr == db) return RC::INTERNAL;

  SelectStmt *sel = ctas->select_stmt();

  // 1) Derive target schema from SELECT expressions
  vector<AttrInfoSqlNode> attrs;
  vector<string>          names;
  auto &exprs = sel->query_expressions();
  attrs.reserve(exprs.size());
  names.reserve(exprs.size());
  for (size_t i = 0; i < exprs.size(); ++i) {
    Expression *e = exprs[i].get();
    AttrType    t = e->value_type();
    string      name;
    if (e->alias() != nullptr) name = e->alias();
    if (name.empty()) name = e->name();
    if (name.empty() && e->type() == ExprType::FIELD) {
      name = static_cast<FieldExpr *>(e)->field_name();
    }
    if (name.empty()) name = string("c") + to_string(i + 1);
    ensure_unique_name(names, name);
    names.push_back(name);

    size_t len = 4; // default scalar length
    switch (t) {
      case AttrType::INTS: len = sizeof(int); break;
      case AttrType::FLOATS: len = sizeof(float); break;
      case AttrType::DATES: len = sizeof(int); break;
      case AttrType::CHARS: {
        int vlen = e->value_length();
        if (vlen <= 0 || vlen > TEXT_MAX_LENGTH) {
          t   = AttrType::TEXTS;
          len = TEXT_MAX_LENGTH;
        } else {
          len = static_cast<size_t>(vlen);
        }
      } break;
      case AttrType::TEXTS: len = static_cast<size_t>(sizeof(int64_t) + sizeof(int32_t)); break;
      case AttrType::VECTORS: len = static_cast<size_t>(sizeof(int64_t) + sizeof(int32_t)); break;
      case AttrType::BOOLEANS: t = AttrType::INTS; len = sizeof(int); break;
      case AttrType::UNDEFINED:
      case AttrType::NULLS:
      default:
        // fallback to TEXT to preserve content
        t   = AttrType::TEXTS;
        len = TEXT_MAX_LENGTH;
        break;
    }
    AttrInfoSqlNode info;
    info.type = t;
    info.name = name;
    info.length = len;
    info.nullable = true; // allow NULLs by default in CTAS
    attrs.emplace_back(std::move(info));
  }

  // 2) Create target table
  const string &table_name = ctas->table_name();
  RC rc = db->create_table(table_name.c_str(), span<const AttrInfoSqlNode>(attrs.data(), attrs.size()), /*pks*/{}, StorageFormat::ROW_FORMAT);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create table for CTAS. table=%s rc=%s", table_name.c_str(), strrc(rc));
    return rc;
  }
  Table *target = db->find_table(table_name.c_str());
  if (target == nullptr) {
    LOG_ERROR("created table not found: %s", table_name.c_str());
    return RC::INTERNAL;
  }

  // 3) Build and execute SELECT plan, insert rows
  unique_ptr<LogicalOperator>  logical;
  LogicalPlanGenerator         logical_gen;
  rc = logical_gen.create(sel, logical);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create logical plan for CTAS select: %s", strrc(rc));
    return rc;
  }

  unique_ptr<PhysicalOperator> physical;
  PhysicalPlanGenerator        phy_gen;
  rc = phy_gen.create(*logical, physical, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create physical plan for CTAS select: %s", strrc(rc));
    return rc;
  }

  Trx *trx = session->current_trx();
  rc = physical->open(trx);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open CTAS select operator: %s", strrc(rc));
    return rc;
  }

  int inserted = 0;
  while (RC::SUCCESS == (rc = physical->next())) {
    Tuple *tuple = physical->current_tuple();
    if (tuple == nullptr) { rc = RC::INTERNAL; break; }
    int n = tuple->cell_num();
    vector<Value> values;
    values.resize(n);
    for (int i = 0; i < n; ++i) {
      Value v;
      RC rc2 = tuple->cell_at(i, v);
      if (OB_FAIL(rc2)) { rc = rc2; break; }
      values[i] = v;
    }
    if (OB_FAIL(rc)) break;

    Record rec;
    rc = target->make_record(n, values.data(), rec);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to build record for CTAS: %s", strrc(rc));
      break;
    }
    rc = target->insert_record(rec);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to insert record for CTAS: %s", strrc(rc));
      break;
    }
    inserted++;
  }
  RC close_rc = physical->close();
  if (rc == RC::RECORD_EOF) rc = RC::SUCCESS;
  if (rc == RC::SUCCESS && close_rc != RC::SUCCESS) rc = close_rc;

  if (rc == RC::SUCCESS) {
    LOG_INFO("CTAS inserted %d rows into %s", inserted, table_name.c_str());
  }
  return rc;
}
