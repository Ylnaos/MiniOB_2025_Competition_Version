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

static unique_ptr<Expression> condition_operand_to_expr(const ConditionSqlNode &condition, bool left)
{
  const unique_ptr<Expression> &expr = left ? condition.left_expr : condition.right_expr;
  if (expr) {
    return expr->copy();
  }

  const int is_attr = left ? condition.left_is_attr : condition.right_is_attr;
  if (is_attr == 1) {
    const RelAttrSqlNode &attr = left ? condition.left_attr : condition.right_attr;
    return make_unique<UnboundFieldExpr>(attr.relation_name, attr.attribute_name);
  }

  const Value &value = left ? condition.left_value : condition.right_value;
  return make_unique<ValueExpr>(value);
}

static unique_ptr<Expression> condition_to_expr(const ConditionSqlNode &condition)
{
  unique_ptr<Expression> left  = condition_operand_to_expr(condition, true);
  unique_ptr<Expression> right = condition_operand_to_expr(condition, false);
  if (condition.comp == IN_OP || condition.comp == NOT_IN_OP) {
    return make_unique<InExpr>(std::move(left), std::move(right), condition.comp == NOT_IN_OP);
  }
  return make_unique<ComparisonExpr>(condition.comp, std::move(left), std::move(right));
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
          // 用视图中的计算表达式替换
          unique_ptr<Expression> replaced = expr_it->second->copy();
          if (expr->alias() != nullptr) {
            replaced->set_alias(expr->alias());
          }
          expr.swap(replaced);
          return RC::SUCCESS;
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
    string view_alias = rel.alias;  // 保存视图的别名（如果有）
    if (db->find_table(rel_name) == nullptr) {
      View *view = db->find_view(rel_name);
      if (view != nullptr) {  // 移除 rel.alias.empty() 限制，支持带别名的视图展开
        ParsedSqlResult parsed;
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
          // 1. 创建视图的SelectStmt作为内层子查询
          Stmt *inner_stmt = nullptr;
          RC rc = SelectStmt::create(db, node->selection, inner_stmt);
          if (OB_FAIL(rc)) {
            LOG_WARN("Failed to create inner SelectStmt for aggregate view '%s'", view->name());
            return rc;
          }
          unique_ptr<SelectStmt> inner_select_guard(static_cast<SelectStmt *>(inner_stmt));
          SelectStmt *inner_select = inner_select_guard.get();

          // 2. 创建外层SelectStmt,将内层查询设置为子查询数据源
          unique_ptr<SelectStmt> outer_select_guard(new SelectStmt());
          SelectStmt *outer_select = outer_select_guard.get();
          outer_select->set_inner_view_stmt(inner_select);
          inner_select_guard.release(); // 所有权交给 outer_select

          // 3. 处理外层的表达式：将UNBOUND_AGGREGATION转换为AggregateExpr
          // 这是必需的，因为执行器需要明确的AggregateExpr类型
          vector<unique_ptr<Expression>> bound_exprs;
          for (auto &expr : select_sql.expressions) {
            unique_ptr<Expression> copied = expr->copy();
            if (copied->type() == ExprType::STAR) {
              const auto &inner_exprs = inner_select->query_expressions();
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
                bound_exprs.emplace_back(std::move(expanded));
              }
            } else if (copied->type() == ExprType::UNBOUND_AGGREGATION) {
              auto *uagg = static_cast<UnboundAggregateExpr *>(copied.get());
              AggregateExpr::Type agg_type;
              RC rc2 = AggregateExpr::type_from_string(uagg->aggregate_name(), agg_type);
              if (OB_FAIL(rc2)) {
                return rc2;
              }
              // 将 count(*) 的子表达式从 STAR 改写为常量 1
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
              } else {
                agg_expr->set_name(string(name));
              }
              if (uagg->alias()) agg_expr->set_alias(uagg->alias());
              bound_exprs.push_back(std::move(agg_expr));
            } else {
              bound_exprs.push_back(std::move(copied));
            }

          }
          BinderContext subquery_binder_context;
          subquery_binder_context.set_inner_view_stmt(inner_select);
          ExpressionBinder subquery_binder(subquery_binder_context);

          vector<unique_ptr<Expression>> final_exprs;
          if (!bound_exprs.empty()) {
            for (auto &candidate : bound_exprs) {
              if (candidate != nullptr && candidate->type() == ExprType::UNBOUND_FIELD) {
                // 绑定 UNBOUND_FIELD（如 SELECT * 展开的列）
                vector<unique_ptr<Expression>> tmp;
                RC bind_rc = subquery_binder.bind_expression(candidate, tmp);
                if (OB_FAIL(bind_rc) || tmp.size() != 1) {
                  LOG_WARN("Failed to bind STAR expanded column for aggregate view. rc=%s, size=%zu",
                      strrc(bind_rc), tmp.size());
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
                  return bind_rc == RC::SUCCESS ? RC::INVALID_ARGUMENT : bind_rc;
                }
                // 用绑定后的表达式替换聚合函数的子表达式
                agg->child().reset(tmp[0].release());
                final_exprs.emplace_back(std::move(candidate));
              } else {
                final_exprs.emplace_back(std::move(candidate));
              }
            }
          }
          outer_select->query_expressions().swap(final_exprs);
          outer_select->limit_ = select_sql.limit;

          vector<unique_ptr<Expression>> predicate_exprs;
          predicate_exprs.reserve(select_sql.conditions.size() + (select_sql.where_expr ? 1 : 0));
          for (const ConditionSqlNode &condition : select_sql.conditions) {
            predicate_exprs.emplace_back(condition_to_expr(condition));
          }
          if (select_sql.where_expr) {
            predicate_exprs.emplace_back(std::move(select_sql.where_expr));
          }

          if (!predicate_exprs.empty()) {
            unique_ptr<Expression> predicate_expr;
            if (predicate_exprs.size() == 1) {
              predicate_expr = std::move(predicate_exprs[0]);
            } else {
              predicate_expr = make_unique<ConjunctionExpr>(ConjunctionExpr::Type::AND, std::move(predicate_exprs));
            }

            subquery_binder_context.set_binding_context(BinderContext::BindingContext::WHERE);
            vector<unique_ptr<Expression>> bound_predicates;
            RC bind_rc = subquery_binder.bind_expression(predicate_expr, bound_predicates);
            subquery_binder_context.set_binding_context(BinderContext::BindingContext::SELECT);
            if (OB_FAIL(bind_rc) || bound_predicates.size() != 1) {
              LOG_WARN("Failed to bind outer predicate for aggregate view. rc=%s, size=%zu",
                  strrc(bind_rc), bound_predicates.size());
              return bind_rc == RC::SUCCESS ? RC::INVALID_ARGUMENT : bind_rc;
            }
            outer_select->where_expr_.reset(bound_predicates[0].release());
          }

          stmt = outer_select_guard.release();
          return RC::SUCCESS;
        }

        // 1) 展开 FROM。外层 WHERE/GROUP/ORDER 仍按视图输出列解析，需先重写，再并入视图自身条件。
        select_sql.relations.swap(node->selection.relations);
        // 如果外层视图有别名，且视图内部只有一个表，将别名赋给这个表
        if (!view_alias.empty() && select_sql.relations.size() == 1) {
          select_sql.relations[0].alias = view_alias;
          LOG_INFO("Assigned view alias '%s' to inner table '%s'",
                   view_alias.c_str(), select_sql.relations[0].relation_name.c_str());
        }

        unordered_map<string, pair<string, string>> name_to_relattr;
        unordered_map<string, unique_ptr<Expression>> name_to_expr;
        const vector<string> &view_fields = view->view_fields();
        build_view_output_mapping(
            db, select_sql.relations, node->selection.expressions, view_fields, name_to_relattr, name_to_expr);

        // 2) 如果是 SELECT *，用视图 SELECT 列替换
        bool only_star = (select_sql.expressions.size() == 1) &&
                         (select_sql.expressions[0] != nullptr) &&
                         (select_sql.expressions[0]->type() == ExprType::STAR);
        if (only_star) {
          for (auto &gexpr : select_sql.group_by) {
            RC rc = rewrite_unqualified_fields(gexpr, name_to_relattr, name_to_expr);
            if (OB_FAIL(rc)) return rc;
          }
          for (auto &item : select_sql.order_by) {
            RC rc = rewrite_unqualified_fields(item.expression, name_to_relattr, name_to_expr);
            if (OB_FAIL(rc)) return rc;
          }
          if (select_sql.where_expr) {
            RC rc = rewrite_unqualified_fields(select_sql.where_expr, name_to_relattr, name_to_expr);
            if (OB_FAIL(rc)) {
              LOG_WARN("Failed to rewrite WHERE condition for view '%s'", view->name());
              return rc;
            }
          }
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
          // 重写WHERE条件中的字段
          if (select_sql.where_expr) {
            RC rc = rewrite_unqualified_fields(select_sql.where_expr, name_to_relattr, name_to_expr);
            if (OB_FAIL(rc)) {
              LOG_WARN("Failed to rewrite WHERE condition for view '%s'", view->name());
              return rc;
            }
          }
        }

        for (auto &cond : node->selection.conditions) {
          select_sql.conditions.emplace_back(std::move(cond));
        }
        // 处理where_expr：如果视图有where_expr，需要与已重写的外层WHERE合并
        if (node->selection.where_expr) {
          if (select_sql.where_expr) {
            vector<unique_ptr<Expression>> children;
            children.push_back(std::move(select_sql.where_expr));
            children.push_back(std::move(node->selection.where_expr));
            select_sql.where_expr = make_unique<ConjunctionExpr>(ConjunctionExpr::Type::AND, std::move(children));
          } else {
            select_sql.where_expr = std::move(node->selection.where_expr);
          }
        }
      }
    }
  }

  // 循环展开嵌套视图（支持 view on view）
  // 示例：create view v8 as select ... from v6; 其中v6也是视图
  const int MAX_VIEW_RECURSION_DEPTH = 10;  // 防止循环引用导致无限递归
  for (int view_depth = 0; view_depth < MAX_VIEW_RECURSION_DEPTH; view_depth++) {
    // 只处理单表FROM的情况（支持带别名的视图）
    if (select_sql.relations.size() != 1) {
      break;
    }

    string nested_view_alias = select_sql.relations[0].alias;  // 保存别名以传递给内部表
    const char *rel_name = select_sql.relations[0].relation_name.c_str();
    // 如果是物理表，退出循环
    if (db->find_table(rel_name) != nullptr) {
      break;
    }

    // 尝试查找视图
    View *nested_view = db->find_view(rel_name);
    if (nested_view == nullptr) {
      break;  // 既不是表也不是视图，退出循环（后续会报错）
    }

    LOG_INFO("Expanding nested view: %s (depth=%d)", nested_view->name(), view_depth + 1);

    // 解析嵌套视图的SQL
    ParsedSqlResult parsed;
    RC parse_rc = parse(nested_view->select_sql(), &parsed);
    if (OB_FAIL(parse_rc) || parsed.sql_nodes().empty()) {
      LOG_WARN("parse nested view select failed. view=%s, sql=%s", nested_view->name(), nested_view->select_sql());
      return RC::SQL_SYNTAX;
    }
    ParsedSqlNode *node = parsed.sql_nodes()[0].get();
    if (node->flag != SCF_SELECT) {
      LOG_WARN("nested view definition is not a SELECT. view=%s", nested_view->name());
      return RC::SQL_SYNTAX;
    }

    // 检查嵌套视图是否包含聚合函数或GROUP BY
    bool nested_view_has_aggregation = false;
    for (const auto &expr : node->selection.expressions) {
      if (contains_aggregation(expr)) {
        nested_view_has_aggregation = true;
        break;
      }
    }
    if (!nested_view_has_aggregation && !node->selection.group_by.empty()) {
      nested_view_has_aggregation = true;
    }

    // 如果嵌套视图包含聚合，不能继续平展开，需要作为子查询处理
    // 退出循环，后续会在表收集阶段将其作为聚合视图处理
    if (nested_view_has_aggregation) {
      LOG_INFO("Nested view '%s' contains aggregation, stop flattening", nested_view->name());
      break;
    }

    // 展开嵌套视图的 FROM/WHERE
    select_sql.relations.swap(node->selection.relations);
    // 如果外层视图有别名，且嵌套视图内部只有一个表，将别名传递给这个表
    if (!nested_view_alias.empty() && select_sql.relations.size() == 1) {
      select_sql.relations[0].alias = nested_view_alias;
      LOG_INFO("Passed nested view alias '%s' to inner table '%s'",
               nested_view_alias.c_str(), select_sql.relations[0].relation_name.c_str());
    }
    for (auto &cond : node->selection.conditions) {
      select_sql.conditions.emplace_back(std::move(cond));
    }
    // 处理where_expr：如果嵌套视图有where_expr，需要与外层WHERE合并
    if (node->selection.where_expr) {
      if (select_sql.where_expr) {
        // 外层也有where_expr，创建AND节点连接
        vector<unique_ptr<Expression>> children;
        children.push_back(std::move(select_sql.where_expr));
        children.push_back(std::move(node->selection.where_expr));
        select_sql.where_expr = make_unique<ConjunctionExpr>(ConjunctionExpr::Type::AND, std::move(children));
      } else {
        // 外层没有where_expr，直接移动
        select_sql.where_expr = std::move(node->selection.where_expr);
      }
    }

    // 处理表达式映射和重写
    bool only_star = (select_sql.expressions.size() == 1) &&
                     (select_sql.expressions[0] != nullptr) &&
                     (select_sql.expressions[0]->type() == ExprType::STAR);
    if (only_star) {
      // SELECT * 的情况：直接用嵌套视图的表达式替换
      select_sql.expressions.swap(node->selection.expressions);
      if (!node->selection.group_by.empty() && select_sql.group_by.empty()) {
        select_sql.group_by.swap(node->selection.group_by);
      }
      if (!node->selection.order_by.empty() && select_sql.order_by.empty()) {
        select_sql.order_by.swap(node->selection.order_by);
      }
    } else {
      // 非 * 的情况：根据嵌套视图输出列名映射，重写外层未限定字段
      unordered_map<string, pair<string, string>> name_to_relattr;
      unordered_map<string, unique_ptr<Expression>> name_to_expr;
      const vector<string> &nested_view_fields = nested_view->view_fields();
      build_view_output_mapping(db, select_sql.relations, node->selection.expressions,
                                nested_view_fields, name_to_relattr, name_to_expr);
      for (auto &outer_expr : select_sql.expressions) {
        RC rc = rewrite_unqualified_fields(outer_expr, name_to_relattr, name_to_expr);
        if (OB_FAIL(rc)) {
          LOG_WARN("Failed to rewrite field for nested view '%s'", nested_view->name());
          return rc;
        }
      }
      for (auto &gexpr : select_sql.group_by) {
        RC rc = rewrite_unqualified_fields(gexpr, name_to_relattr, name_to_expr);
        if (OB_FAIL(rc)) return rc;
      }
      for (auto &item : select_sql.order_by) {
        RC rc = rewrite_unqualified_fields(item.expression, name_to_relattr, name_to_expr);
        if (OB_FAIL(rc)) return rc;
      }
      // 重写WHERE条件中的字段
      if (select_sql.where_expr) {
        RC rc = rewrite_unqualified_fields(select_sql.where_expr, name_to_relattr, name_to_expr);
        if (OB_FAIL(rc)) {
          LOG_WARN("Failed to rewrite WHERE condition for nested view '%s'", nested_view->name());
          return rc;
        }
      }
    }

    LOG_INFO("Successfully expanded nested view: %s", nested_view->name());
    // 继续循环，检查新的relations是否还包含视图
  }

  // TODO: 多表FROM子句中的视图展开需要更复杂的列引用重写逻辑
  // 当前暂不支持，视图将作为派生表处理

  // 绑定阶段
  BinderContext binder_context;

  // 收集 FROM 表（包括物理表和视图）
  vector<Table *>                tables;
  vector<string>                 table_aliases;
  table_aliases.reserve(select_sql.relations.size());
  unordered_map<string, Table *> table_map;
  vector<SelectStmt::FromItem>   from_items;
  from_items.reserve(select_sql.relations.size());
  for (size_t i = 0; i < select_sql.relations.size(); i++) {
    const RelationSqlNode &r = select_sql.relations[i];
    const char *table_name = r.relation_name.c_str();
    if (nullptr == table_name) {
      LOG_WARN("invalid argument. relation name is null. index=%d", i);
      return RC::INVALID_ARGUMENT;
    }

    Table *table = db->find_table(table_name);
    if (nullptr == table) {
      // 尝试查找视图
      View *view = db->find_view(table_name);
      if (view != nullptr) {
        // 找到视图，将其作为派生表处理
        LOG_INFO("Found view '%s' in FROM clause, treating as derived table with alias '%s'",
                 table_name, r.alias.empty() ? table_name : r.alias.c_str());

        // 解析视图的SQL
        ParsedSqlResult parsed;
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

        // 递归创建视图的SelectStmt
        Stmt *view_stmt = nullptr;
        RC rc = SelectStmt::create(db, node->selection, view_stmt);
        if (OB_FAIL(rc)) {
          LOG_WARN("Failed to create SelectStmt for view '%s'", view->name());
          return rc;
        }

        SelectStmt *view_select_stmt = static_cast<SelectStmt *>(view_stmt);

        // 确定别名（如果没有指定别名，使用视图名）
        string alias_token = r.alias.empty() ? string(table_name) : r.alias;
        common::str_to_upper(alias_token);

        // 注册派生表到BinderContext
        binder_context.add_derived_table(alias_token, view_select_stmt);

        // 记录 FROM 子句顺序
        SelectStmt::FromItem item;
        item.type    = SelectStmt::FromItem::Type::DERIVED;
        item.alias   = alias_token;
        item.derived = view_select_stmt;
        from_items.emplace_back(std::move(item));

        // 注意：由于这是派生表，不添加到tables和table_map中
        // （tables只包含物理表，派生表通过binder_context.derived_tables()访问）

        continue;  // 处理下一个relation
      }

      // 既不是表也不是视图，报错
      LOG_WARN("no such table or view. db=%s, name=%s", db->name(), table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    // 找到物理表，正常处理
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
    SelectStmt::FromItem item;
    item.type  = SelectStmt::FromItem::Type::TABLE;
    item.table = table;
    item.alias = alias_token;
    from_items.emplace_back(std::move(item));
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
  // 设置绑定上下文为ORDER BY，聚合函数使用需要检查
  binder_context.set_binding_context(BinderContext::BindingContext::ORDER_BY);
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
  // 恢复为默认的SELECT上下文
  binder_context.set_binding_context(BinderContext::BindingContext::SELECT);

  // WHERE 过滤：支持两种来源
  // 1) 传统 AND 链（conditions）
  // 2) where_expr（支持 AND/OR 的布尔表达式）
  Table *default_table = nullptr;
  if (tables.size() == 1) {
    default_table = tables[0];
  }
  FilterStmt *filter_stmt = nullptr;
  RC rc = RC::SUCCESS;
  // 注意：where_expr的绑定统一放到后面与其他表达式一起处理，避免重复绑定
  if (!select_sql.where_expr) {
    // 仅当没有where_expr时，使用传统的FilterStmt（条件链）
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
  unique_ptr<SelectStmt> select_stmt_guard(new SelectStmt());
  SelectStmt *select_stmt = select_stmt_guard.get();
  select_stmt->tables_.swap(tables);
  select_stmt->table_aliases_.swap(table_aliases);
  select_stmt->from_items_.swap(from_items);
  select_stmt->query_expressions_.swap(bound_expressions);
  select_stmt->filter_stmt_ = filter_stmt;
  select_stmt->group_by_.swap(group_by_expressions);
  select_stmt->order_by_.swap(order_by_items);
  select_stmt->limit_ = select_sql.limit;
  if (select_sql.where_expr) {
    // 诊断：打印解析后的WHERE表达式结构
    if (select_sql.where_expr->type() == ExprType::CONJUNCTION) {
      auto *conj = static_cast<ConjunctionExpr *>(select_sql.where_expr.get());
      LOG_INFO("[SELECT_STMT] BEFORE bind: WHERE is ConjunctionExpr, type=%s, num_children=%zu",
                conj->conjunction_type() == ConjunctionExpr::Type::AND ? "AND" : "OR",
                conj->children().size());
      for (size_t i = 0; i < conj->children().size(); i++) {
        LOG_INFO("[SELECT_STMT]   child[%zu]: expr_type=%d", i, static_cast<int>(conj->children()[i]->type()));
      }
    } else {
      LOG_INFO("[SELECT_STMT] BEFORE bind: WHERE expr_type=%d", static_cast<int>(select_sql.where_expr->type()));
    }

    // 设置绑定上下文为WHERE，以便检查聚合函数使用
    binder_context.set_binding_context(BinderContext::BindingContext::WHERE);
    vector<unique_ptr<Expression>> bound;
    RC rc2 = expression_binder.bind_expression(select_sql.where_expr, bound);
    // 恢复为默认的SELECT上下文
    binder_context.set_binding_context(BinderContext::BindingContext::SELECT);
    if (OB_FAIL(rc2) || bound.size() != 1) {
      LOG_WARN("bind where boolean expression failed. rc=%s", strrc(rc2));
      return rc2 == RC::SUCCESS ? RC::INVALID_ARGUMENT : rc2;
    }

    // 诊断：打印绑定后的WHERE表达式结构
    if (bound[0]->type() == ExprType::CONJUNCTION) {
      auto *conj = static_cast<ConjunctionExpr *>(bound[0].get());
      LOG_INFO("[SELECT_STMT] AFTER bind: WHERE is ConjunctionExpr, type=%s, num_children=%zu",
                conj->conjunction_type() == ConjunctionExpr::Type::AND ? "AND" : "OR",
                conj->children().size());
      for (size_t i = 0; i < conj->children().size(); i++) {
        LOG_INFO("[SELECT_STMT]   child[%zu]: expr_type=%d", i, static_cast<int>(conj->children()[i]->type()));
      }
    } else {
      LOG_INFO("[SELECT_STMT] AFTER bind: WHERE expr_type=%d", static_cast<int>(bound[0]->type()));
    }

    select_stmt->where_expr_.reset(bound[0].release());
  }
  
  // 绑定 HAVING（将 AND 串联的条件转为一个布尔表达式树）
  if (!select_sql.having.empty()) {
    // 设置绑定上下文为HAVING，允许使用聚合函数
    binder_context.set_binding_context(BinderContext::BindingContext::HAVING);
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
      unique_ptr<Expression> having_expr(new ConjunctionExpr(ConjunctionExpr::Type::AND, std::move(cmp_exprs)));
      select_stmt->having_expr_.swap(having_expr);
    }
    // 恢复为默认的SELECT上下文
    binder_context.set_binding_context(BinderContext::BindingContext::SELECT);
  }

  if (!select_sql.set_operations.empty()) {
    const size_t expected_columns = select_stmt->query_expressions_.size();
    for (auto &set_node : select_sql.set_operations) {
      if (!set_node.select) {
        LOG_WARN("union branch is null");
        return RC::INVALID_ARGUMENT;
      }
      Stmt *child_stmt = nullptr;
      RC rc2 = SelectStmt::create(db, *set_node.select, child_stmt);
      if (OB_FAIL(rc2)) {
        LOG_WARN("failed to create select stmt for union branch. rc=%s", strrc(rc2));
        return rc2;
      }
      unique_ptr<SelectStmt> child_select(static_cast<SelectStmt *>(child_stmt));
      if (expected_columns != child_select->query_expressions().size()) {
        LOG_WARN("union branches column count mismatch. expected=%zu, got=%zu",
            expected_columns,
            child_select->query_expressions().size());
        return RC::SQL_SYNTAX;
      }
      SelectStmt::SetOperation op;
      op.type = set_node.type;
      op.stmt = std::move(child_select);
      select_stmt->set_operations_.emplace_back(std::move(op));
    }
  }

  // 保存派生表（视图）的SelectStmt到当前SelectStmt中，避免被释放
  for (const auto &entry : binder_context.derived_tables()) {
    select_stmt->add_derived_table_stmt(entry.first, entry.second);
  }

  stmt = select_stmt_guard.release();
  return RC::SUCCESS;
}
