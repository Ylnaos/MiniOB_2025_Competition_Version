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
// Created by Wangyunlai on 2022/6/6.
//

#include "sql/stmt/select_stmt.h"
#include "common/lang/string.h"
#include "common/log/log.h"
#include "sql/stmt/filter_stmt.h"
#include "storage/db/db.h"
#include "storage/table/table.h"
#include "sql/parser/expression_binder.h"
#include "sql/parser/parse.h"
#include "storage/view/view.h"
#include "sql/expr/expression.h"

using namespace std;
using namespace common;

// 从视图的 SELECT 列表推导列名到底层字段(表.列)的映射
// 支持两类：
// 1) 未绑定字段（直接映射 列名 -> 表.列）
// 2) 星号(*)：当且仅当能唯一定位到一个底层表时，将该表所有可见列加入映射
static void build_view_output_mapping(
    Db *db,
    const vector<RelationSqlNode> &view_rels,
    const vector<unique_ptr<Expression>> &view_exprs,
    unordered_map<string, pair<string, string>> &name_to_relattr)
{
  name_to_relattr.clear();

  auto add_table_columns = [&](const RelationSqlNode &rel) {
    Table *tbl = db->find_table(rel.relation_name.c_str());
    if (tbl == nullptr) return;  // 底层不是物理表，忽略

    const TableMeta &tm = tbl->table_meta();
    const string     qn = rel.alias.empty() ? rel.relation_name : rel.alias; // 使用别名优先
    for (int i = tm.sys_field_num(); i < tm.field_num(); ++i) {
      const FieldMeta *fm = tm.field(i);
      if (fm == nullptr) continue;
      string key = fm->name();
      common::str_to_lower(key);
      name_to_relattr[key] = {qn, fm->name()};
    }
  };

  for (const auto &expr : view_exprs) {
    if (expr == nullptr) continue;

    if (expr->type() == ExprType::UNBOUND_FIELD) {
      // 结果列名优先使用别名
      string label = expr->alias() && expr->alias()[0] != '\0'
                         ? string(expr->alias())
                         : string(static_cast<UnboundFieldExpr *>(expr.get())->field_name());

      auto *uf = static_cast<UnboundFieldExpr *>(expr.get());
      string key = label;
      common::str_to_lower(key);
      name_to_relattr[key] = {uf->table_name(), uf->field_name()};
    } else if (expr->type() == ExprType::STAR) {
      // 仅当能唯一确定底层表时，展开映射
      const char *star_tbl = static_cast<const StarExpr *>(expr.get())->table_name();
      if (star_tbl != nullptr && star_tbl[0] != '\0') {
        // 优先用别名匹配，否则用表名匹配
        bool matched = false;
        for (const auto &rel : view_rels) {
          if ((!rel.alias.empty() && 0 == strcasecmp(rel.alias.c_str(), star_tbl)) ||
              0 == strcasecmp(rel.relation_name.c_str(), star_tbl)) {
            add_table_columns(rel);
            matched = true;
            break;
          }
        }
        (void)matched; // 未匹配到则忽略
      } else if (view_rels.size() == 1) {
        add_table_columns(view_rels[0]);
      }
    } else {
      // 复杂表达式：只有在有别名时才作为输出列名参与映射（此处不处理）
      if (expr->alias() != nullptr && expr->alias()[0] != '\0') {
        // 无法反推到底层字段，仅占位，避免误将缺失视为未输出列
        string key = expr->alias();
        common::str_to_lower(key);
        // 使用空表名占位，后续 rewrite 找不到再交由 binder 处理
        name_to_relattr.emplace(key, make_pair(string(), string()));
      }
    }
  }
}

