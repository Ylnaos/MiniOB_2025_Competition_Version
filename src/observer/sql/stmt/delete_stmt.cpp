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

#include "sql/stmt/delete_stmt.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "storage/view/view.h"
#include "sql/expr/expression.h"
#include "sql/parser/parse.h"

namespace {

ConditionSqlNode clone_condition(const ConditionSqlNode &condition)
{
  ConditionSqlNode cloned;
  cloned.left_is_attr  = condition.left_is_attr;
  cloned.left_value    = condition.left_value;
  cloned.left_attr     = condition.left_attr;
  cloned.comp          = condition.comp;
  cloned.right_is_attr = condition.right_is_attr;
  cloned.right_attr    = condition.right_attr;
  cloned.right_value   = condition.right_value;
  if (condition.left_expr) {
    cloned.left_expr = condition.left_expr->copy();
  }
  if (condition.right_expr) {
    cloned.right_expr = condition.right_expr->copy();
  }
  return cloned;
}

RC append_filter_conditions(Expression &expr, vector<ConditionSqlNode> &conditions)
{
  if (expr.type() == ExprType::COMPARISON) {
    auto *cmp = static_cast<ComparisonExpr *>(&expr);
    ConditionSqlNode condition;
    condition.left_expr   = cmp->left()->copy();
    condition.right_expr  = cmp->right()->copy();
    condition.left_is_attr  = -1;
    condition.right_is_attr = -1;
    condition.comp          = cmp->comp();
    conditions.emplace_back(std::move(condition));
    return RC::SUCCESS;
  }

  if (expr.type() == ExprType::CONJUNCTION) {
    auto *conj = static_cast<ConjunctionExpr *>(&expr);
    if (conj->conjunction_type() != ConjunctionExpr::Type::AND) {
      return RC::UNSUPPORTED;
    }
    for (const auto &child : conj->children()) {
      RC rc = append_filter_conditions(*child, conditions);
      if (OB_FAIL(rc)) {
        return rc;
      }
    }
    return RC::SUCCESS;
  }

  return RC::UNSUPPORTED;
}

}  // namespace

DeleteStmt::DeleteStmt(Table *table, FilterStmt *filter_stmt) : table_(table), filter_stmt_(filter_stmt) {}

DeleteStmt::~DeleteStmt()
{
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

RC DeleteStmt::create(Db *db, const DeleteSqlNode &delete_sql, Stmt *&stmt)
{
  const char *table_name = delete_sql.relation_name.c_str();
  if (nullptr == db || nullptr == table_name) {
    LOG_WARN("invalid argument. db=%p, table_name=%p", db, table_name);
    return RC::INVALID_ARGUMENT;
  }

  // check whether the table exists
  Table *table = db->find_table(table_name);
  string view_relation_name;
  string view_relation_alias;
  unique_ptr<Expression> view_filter_expr;
  if (nullptr == table) {
    // 尝试查找视图
    View *view = db->find_view(table_name);
    if (view != nullptr) {
      // 解析视图定义获取底层表
      ParsedSqlResult parsed;
      RC parse_rc = parse(view->select_sql(), &parsed);
      if (OB_FAIL(parse_rc) || parsed.sql_nodes().empty()) {
        LOG_WARN("failed to parse view definition for delete. view=%s", table_name);
        return RC::SQL_SYNTAX;
      }

      ParsedSqlNode *node = parsed.sql_nodes()[0].get();
      if (node->flag != SCF_SELECT) {
        LOG_WARN("view definition is not SELECT for delete. view=%s", table_name);
        return RC::UNSUPPORTED;
      }

      SelectSqlNode &select_node = node->selection;

      // 只支持单表简单视图的删除
      if (select_node.relations.size() != 1) {
        LOG_WARN("delete from multi-table view not supported. view=%s", table_name);
        return RC::UNSUPPORTED;
      }

      // 获取底层物理表
      const char *base_table_name = select_node.relations[0].relation_name.c_str();
      table = db->find_table(base_table_name);
      if (table == nullptr) {
        LOG_WARN("base table not found for view. view=%s, base_table=%s",
                 table_name, base_table_name);
        return RC::SCHEMA_TABLE_NOT_EXIST;
      }

      view_relation_name = select_node.relations[0].relation_name;
      view_relation_alias = select_node.relations[0].alias;
      if (select_node.where_expr) {
        view_filter_expr = select_node.where_expr->copy();
      }

      LOG_INFO("rewrite delete on view(%s) to base table(%s)", table_name, table->name());
    }

    if (table == nullptr) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
  }

  unordered_map<string, Table *> table_map;
  table_map.insert(pair<string, Table *>(string(table_name), table));
  table_map.insert(pair<string, Table *>(string(table->name()), table));
  if (!view_relation_name.empty()) {
    table_map.insert(pair<string, Table *>(view_relation_name, table));
  }
  if (!view_relation_alias.empty()) {
    table_map.insert(pair<string, Table *>(view_relation_alias, table));
  }

  vector<ConditionSqlNode> conditions;
  conditions.reserve(delete_sql.conditions.size() + (view_filter_expr ? 1 : 0));
  for (const ConditionSqlNode &condition : delete_sql.conditions) {
    conditions.emplace_back(clone_condition(condition));
  }
  if (view_filter_expr) {
    RC append_rc = append_filter_conditions(*view_filter_expr, conditions);
    if (OB_FAIL(append_rc)) {
      return append_rc;
    }
  }

  FilterStmt *filter_stmt = nullptr;
  RC          rc          = FilterStmt::create(
      db, table, &table_map, conditions.data(), static_cast<int>(conditions.size()), filter_stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create filter statement. rc=%d:%s", rc, strrc(rc));
    return rc;
  }

  stmt = new DeleteStmt(table, filter_stmt);
  return rc;
}
