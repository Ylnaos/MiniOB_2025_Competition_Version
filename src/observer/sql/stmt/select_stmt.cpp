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

// 递归检查表达式树中是否包含聚合函数或未绑定聚合
static bool contains_aggregation(const unique_ptr<Expression> &expr)
{
  if (!expr) return false;

  switch (expr->type()) {
    case ExprType::AGGREGATION:
    case ExprType::UNBOUND_AGGREGATION:
      return true;

    case ExprType::ARITHMETIC: {
      auto *arith = static_cast<ArithmeticExpr *>(expr.get());
      if (contains_aggregation(arith->left())) return true;
      if (arith->right() && contains_aggregation(arith->right())) return true;
      return false;
    }

    case ExprType::COMPARISON: {
      auto *cmp = static_cast<ComparisonExpr *>(expr.get());
      if (contains_aggregation(cmp->left())) return true;
      if (contains_aggregation(cmp->right())) return true;
      return false;
    }

    case ExprType::CONJUNCTION: {
      auto *conj = static_cast<ConjunctionExpr *>(expr.get());
      for (auto &child : conj->children()) {
        if (contains_aggregation(child)) return true;
      }
      return false;
    }

    case ExprType::CAST: {
      auto *c = static_cast<CastExpr *>(expr.get());
      return contains_aggregation(c->child());
    }

    case ExprType::FUNCTION: {
      auto *fn = static_cast<ScalarFunctionExpr *>(expr.get());
      return contains_aggregation(fn->child());
    }

    case ExprType::IN_LIST: {
      auto *in = static_cast<InExpr *>(expr.get());
      if (contains_aggregation(in->test_expr())) return true;
      if (contains_aggregation(in->set_expr())) return true;
      return false;
    }

    default:
      return false;
  }
}

// 从视图的 SELECT 列表推导列名到底层字段(表.列)的映射
// 支持两类：
// 1) 未绑定字段（直接映射 列名 -> 表.列）
// 2) 星号(*)：当且仅当能唯一定位到一个底层表时，将该表所有可见列加入映射
// 3) 复杂表达式：存储表达式副本
static void build_view_output_mapping(
    Db *db,
    const vector<RelationSqlNode> &view_rels,
    const vector<unique_ptr<Expression>> &view_exprs,
    const vector<string> &view_field_names,  // 视图定义的列名（可能为空）
    unordered_map<string, pair<string, string>> &name_to_relattr,
    unordered_map<string, unique_ptr<Expression>> &name_to_expr)
{
  name_to_relattr.clear();
  name_to_expr.clear();

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

  for (size_t i = 0; i < view_exprs.size(); ++i) {
    const auto &expr = view_exprs[i];
    if (expr == nullptr) continue;

    if (expr->type() == ExprType::UNBOUND_FIELD) {
      // 如果视图有定义列名，使用视图定义的列名；否则使用别名或字段名
      string label;
      if (i < view_field_names.size() && !view_field_names[i].empty()) {
        label = view_field_names[i];
      } else {
        label = expr->alias() && expr->alias()[0] != '\0'
                    ? string(expr->alias())
                    : string(static_cast<UnboundFieldExpr *>(expr.get())->field_name());
      }

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
      // 复杂表达式：存储表达式副本,用于后续展开
      // 优先使用视图定义的列名，其次使用别名
      string label;
      if (i < view_field_names.size() && !view_field_names[i].empty()) {
        label = view_field_names[i];
      } else if (expr->alias() != nullptr && expr->alias()[0] != '\0') {
        label = expr->alias();
      }

      if (!label.empty()) {
        string key = label;
        common::str_to_lower(key);
        // 复制表达式用于后续重写
        name_to_expr[key] = expr->copy();
        LOG_DEBUG("Added computed column '%s' to name_to_expr mapping", key.c_str());
      }
    }
  }

  // 移除错误逻辑：不应该自动将底层表的所有列加入映射
  // 视图对外只暴露SELECT列表中明确输出的字段，不应该暴露底层表的其他字段
  // 如果外层查询引用了视图未输出的列，应该报错
  // 原有逻辑会导致类似 "select count(name) from view" 这样的查询错误地访问到底层表的name字段
}