// 在绑定前重写外层表达式树中未限定的字段引用：
//   name -> real_table.real_field (依据视图输出列名映射)
static RC rewrite_unqualified_fields(
    unique_ptr<Expression> &expr,
    const unordered_map<string, pair<string, string>> &name_to_relattr)
{
  if (!expr) return RC::SUCCESS;

  switch (expr->type()) {
    case ExprType::UNBOUND_FIELD: {
      auto *uf = static_cast<UnboundFieldExpr *>(expr.get());
      const char *tbl = uf->table_name();
      const char *col = uf->field_name();
      if ((tbl == nullptr || tbl[0] == '\0') && col != nullptr && col[0] != '\0') {
        string key = string(col);
        common::str_to_lower(key);
        auto   it  = name_to_relattr.find(key);
        if (it != name_to_relattr.end()) {
          // 用底层限定字段替换
          unique_ptr<Expression> replaced = make_unique<UnboundFieldExpr>(it->second.first, it->second.second);
          // 继承展示名称(保持简单)：设置为 "table.field"
          string display = it->second.first;
          if (!display.empty()) display += ".";
          display += it->second.second;
          replaced->set_name(display);
          if (expr->alias() != nullptr) {
            replaced->set_alias(expr->alias());
          }
          expr.swap(replaced);
          return RC::SUCCESS;
        } else {
          // 外层引用了视图未输出的列，应报错
          return RC::SCHEMA_FIELD_MISSING;
        }
      }
      return RC::SUCCESS;
    } break;

    case ExprType::UNBOUND_AGGREGATION: {
      auto *agg = static_cast<UnboundAggregateExpr *>(expr.get());
      return rewrite_unqualified_fields(agg->child(), name_to_relattr);
    } break;

    case ExprType::ARITHMETIC: {
      auto *arith = static_cast<ArithmeticExpr *>(expr.get());
      RC rc = rewrite_unqualified_fields(arith->left(), name_to_relattr);
      if (OB_FAIL(rc)) return rc;
      if (arith->right()) {
        rc = rewrite_unqualified_fields(arith->right(), name_to_relattr);
        if (OB_FAIL(rc)) return rc;
      }
      return RC::SUCCESS;
    } break;

    case ExprType::COMPARISON: {
      auto *cmp = static_cast<ComparisonExpr *>(expr.get());
      RC rc = rewrite_unqualified_fields(cmp->left(), name_to_relattr);
      if (OB_FAIL(rc)) return rc;
      rc = rewrite_unqualified_fields(cmp->right(), name_to_relattr);
      return rc;
    } break;

    case ExprType::CONJUNCTION: {
      auto *conj = static_cast<ConjunctionExpr *>(expr.get());
      for (auto &child : conj->children()) {
        RC rc = rewrite_unqualified_fields(child, name_to_relattr);
        if (OB_FAIL(rc)) return rc;
      }
      return RC::SUCCESS;
    } break;

    case ExprType::CAST: {
      auto *c = static_cast<CastExpr *>(expr.get());
      return rewrite_unqualified_fields(c->child(), name_to_relattr);
    } break;

    case ExprType::FUNCTION: {
      auto *fn = static_cast<ScalarFunctionExpr *>(expr.get());
      return rewrite_unqualified_fields(fn->child(), name_to_relattr);
    } break;

    case ExprType::IN_LIST: {
      auto *in = static_cast<InExpr *>(expr.get());
      RC rc = rewrite_unqualified_fields(in->test_expr(), name_to_relattr);
      if (OB_FAIL(rc)) return rc;
      rc = rewrite_unqualified_fields(in->set_expr(), name_to_relattr);
      return rc;
    } break;

    default:
      // 其它类型无需处理或在binder中处理
      return RC::SUCCESS;
  }
}

SelectStmt::~SelectStmt()
{
  if (nullptr != filter_stmt_) {
    delete filter_stmt_;
    filter_stmt_ = nullptr;
  }
}

