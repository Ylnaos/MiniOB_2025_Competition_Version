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

#include "common/lang/unordered_set.h"
#include "common/log/log.h"
#include "sql/expr/expression.h"
#include "sql/operator/join_logical_operator.h"
#include "sql/operator/predicate_logical_operator.h"
#include "sql/operator/table_get_logical_operator.h"

using namespace std;

namespace {

string field_relation(Expression *expr)
{
  if (expr == nullptr || expr->type() != ExprType::FIELD) {
    return "";
  }
  auto *field_expr = static_cast<FieldExpr *>(expr);
  if (!field_expr->relation_name().empty()) {
    return field_expr->relation_name();
  }
  return field_expr->table_name();
}

void collect_relations(LogicalOperator *oper, unordered_set<string> &relations)
{
  if (oper == nullptr) {
    return;
  }
  if (oper->type() == LogicalOperatorType::TABLE_GET) {
    auto *table_get = static_cast<TableGetLogicalOperator *>(oper);
    if (!table_get->alias().empty()) {
      relations.insert(table_get->alias());
    } else if (table_get->table() != nullptr) {
      relations.insert(table_get->table()->name());
    }
    return;
  }

  for (auto &child : oper->children()) {
    collect_relations(child.get(), relations);
  }
}

bool contains_relation(LogicalOperator *oper, const string &relation)
{
  unordered_set<string> relations;
  collect_relations(oper, relations);
  return relations.find(relation) != relations.end();
}

bool is_hash_join_predicate(Expression *expr, string &left_relation, string &right_relation)
{
  if (expr == nullptr || expr->type() != ExprType::COMPARISON) {
    return false;
  }

  auto *comparison = static_cast<ComparisonExpr *>(expr);
  if (comparison->comp() != EQUAL_TO) {
    return false;
  }

  left_relation  = field_relation(comparison->left().get());
  right_relation = field_relation(comparison->right().get());
  return !left_relation.empty() && !right_relation.empty() && left_relation != right_relation;
}

bool attach_to_join(LogicalOperator *oper, unique_ptr<Expression> &expr)
{
  if (oper == nullptr || oper->type() != LogicalOperatorType::JOIN) {
    return false;
  }

  string left_relation;
  string right_relation;
  if (!is_hash_join_predicate(expr.get(), left_relation, right_relation)) {
    return false;
  }

  auto &children = oper->children();
  if (children.size() != 2) {
    return false;
  }

  const bool left_in_left   = contains_relation(children[0].get(), left_relation);
  const bool left_in_right  = contains_relation(children[1].get(), left_relation);
  const bool right_in_left  = contains_relation(children[0].get(), right_relation);
  const bool right_in_right = contains_relation(children[1].get(), right_relation);

  if ((left_in_left && right_in_right) || (left_in_right && right_in_left)) {
    auto *join = static_cast<JoinLogicalOperator *>(oper);
    join->add_join_predicate(std::move(expr));
    return true;
  }

  for (auto &child : children) {
    if (contains_relation(child.get(), left_relation) && contains_relation(child.get(), right_relation)) {
      return attach_to_join(child.get(), expr);
    }
  }

  return false;
}

void replace_with_child(unique_ptr<LogicalOperator> &oper)
{
  vector<unique_ptr<LogicalOperator>> &children = oper->children();
  if (children.size() != 1) {
    return;
  }
  unique_ptr<LogicalOperator> child = std::move(children.front());
  oper = std::move(child);
}

bool extract_join_predicates(LogicalOperator *join, unique_ptr<Expression> &expr, bool &change_made)
{
  if (expr == nullptr) {
    return true;
  }

  if (expr->type() == ExprType::CONJUNCTION) {
    auto *conjunction = static_cast<ConjunctionExpr *>(expr.get());
    if (conjunction->conjunction_type() != ConjunctionExpr::Type::AND) {
      return false;
    }

    auto &children = conjunction->children();
    for (auto iter = children.begin(); iter != children.end();) {
      if (extract_join_predicates(join, *iter, change_made)) {
        iter = children.erase(iter);
        change_made = true;
      } else {
        ++iter;
      }
    }

    if (children.empty()) {
      expr.reset();
      return true;
    }

    if (children.size() == 1) {
      expr = std::move(children.front());
      change_made = true;
    }
    return false;
  }

  if (attach_to_join(join, expr)) {
    change_made = true;
    return true;
  }
  return false;
}

}  // namespace

RC PredicateToJoinRewriter::rewrite(unique_ptr<LogicalOperator> &oper, bool &change_made)
{
  if (oper->type() != LogicalOperatorType::PREDICATE || oper->children().size() != 1 ||
      oper->children().front()->type() != LogicalOperatorType::JOIN || oper->expressions().size() != 1) {
    return RC::SUCCESS;
  }

  unique_ptr<Expression> &expr = oper->expressions().front();
  LogicalOperator        *join = oper->children().front().get();

  if (extract_join_predicates(join, expr, change_made)) {
    replace_with_child(oper);
  }

  return RC::SUCCESS;
}