// 在绑定前重写外层表达式树中未限定的字段引用：
//   name -> real_table.real_field (依据视图输出列名映射)
//   或 name -> 计算表达式 (对于复杂表达式)
static RC rewrite_unqualified_fields(
    unique_ptr<Expression> &expr,
    const unordered_map<string, pair<string, string>> &name_to_relattr,
    const unordered_map<string, unique_ptr<Expression>> &name_to_expr)
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

        // 优先查找表达式映射(计算列)
        auto expr_it = name_to_expr.find(key);
        if (expr_it != name_to_expr.end()) {
          LOG_DEBUG("Found computed column '%s' in name_to_expr mapping", key.c_str());
          // 用视图中的计算表达式替换
          unique_ptr<Expression> replaced = expr_it->second->copy();
          if (expr->alias() != nullptr) {
            replaced->set_alias(expr->alias());
          }
          expr.swap(replaced);
          // 递归重写替换后表达式内部的未限定字段
          return rewrite_unqualified_fields(expr, name_to_relattr, name_to_expr);
        }

        // 其次查找字段映射(简单字段)
        auto field_it = name_to_relattr.find(key);
        if (field_it != name_to_relattr.end()) {
          // 用底层限定字段替换
          unique_ptr<Expression> replaced = make_unique<UnboundFieldExpr>(field_it->second.first, field_it->second.second);
          // 继承展示名称(保持简单)：设置为 "table.field"
          string display = field_it->second.first;
          if (!display.empty()) display += ".";
          display += field_it->second.second;
          replaced->set_name(display);
          if (expr->alias() != nullptr) {
            replaced->set_alias(expr->alias());
          }
          expr.swap(replaced);
          return RC::SUCCESS;
        }

        // 外层引用了视图未输出的列，应报错
        return RC::SCHEMA_FIELD_MISSING;
      }
      return RC::SUCCESS;
    } break;

    case ExprType::UNBOUND_AGGREGATION: {
      auto *agg = static_cast<UnboundAggregateExpr *>(expr.get());
      RC rc = rewrite_unqualified_fields(agg->child(), name_to_relattr, name_to_expr);
      // 如果子表达式重写失败（如字段不存在），应该正确传递错误
      return rc;
    } break;

    case ExprType::ARITHMETIC: {
      auto *arith = static_cast<ArithmeticExpr *>(expr.get());
      LOG_DEBUG("Rewriting arithmetic expression");
      RC rc = rewrite_unqualified_fields(arith->left(), name_to_relattr, name_to_expr);
      if (OB_FAIL(rc)) {
        LOG_WARN("Failed to rewrite left expression of arithmetic");
        return rc;
      }
      if (arith->right()) {
        rc = rewrite_unqualified_fields(arith->right(), name_to_relattr, name_to_expr);
        if (OB_FAIL(rc)) {
          LOG_WARN("Failed to rewrite right expression of arithmetic");
          return rc;
        }
      }
      return RC::SUCCESS;
    } break;

    case ExprType::COMPARISON: {
      auto *cmp = static_cast<ComparisonExpr *>(expr.get());
      RC rc = rewrite_unqualified_fields(cmp->left(), name_to_relattr, name_to_expr);
      if (OB_FAIL(rc)) return rc;
      rc = rewrite_unqualified_fields(cmp->right(), name_to_relattr, name_to_expr);
      return rc;
    } break;

    case ExprType::CONJUNCTION: {
      auto *conj = static_cast<ConjunctionExpr *>(expr.get());
      for (auto &child : conj->children()) {
        RC rc = rewrite_unqualified_fields(child, name_to_relattr, name_to_expr);
        if (OB_FAIL(rc)) return rc;
      }
      return RC::SUCCESS;
    } break;

    case ExprType::CAST: {
      auto *c = static_cast<CastExpr *>(expr.get());
      return rewrite_unqualified_fields(c->child(), name_to_relattr, name_to_expr);
    } break;

    case ExprType::FUNCTION: {
      auto *fn = static_cast<ScalarFunctionExpr *>(expr.get());
      return rewrite_unqualified_fields(fn->child(), name_to_relattr, name_to_expr);
    } break;

    case ExprType::IN_LIST: {
      auto *in = static_cast<InExpr *>(expr.get());
      RC rc = rewrite_unqualified_fields(in->test_expr(), name_to_relattr, name_to_expr);
      if (OB_FAIL(rc)) return rc;
      rc = rewrite_unqualified_fields(in->set_expr(), name_to_relattr, name_to_expr);
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
  if (nullptr != inner_view_stmt_) {
    delete inner_view_stmt_;
    inner_view_stmt_ = nullptr;
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
  // - 特殊情况:如果视图包含聚合函数,则不能展开(否则聚合语义会丢失)
  if (select_sql.relations.size() == 1) {
    const RelationSqlNode &rel = select_sql.relations[0];
    const char *rel_name = rel.relation_name.c_str();
    if (db->find_table(rel_name) == nullptr) {
      View *view = db->find_view(rel_name);
      if (view != nullptr && rel.alias.empty()) {
        ParsedSqlResult parsed;
        LOG_DEBUG("Parsing view SELECT SQL: %s", view->select_sql());
        RC parse_rc = parse(view->select_sql(), &parsed);
        if (OB_FAIL(parse_rc) || parsed.sql_nodes().empty()) {
          LOG_WARN("parse view select failed. view=%s, sql=%s", view->name(), view->select_sql());
          return RC::SQL_SYNTAX;
        }
        ParsedSqlNode *node = parsed.sql_nodes()[0].get();
        if (node->flag != SCF_SELECT) {
          LOG_WARN("view definition is not a SELECT. view=%s", view->name());
          return RC::SQL_SYNTAX;
        }

        // [DEBUG] 检查解析后的WHERE条件 - 使用ERROR级别确保输出
        LOG_ERROR("[TRACE] Parsed node: conditions.size()=%zu, where_expr=%s, relations.size()=%zu",
            node->selection.conditions.size(),
            node->selection.where_expr ? "EXISTS" : "NULL",
            node->selection.relations.size());

        // 检查视图定义中是否包含聚合函数或GROUP BY
        bool view_has_aggregation = false;
        for (const auto &expr : node->selection.expressions) {
          if (contains_aggregation(expr)) {
            view_has_aggregation = true;
            break;
          }
        }
        if (!view_has_aggregation && !node->selection.group_by.empty()) {
          view_has_aggregation = true;
        }

        // 如果视图包含聚合,则不展开
        // 对于这种情况,需要将外层查询转换为对视图结果的查询
        // 使用通用的子查询机制处理
        if (view_has_aggregation) {
          LOG_DEBUG("View '%s' contains aggregation, using subquery mechanism", view->name());
          LOG_DEBUG("[DEBUG] View SQL: %s", view->select_sql());
          LOG_DEBUG("Outer SELECT expression count: %d", static_cast<int>(select_sql.expressions.size()));
          for (size_t idx = 0; idx < select_sql.expressions.size(); idx++) {
            Expression *expr_ptr = select_sql.expressions[idx].get();
            LOG_DEBUG("Outer SELECT expression[%d] initial type=%d",
                static_cast<int>(idx),
                expr_ptr ? static_cast<int>(expr_ptr->type()) : -1);
          }

          // 1. 创建视图的SelectStmt作为内层子查询
          Stmt *inner_stmt = nullptr;
          RC rc = SelectStmt::create(db, node->selection, inner_stmt);
          if (OB_FAIL(rc)) {
            LOG_WARN("Failed to create inner SelectStmt for aggregate view '%s'", view->name());
            return rc;
          }
          SelectStmt *inner_select = static_cast<SelectStmt *>(inner_stmt);

          // [TRACE] 检查内层SelectStmt的query_expressions和WHERE条件 - 使用ERROR确保输出
          LOG_ERROR("[TRACE] Inner SelectStmt created, query_expressions=%zu, filter_stmt=%s, where_expr=%s, tables=%zu",
              inner_select->query_expressions().size(),
              inner_select->filter_stmt() ? "EXISTS" : "NULL",
              inner_select->where_expr() ? "EXISTS" : "NULL",
              inner_select->tables().size());
          if (inner_select->filter_stmt()) {
            LOG_ERROR("[TRACE]   filter_stmt has %zu filter units",
                inner_select->filter_stmt()->filter_units().size());
          }

          // [CRITICAL FIX] 检查内层查询的WHERE条件是否丢失
          if (!inner_select->filter_stmt() && !inner_select->where_expr()) {
            LOG_ERROR("[CRITICAL] Inner SelectStmt has NO WHERE conditions!");
            LOG_ERROR("[CRITICAL] This will cause Cartesian product in aggregation!");
            LOG_ERROR("[CRITICAL] View SQL was: %s", view->select_sql());
            LOG_ERROR("[CRITICAL] View has %zu tables", inner_select->tables().size());
            // 这是一个严重错误，说明WHERE条件在解析时丢失了
            // 暂时返回错误，避免返回错误结果
            delete inner_select;
            return RC::INTERNAL;
          }

          // 2. 创建外层SelectStmt,将内层查询设置为子查询数据源
          SelectStmt *outer_select = new SelectStmt();
          outer_select->set_inner_view_stmt(inner_select);
          LOG_DEBUG("Created outer SelectStmt %p with inner view stmt %p",
              static_cast<void *>(outer_select),
              static_cast<void *>(inner_select));

          // 3. 处理外层的表达式：将UNBOUND_AGGREGATION转换为AggregateExpr
          // 这是必需的，因为执行器需要明确的AggregateExpr类型
          vector<unique_ptr<Expression>> bound_exprs;
          int expr_idx = 0;
          for (auto &expr : select_sql.expressions) {
            unique_ptr<Expression> copied = expr->copy();
            if (copied->type() == ExprType::STAR) {
              const auto &inner_exprs = inner_select->query_expressions();
              LOG_DEBUG("Expanding STAR for aggregate view: inner column size=%zu", inner_exprs.size());
              for (size_t inner_idx = 0; inner_idx < inner_exprs.size(); inner_idx++) {
                const auto &inner_expr = inner_exprs[inner_idx];
                if (!inner_expr) {
                  continue;
                }
                const char *label = inner_expr->alias();
                if (is_blank(label)) {
                  label = inner_expr->name();
                }
                string column_name;
                if (!is_blank(label)) {
                  column_name = label;
                } else {
                  column_name = string("COLUMN_") + std::to_string(inner_idx + 1);
                }
                common::str_to_upper(column_name);
                auto expanded = make_unique<UnboundFieldExpr>("", column_name);
                expanded->set_name(column_name);
                LOG_DEBUG("  STAR expands to column '%s' (index=%zu)", column_name.c_str(), inner_idx);
                bound_exprs.emplace_back(std::move(expanded));
              }
            } else if (copied->type() == ExprType::UNBOUND_AGGREGATION) {
              auto *uagg = static_cast<UnboundAggregateExpr *>(copied.get());
              AggregateExpr::Type agg_type;
              RC rc2 = AggregateExpr::type_from_string(uagg->aggregate_name(), agg_type);
              if (OB_FAIL(rc2)) {
                delete inner_select;
                delete outer_select;
                return rc2;
              }
              // 将 count(*) 的子表达式从 STAR 改写为常量 1
              LOG_DEBUG("Converting outer expression[%d] UNBOUND_AGG '%s' to AggregateExpr type=%d",
                  expr_idx,
                  uagg->aggregate_name(),
                  static_cast<int>(agg_type));
              unique_ptr<Expression> child;
              if (agg_type == AggregateExpr::Type::COUNT && uagg->child()->type() == ExprType::STAR) {
                child.reset(new ValueExpr(Value(1)));
              } else {
                child = uagg->child()->copy();
              }
              unique_ptr<Expression> agg_expr = make_unique<AggregateExpr>(agg_type, std::move(child));
              // 设置聚合表达式的名字
              // 如果原始表达式有名字，使用它；否则生成默认名字 (如 "COUNT(*)")
              const char *name = uagg->name();
              if (name == nullptr || name[0] == '\0') {
                // 生成默认名字，格式: "AGGREGATE_NAME(*)"
                string default_name = string(uagg->aggregate_name()) + "(*)";
                agg_expr->set_name(default_name);  // set_name 接受 string，会自动复制
                LOG_DEBUG("  Generated default name '%s' for aggregate expression", default_name.c_str());
              } else {
                agg_expr->set_name(string(name));
              }
              if (uagg->alias()) agg_expr->set_alias(uagg->alias());
              bound_exprs.push_back(std::move(agg_expr));
            } else {
              bound_exprs.push_back(std::move(copied));
            }
            expr_idx++;
          }
          vector<unique_ptr<Expression>> final_exprs;
          if (!bound_exprs.empty()) {
            BinderContext subquery_binder_context;
            subquery_binder_context.set_inner_view_stmt(inner_select);
            ExpressionBinder subquery_binder(subquery_binder_context);
            for (auto &candidate : bound_exprs) {
              if (candidate != nullptr && candidate->type() == ExprType::UNBOUND_FIELD) {
                // 绑定 UNBOUND_FIELD（如 SELECT * 展开的列）
                vector<unique_ptr<Expression>> tmp;
                RC bind_rc = subquery_binder.bind_expression(candidate, tmp);
                if (OB_FAIL(bind_rc) || tmp.size() != 1) {
                  LOG_WARN("Failed to bind STAR expanded column for aggregate view. rc=%s, size=%zu",
                      strrc(bind_rc), tmp.size());
                  delete inner_select;
                  delete outer_select;
                  return bind_rc == RC::SUCCESS ? RC::INVALID_ARGUMENT : bind_rc;
                }
                final_exprs.emplace_back(std::move(tmp[0]));
              } else if (candidate != nullptr && candidate->type() == ExprType::AGGREGATION) {
                // 绑定 AGGREGATION 的子表达式（如 SUM(num) 中的 num）
                auto *agg = static_cast<AggregateExpr *>(candidate.get());
                vector<unique_ptr<Expression>> tmp;
                RC bind_rc = subquery_binder.bind_expression(agg->child(), tmp);
                if (OB_FAIL(bind_rc) || tmp.size() != 1) {
                  LOG_WARN("Failed to bind aggregate child expression for view. rc=%s, size=%zu",
                      strrc(bind_rc), tmp.size());
                  delete inner_select;
                  delete outer_select;
                  return bind_rc == RC::SUCCESS ? RC::INVALID_ARGUMENT : bind_rc;
                }
                // 用绑定后的表达式替换聚合函数的子表达式
                agg->child().reset(tmp[0].release());
                LOG_DEBUG("Successfully bound aggregate expression child for view");
                final_exprs.emplace_back(std::move(candidate));
              } else {
                final_exprs.emplace_back(std::move(candidate));
              }
            }
          }
          outer_select->query_expressions().swap(final_exprs);

          // 验证外层SelectStmt设置正确
          LOG_DEBUG("Outer SelectStmt configured: query_exprs=%d, has_aggregation=%s",
              static_cast<int>(outer_select->query_expressions().size()),
              outer_select->query_expressions().empty() ? "NO" : "YES");
          for (size_t i = 0; i < outer_select->query_expressions().size(); i++) {
            const auto &expr = outer_select->query_expressions()[i];
            LOG_DEBUG("  expr[%d]: type=%d name=%s",
                static_cast<int>(i),
                static_cast<int>(expr->type()),
                expr->name() ? expr->name() : "(null)");
          }

          stmt = outer_select;
          return RC::SUCCESS;
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
          unordered_map<string, unique_ptr<Expression>> name_to_expr;
          // 注意：上面已经将视图内部的 relation 列表 swap 到 select_sql.relations 中
          // 此处必须使用新的 relations，否则会拿到原外层的视图名，导致无法建立字段映射
          // 获取视图的定义列名
          const vector<string> &view_fields = view->view_fields();
          build_view_output_mapping(db, select_sql.relations, node->selection.expressions, view_fields, name_to_relattr, name_to_expr);
          for (auto &outer_expr : select_sql.expressions) {
            RC rc = rewrite_unqualified_fields(outer_expr, name_to_relattr, name_to_expr);
            if (OB_FAIL(rc)) return rc;
          }
          // group by / order by 同样做一次字段重写，出现未输出列一律报错
          for (auto &gexpr : select_sql.group_by) {
            RC rc = rewrite_unqualified_fields(gexpr, name_to_relattr, name_to_expr);
            if (OB_FAIL(rc)) return rc;
          }
          for (auto &item : select_sql.order_by) {
            RC rc = rewrite_unqualified_fields(item.expression, name_to_relattr, name_to_expr);
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
  vector<string>                 table_aliases;
  table_aliases.reserve(select_sql.relations.size());
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
    string alias_token;
    if (!r.alias.empty()) {
      alias_token = r.alias;
    } else {
      alias_token = r.relation_name;
    }
    common::str_to_upper(alias_token);
    table_aliases.push_back(alias_token);
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
  select_stmt->table_aliases_.swap(table_aliases);
  select_stmt->query_expressions_.swap(bound_expressions);
  select_stmt->filter_stmt_ = filter_stmt;
  select_stmt->group_by_.swap(group_by_expressions);
  select_stmt->order_by_.swap(order_by_items);
  select_stmt->limit_ = select_sql.limit;
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
