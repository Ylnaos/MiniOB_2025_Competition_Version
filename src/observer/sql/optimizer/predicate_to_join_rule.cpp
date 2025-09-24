/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/optimizer/predicate_to_join_rule.h"
#include "common/log/log.h"
#include "sql/operator/predicate_logical_operator.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"
#include "sql/expr/expression.h"
#include "sql/expr/expression_iterator.h"
#include <unordered_set>
#include <functional>

using namespace std;

// 判断一个表达式是否为“跨表”比较（典型JOIN条件）：
// 规则：比较表达式，且其中引用到至少两个不同的表
// 收集某逻辑子树中涉及的所有表名（基于 TableGetLogicalOperator）
static void collect_tables_in_subtree(LogicalOperator &op, std::unordered_set<std::string> &out)
{
  if (op.type() == LogicalOperatorType::TABLE_GET) {
    auto *get = static_cast<TableGetLogicalOperator *>(&op);
    if (get->table()) {
      out.emplace(get->table()->name());
    }
  }
  for (auto &ch : op.children()) {
    if (ch) collect_tables_in_subtree(*ch, out);
  }
}

// 判断一个表达式是否属于当前JOIN：
// - 若同时引用左右两侧任意表 -> 属于JOIN条件
// - 或仅引用右侧表 -> 也视为JOIN相关（INNER JOIN下与ON等价）
static bool is_join_condition(Expression &expr,
                              const std::unordered_set<std::string> &left_tables,
                              const std::unordered_set<std::string> &right_tables)
{
  if (expr.type() != ExprType::COMPARISON) {
    return false;
  }

  std::unordered_set<std::string> tables;
  std::function<RC(unique_ptr<Expression>&)> collect_tables = [&](unique_ptr<Expression> &child) -> RC {
    if (!child) return RC::SUCCESS;
    if (child->type() == ExprType::FIELD) {
      auto *f = static_cast<FieldExpr *>(child.get());
      if (f->field().table()) {
        tables.emplace(f->field().table()->name());
      }
    }
    return ExpressionIterator::iterate_child_expr(*child, collect_tables);
  };

  // 遍历当前比较表达式的左右子树
  (void)ExpressionIterator::iterate_child_expr(expr, collect_tables);

  // 是否命中左右两侧
  bool hit_left  = false;
  bool hit_right = false;
  for (const auto &t : tables) {
    if (!hit_left && left_tables.count(t))  hit_left  = true;
    if (!hit_right && right_tables.count(t)) hit_right = true;
  }

  if (hit_left && hit_right) return true;  // 典型连接条件
  if (!hit_left && hit_right) return true; // 右表独有过滤，内连接下放到JOIN亦可
  return false;
}

RC PredicateToJoinRule::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made)
{
  change_made = false;
  if (oper->type() != LogicalOperatorType::PREDICATE) {
    return RC::SUCCESS;
  }

  // 仅处理形如：Predicate( Join(...) ) 的结构
  if (oper->children().size() != 1) {
    return RC::SUCCESS;
  }

  auto *pred_oper = static_cast<PredicateLogicalOperator *>(oper.get());
  auto &exprs = pred_oper->expressions();
  if (exprs.size() != 1) {
    return RC::SUCCESS;
  }

  unique_ptr<LogicalOperator> &child = oper->children().front();
  if (child->type() != LogicalOperatorType::JOIN) {
    return RC::SUCCESS;
  }
  auto *join_oper = static_cast<JoinLogicalOperator *>(child.get());

  // 识别左右子树包含的表集合
  std::unordered_set<std::string> left_tables;
  std::unordered_set<std::string> right_tables;
  if (child->children().size() == 2) {
    collect_tables_in_subtree(*child->children()[0], left_tables);
    collect_tables_in_subtree(*child->children()[1], right_tables);
  }

  Expression *root = exprs.front().get();
  if (root->type() != ExprType::CONJUNCTION) {
    // 单一表达式：若为跨表比较，直接下推到JOIN
    if (is_join_condition(*root, left_tables, right_tables)) {
      join_oper->add_join_predicate(std::move(exprs.front()));
      // 用恒真替换该谓词，后续Rewrite会将空谓词折叠掉
      exprs.clear();
      exprs.emplace_back(make_unique<ValueExpr>(Value(true)));
      change_made = true;
    }
    return RC::SUCCESS;
  }

  // 拆分合取表达式，将跨表条件下推到JOIN，其余保留在PREDICATE上
  auto *conj = static_cast<ConjunctionExpr *>(root);
  if (conj->conjunction_type() != ConjunctionExpr::Type::AND) {
    return RC::SUCCESS; // 仅处理 AND 连接的情形
  }

  vector<unique_ptr<Expression>> remain;
  vector<unique_ptr<Expression>> moved;
  for (auto &child_expr : conj->children()) {
    if (child_expr && is_join_condition(*child_expr, left_tables, right_tables)) {
      moved.emplace_back(std::move(child_expr));
    } else {
      remain.emplace_back(std::move(child_expr));
    }
  }

  if (!moved.empty()) {
    // 将跨表条件挂到JOIN上
    for (auto &e : moved) {
      join_oper->add_join_predicate(std::move(e));
    }

    // 重新组装剩余谓词
    if (remain.empty()) {
      exprs.clear();
      exprs.emplace_back(make_unique<ValueExpr>(Value(true)));
    } else if (remain.size() == 1) {
      exprs.clear();
      exprs.emplace_back(std::move(remain.front()));
    } else {
      exprs.clear();
      // ConjunctionExpr 构造函数接收左值引用，这里直接以lvalue传入以便内部接管
      exprs.emplace_back(std::unique_ptr<Expression>(new ConjunctionExpr(ConjunctionExpr::Type::AND, remain)));
    }
    change_made = true;
  }

  return RC::SUCCESS;
}