RC SelectStmt::create(Db *db, SelectSqlNode &select_sql, Stmt *&stmt)
{
  if (nullptr == db) {
    LOG_WARN("invalid argument. db is null");
    return RC::INVALID_ARGUMENT;
  }

  // 视图展开（单表 FROM 视图）：统一展开 FROM/WHERE；
  // - SELECT * 用视图 SELECT 列替换
  // - 其余场景根据视图输出列名映射重写未限定字段：若外层引用视图未输出的列，报错
  if (select_sql.relations.size() == 1) {
    const RelationSqlNode &rel = select_sql.relations[0];
    const char *rel_name = rel.relation_name.c_str();
    if (db->find_table(rel_name) == nullptr) {
      View *view = db->find_view(rel_name);
      if (view != nullptr && rel.alias.empty()) {
        ParsedSqlResult parsed;
        RC parse_rc = parse(view->select_sql(), &parsed);
        if (OB_FAIL(parse_rc) || parsed.sql_nodes().empty()) {
          LOG_WARN("parse view select failed. view=%s", view->name());
          return RC::SQL_SYNTAX;
        }
        ParsedSqlNode *node = parsed.sql_nodes()[0].get();
        if (node->flag != SCF_SELECT) {
          LOG_WARN("view definition is not a SELECT. view=%s", view->name());
          return RC::SQL_SYNTAX;
        }

        // 1) 展开 FROM/WHERE（将视图条件并入外层 WHERE）
        select_sql.relations.swap(node->selection.relations);
        for (auto &cond : node->selection.conditions) {
          select_sql.conditions.emplace_back(std::move(cond));
        }

        // 2) 如果是 SELECT *，用视图 SELECT 列替换
        bool only_star = (select_sql.expressions.size() == 1) &&
                         (select_sql.expressions[0] != nullptr) &&
                         (select_sql.expressions[0]->type() == ExprType::STAR);
        if (only_star) {
          select_sql.expressions.swap(node->selection.expressions);
          // 同步 group/order（如果视图中带有）
          if (!node->selection.group_by.empty() && select_sql.group_by.empty()) {
            select_sql.group_by.swap(node->selection.group_by);
          }
          if (!node->selection.order_by.empty() && select_sql.order_by.empty()) {
            select_sql.order_by.swap(node->selection.order_by);
          }
        } else {
          // 3) 非 * 的情形：根据视图输出列名映射，重写外层未限定字段
          unordered_map<string, pair<string, string>> name_to_relattr;
          build_view_output_mapping(db, node->selection.relations, node->selection.expressions, name_to_relattr);
          for (auto &outer_expr : select_sql.expressions) {
            RC rc = rewrite_unqualified_fields(outer_expr, name_to_relattr);
            if (OB_FAIL(rc)) return rc;
          }
          // group by / order by 同样做一次字段重写，出现未输出列一律报错
          for (auto &gexpr : select_sql.group_by) {
            RC rc = rewrite_unqualified_fields(gexpr, name_to_relattr);
            if (OB_FAIL(rc)) return rc;
          }
          for (auto &item : select_sql.order_by) {
            RC rc = rewrite_unqualified_fields(item.expression, name_to_relattr);
            if (OB_FAIL(rc)) return rc;
          }
        }
      }
    }
  }

  // 绑定阶段
  BinderContext binder_context;

  // 收集 FROM 表
  vector<Table *>                tables;
  unordered_map<string, Table *> table_map;
  for (size_t i = 0; i < select_sql.relations.size(); i++) {
    const RelationSqlNode &r = select_sql.relations[i];
    const char *table_name = r.relation_name.c_str();
    if (nullptr == table_name) {
      LOG_WARN("invalid argument. relation name is null. index=%d", i);
      return RC::INVALID_ARGUMENT;
    }
    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      LOG_WARN("no such table. db=%s, table_name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
    binder_context.add_table(table);
    tables.push_back(table);
    table_map.insert({table_name, table});
    // 别名检查：同层不重复
    if (!r.alias.empty()) {
      if (table_map.find(r.alias) != table_map.end()) {
        LOG_WARN("duplicate table alias in same scope: %s", r.alias.c_str());
        return RC::INVALID_ARGUMENT;
      }
      table_map.insert({r.alias, table});
      binder_context.add_alias(r.alias, table);
    }
  }

  // 绑定 SELECT 列
  vector<unique_ptr<Expression>> bound_expressions;
  ExpressionBinder expression_binder(binder_context);
  for (unique_ptr<Expression> &expression : select_sql.expressions) {
    RC rc = expression_binder.bind_expression(expression, bound_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  // 绑定 GROUP BY
  vector<unique_ptr<Expression>> group_by_expressions;
  for (unique_ptr<Expression> &expression : select_sql.group_by) {
    RC rc = expression_binder.bind_expression(expression, group_by_expressions);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind expression failed. rc=%s", strrc(rc));
      return rc;
    }
  }

  // 绑定 ORDER BY
  vector<pair<unique_ptr<Expression>, bool>> order_by_items;
  for (auto &item : select_sql.order_by) {
    vector<unique_ptr<Expression>> bound;
    RC rc = expression_binder.bind_expression(item.expression, bound);
    if (OB_FAIL(rc)) {
      LOG_INFO("bind order by expression failed. rc=%s", strrc(rc));
      return rc;
    }
    if (bound.size() != 1) {
      LOG_WARN("invalid order by expression size: %d", bound.size());
      return RC::INVALID_ARGUMENT;
    }
    order_by_items.emplace_back(std::move(bound[0]), item.asc);
  }

  // WHERE 过滤：支持两种来源
  // 1) 传统 AND 链（conditions）
  // 2) where_expr（支持 AND/OR 的布尔表达式）
  Table *default_table = nullptr;
  if (tables.size() == 1) {
    default_table = tables[0];
  }
  FilterStmt *filter_stmt = nullptr;
  RC rc = RC::SUCCESS;
  if (select_sql.where_expr) {
    // 使用表达式绑定
    vector<unique_ptr<Expression>> bound;
    RC rc2 = expression_binder.bind_expression(select_sql.where_expr, bound);
    if (OB_FAIL(rc2) || bound.size() != 1) {
      LOG_WARN("bind where boolean expression failed. rc=%s", strrc(rc2));
      return rc2 == RC::SUCCESS ? RC::INVALID_ARGUMENT : rc2;
    }
    // 后续在逻辑阶段接成谓词算子
    // 先占位到 select_stmt 中
    // filter_stmt 为空，表示不使用传统过滤器
  } else {
    rc = FilterStmt::create(db,
        default_table,
        &table_map,
        select_sql.conditions.data(),
        static_cast<int>(select_sql.conditions.size()),
        filter_stmt);
    if (rc != RC::SUCCESS) {
      LOG_WARN("cannot construct filter stmt");
      return rc;
    }
  }

  // 组装 SelectStmt
  SelectStmt *select_stmt = new SelectStmt();
  select_stmt->tables_.swap(tables);
  select_stmt->query_expressions_.swap(bound_expressions);
  select_stmt->filter_stmt_ = filter_stmt;
  select_stmt->group_by_.swap(group_by_expressions);
  select_stmt->order_by_.swap(order_by_items);
  if (select_sql.where_expr) {
    vector<unique_ptr<Expression>> bound;
    RC rc2 = expression_binder.bind_expression(select_sql.where_expr, bound);
    if (OB_FAIL(rc2) || bound.size() != 1) {
      LOG_WARN("bind where boolean expression failed. rc=%s", strrc(rc2));
      return rc2 == RC::SUCCESS ? RC::INVALID_ARGUMENT : rc2;
    }
    select_stmt->where_expr_.reset(bound[0].release());
  }
  
  // 绑定 HAVING（将 AND 串联的条件转为一个布尔表达式树）
  if (!select_sql.having.empty()) {
    vector<unique_ptr<Expression>> cmp_exprs;
    for (auto &cond : select_sql.having) {
      unique_ptr<Expression> left_expr;
      unique_ptr<Expression> right_expr;

      if (cond.left_expr) {
        vector<unique_ptr<Expression>> bound;
        RC rc2 = expression_binder.bind_expression(cond.left_expr, bound);
        if (OB_FAIL(rc2) || bound.size() != 1) {
          LOG_WARN("bind having left expr failed. rc=%s", strrc(rc2));
          return rc2 == RC::SUCCESS ? RC::INVALID_ARGUMENT : rc2;
        }
        left_expr.reset(bound[0].release());
      } else if (cond.left_is_attr == 1) {
        // 不太可能触发（语法上HAVING使用表达式路径），兜底
        FieldExpr *f = new FieldExpr(Field());
        left_expr.reset(f);
      } else {
        left_expr.reset(new ValueExpr(cond.left_value));
      }

      if (cond.right_expr) {
        vector<unique_ptr<Expression>> bound;
        RC rc2 = expression_binder.bind_expression(cond.right_expr, bound);
        if (OB_FAIL(rc2) || bound.size() != 1) {
          LOG_WARN("bind having right expr failed. rc=%s", strrc(rc2));
          return rc2 == RC::SUCCESS ? RC::INVALID_ARGUMENT : rc2;
        }
        right_expr.reset(bound[0].release());
      } else if (cond.right_is_attr == 1) {
        FieldExpr *f = new FieldExpr(Field());
        right_expr.reset(f);
      } else {
        right_expr.reset(new ValueExpr(cond.right_value));
      }

      if (cond.comp == IN_OP || cond.comp == NOT_IN_OP) {
        bool not_in = (cond.comp == NOT_IN_OP);
        cmp_exprs.emplace_back(new InExpr(std::move(left_expr), std::move(right_expr), not_in));
      } else {
        cmp_exprs.emplace_back(new ComparisonExpr(cond.comp, std::move(left_expr), std::move(right_expr)));
      }
    }

    if (!cmp_exprs.empty()) {
      unique_ptr<Expression> having_expr(new ConjunctionExpr(ConjunctionExpr::Type::AND, cmp_exprs));
      select_stmt->having_expr_.swap(having_expr);
    }
  }

  stmt = select_stmt;
  return RC::SUCCESS;
}
