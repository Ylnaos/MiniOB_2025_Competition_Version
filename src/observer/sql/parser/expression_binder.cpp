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
// Created by Wangyunlai on 2024/05/29.
//

#include "common/log/log.h"
#include "common/lang/string.h"
#include "common/lang/ranges.h"
#include "sql/parser/expression_binder.h"
#include "sql/expr/expression_iterator.h"
#include "sql/stmt/select_stmt.h"

using namespace common;

Table *BinderContext::find_table(const char *table_name) const
{
  // 先按别名匹配（大小写不敏感）
  if (table_name != nullptr && *table_name != '\0') {
    string table_name_upper = table_name;
    common::str_to_upper(table_name_upper);
    auto it = alias_map_.find(table_name_upper);
    if (it != alias_map_.end()) {
      return it->second;
    }
  }
  auto pred = [table_name](Table *table) { return 0 == strcasecmp(table_name, table->name()); };
  auto iter = ranges::find_if(query_tables_, pred);
  if (iter == query_tables_.end()) {
    return nullptr;
  }
  return *iter;
}

////////////////////////////////////////////////////////////////////////////////
static void wildcard_fields(
    Table *table, const string &relation_name, vector<unique_ptr<Expression>> &expressions)
{
  const TableMeta &table_meta = table->table_meta();
  const int        field_num  = table_meta.field_num();
  for (int i = table_meta.sys_field_num(); i < field_num; i++) {
    Field      field(table, table_meta.field(i));
    FieldExpr *field_expr = new FieldExpr(field, relation_name);
    field_expr->set_name(field.field_name());
    LOG_DEBUG("wildcard bind field: table=%s relation_name=%s field=%s",
        table->name(), relation_name.c_str(), field.field_name());
    expressions.emplace_back(field_expr);
  }
}

RC ExpressionBinder::bind_expression(unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  // Pre-handle new or special expression types to avoid falling into legacy branches
  if (expr->type() == ExprType::SUBQUERY) {
    // 子查询作为叶子表达式，直接下放到执行阶段
    bound_expressions.emplace_back(std::move(expr));
    return RC::SUCCESS;
  }

  if (expr->type() == ExprType::EXISTS) {
    // EXISTS 表达式内部只包含一个子查询，保持原状交给执行期处理
    bound_expressions.emplace_back(std::move(expr));
    return RC::SUCCESS;
  }

  if (expr->type() == ExprType::MATCH_AGAINST) {
    // MATCH...AGAINST 全文检索表达式
    auto *match_expr = static_cast<MatchAgainstExpr *>(expr.get());

    // 绑定字段表达式
    for (auto &field : match_expr->fields()) {
      vector<unique_ptr<Expression>> field_bound;
      RC rc = bind_expression(field, field_bound);
      if (OB_FAIL(rc)) {
        LOG_WARN("failed to bind field in MATCH expression. rc=%s", strrc(rc));
        return rc;
      }
      if (field_bound.size() == 1 && field_bound[0].get() != field.get()) {
        field.reset(field_bound[0].release());
      }
    }

    // 绑定搜索文本表达式
    vector<unique_ptr<Expression>> search_bound;
    RC rc = bind_expression(match_expr->search_text(), search_bound);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to bind search text in MATCH expression. rc=%s", strrc(rc));
      return rc;
    }
    if (search_bound.size() == 1 && search_bound[0].get() != match_expr->search_text().get()) {
      match_expr->search_text().reset(search_bound[0].release());
    }

    // 验证字段类型并查找全文索引
    if (match_expr->fields().empty()) {
      LOG_WARN("MATCH expression requires at least one field");
      return RC::INVALID_ARGUMENT;
    }

    // 获取字段对应的表
    auto &first_field = match_expr->fields()[0];
    if (first_field->type() != ExprType::FIELD) {
      LOG_WARN("MATCH expression requires bound field expression");
      return RC::INVALID_ARGUMENT;
    }

    auto *field_expr = static_cast<FieldExpr *>(first_field.get());
    Table *table = const_cast<Table *>(field_expr->field().table());

    // 查找该字段上的全文索引
    const FieldMeta *field_meta = field_expr->field().meta();
    const TableMeta &table_meta = table->table_meta();
    Index *fulltext_index = nullptr;

    for (int i = 0; i < table_meta.index_num(); i++) {
      const IndexMeta *index_meta = table_meta.index(i);
      if (index_meta && index_meta->is_full_text_index() &&
          index_meta->fields().size() == 1 &&
          index_meta->fields()[0] == field_meta->name()) {
        fulltext_index = table->find_index(index_meta->name());
        break;
      }
    }

    if (fulltext_index == nullptr) {
      LOG_WARN("no full-text index found on field %s.%s", table->name(), field_meta->name());
      return RC::NOT_EXIST;
    }

    // 设置表和索引
    match_expr->set_table(table);
    match_expr->set_index(fulltext_index);

    bound_expressions.emplace_back(std::move(expr));
    return RC::SUCCESS;
  }

  if (expr->type() == ExprType::FUNCTION) {
    auto *func = static_cast<ScalarFunctionExpr *>(expr.get());
    auto bind_child = [&](unique_ptr<Expression> &child) -> RC {
      if (!child) {
        return RC::SUCCESS;
      }
      vector<unique_ptr<Expression>> child_bound;
      RC rc = bind_expression(child, child_bound);
      if (OB_FAIL(rc)) {
        return rc;
      }
      if (child_bound.size() == 1 && child_bound[0].get() != child.get()) {
        child.reset(child_bound[0].release());
      }
      return RC::SUCCESS;
    };

    RC rc = bind_child(func->child());
    if (OB_FAIL(rc)) {
      return rc;
    }
    rc = bind_child(func->child2());
    if (OB_FAIL(rc)) {
      return rc;
    }
    rc = bind_child(func->child3());
    if (OB_FAIL(rc)) {
      return rc;
    }

    AttrType arg_type = func->child()->value_type();
    switch (func->function_type()) {
      case ScalarFunctionExpr::FuncType::LENGTH:
        // 题目约束：length 仅支持 CHAR 类型
        if (arg_type != AttrType::CHARS) {
          LOG_WARN("length expects char type, got %d", (int)arg_type);
          return RC::INVALID_ARGUMENT;
        }
        break;
      case ScalarFunctionExpr::FuncType::ROUND:
        // 题目约束：round 仅支持 FLOAT 类型
        if (arg_type != AttrType::FLOATS) {
          LOG_WARN("round expects float type, got %d", (int)arg_type);
          return RC::INVALID_ARGUMENT;
        }
        // 若有第二个参数，要求为 INT 类型
        if (func->child2()) {
          AttrType s_type = func->child2()->value_type();
          if (s_type != AttrType::INTS) {
            LOG_WARN("round(scale) expects int type, got %d", (int)s_type);
            return RC::INVALID_ARGUMENT;
          }
        }
        break;
      case ScalarFunctionExpr::FuncType::DATE_FORMAT: {
        // 允许第一个参数为 DATE 或可解析为 DATE 的字符串
        if (arg_type == AttrType::CHARS) {
          // 隐式转换为 DATE，以复用后续执行逻辑
          auto cast = make_unique<CastExpr>(func->child()->copy(), AttrType::DATES);
          func->child().reset(cast.release());
        } else if (arg_type != AttrType::DATES) {
          LOG_WARN("date_format expects date or char type, got %d", (int)arg_type);
          return RC::INVALID_ARGUMENT;
        }
        // 如存在第二个参数，要求其为字符串（格式串）
        if (func->child2()) {
          AttrType fmt_type = func->child2()->value_type();
          if (fmt_type != AttrType::CHARS) {
            LOG_WARN("date_format format expects char type, got %d", (int)fmt_type);
            return RC::INVALID_ARGUMENT;
          }
        }
      } break;
      case ScalarFunctionExpr::FuncType::L2_DISTANCE:
      case ScalarFunctionExpr::FuncType::COSINE_DISTANCE:
      case ScalarFunctionExpr::FuncType::INNER_PRODUCT: {
        // 距离函数要求两个向量参数
        // 第一个参数允许为 VECTORS 或可转换为 VECTORS 的类型（如字符串）
        if (arg_type == AttrType::CHARS) {
          // 字符串隐式转换为向量
          auto cast = make_unique<CastExpr>(func->child()->copy(), AttrType::VECTORS);
          func->child().reset(cast.release());
        } else if (arg_type != AttrType::VECTORS) {
          LOG_WARN("distance function expects vector type for first arg, got %d", (int)arg_type);
          return RC::INVALID_ARGUMENT;
        }
        // 第二个参数也必须是向量或可转换为向量
        if (func->child2()) {
          AttrType arg2_type = func->child2()->value_type();
          if (arg2_type == AttrType::CHARS) {
            auto cast = make_unique<CastExpr>(func->child2()->copy(), AttrType::VECTORS);
            func->child2().reset(cast.release());
          } else if (arg2_type != AttrType::VECTORS) {
            LOG_WARN("distance function expects vector type for second arg, got %d", (int)arg2_type);
            return RC::INVALID_ARGUMENT;
          }
        } else {
          LOG_WARN("distance function requires two arguments");
          return RC::INVALID_ARGUMENT;
        }
      } break;
      case ScalarFunctionExpr::FuncType::STRING_TO_VECTOR: {
        // 字符串到向量的转换函数，兼容已经解析成向量的字面量
        if (arg_type == AttrType::CHARS || arg_type == AttrType::TEXTS) {
          // 字符串输入保持现状，由执行阶段完成解析
        } else if (arg_type == AttrType::VECTORS) {
          // 字面量或列已经是向量，直接放行
        } else {
          LOG_WARN("string_to_vector expects char/text/vector type, got %d", (int)arg_type);
          return RC::INVALID_ARGUMENT;
        }
      } break;
      case ScalarFunctionExpr::FuncType::VECTOR_TO_STRING: {
        // 向量到字符串的转换函数
        if (arg_type == AttrType::CHARS) {
          // 字符串输入：显式增加一次向量转换，便于运行期重用解析逻辑
          auto cast = make_unique<CastExpr>(func->child()->copy(), AttrType::VECTORS);
          func->child().reset(cast.release());
        } else if (arg_type == AttrType::NULLS) {
          // NULL 直接透传
        } else if (arg_type != AttrType::VECTORS) {
          LOG_WARN("vector_to_string expects vector type, got %d", (int)arg_type);
          return RC::INVALID_ARGUMENT;
        }
      } break;
      case ScalarFunctionExpr::FuncType::TOKENIZE: {
        // TOKENIZE 函数要求输入为字符串或文本，允许 NULL 直接透传
        if (arg_type != AttrType::CHARS && arg_type != AttrType::TEXTS && arg_type != AttrType::NULLS) {
          LOG_WARN("tokenize expects char/text type, got %d", (int)arg_type);
          return RC::INVALID_ARGUMENT;
        }
        if (func->child2()) {
          AttrType parser_type = func->child2()->value_type();
          if (parser_type != AttrType::CHARS && parser_type != AttrType::TEXTS && parser_type != AttrType::NULLS) {
            LOG_WARN("tokenize parser expects char/text type, got %d", (int)parser_type);
            return RC::INVALID_ARGUMENT;
          }
        }
      } break;
      case ScalarFunctionExpr::FuncType::DISTANCE: {
        // 通用距离函数，需要三个参数：vec1, vec2, distance_type
        // 这里只处理前两个参数的类型检查
        if (arg_type == AttrType::CHARS) {
          auto cast = make_unique<CastExpr>(func->child()->copy(), AttrType::VECTORS);
          func->child().reset(cast.release());
        } else if (arg_type != AttrType::VECTORS) {
          LOG_WARN("distance function expects vector type for first arg, got %d", (int)arg_type);
          return RC::INVALID_ARGUMENT;
        }
        if (func->child2()) {
          AttrType arg2_type = func->child2()->value_type();
          if (arg2_type == AttrType::CHARS) {
            auto cast = make_unique<CastExpr>(func->child2()->copy(), AttrType::VECTORS);
            func->child2().reset(cast.release());
          } else if (arg2_type != AttrType::VECTORS) {
            LOG_WARN("distance function expects vector type for second arg, got %d", (int)arg2_type);
            return RC::INVALID_ARGUMENT;
          }
        }
        // 第三个参数应该是距离类型字符串，不需要类型检查
      } break;
    }
    // 落入常规绑定，便于后续统一处理列位置等
    return bind_field_expression(expr, bound_expressions);
  }

  switch (expr->type()) {
    case ExprType::STAR: {
      return bind_star_expression(expr, bound_expressions);
    } break;

    case ExprType::UNBOUND_FIELD: {
      return bind_unbound_field_expression(expr, bound_expressions);
    } break;

    case ExprType::UNBOUND_AGGREGATION: {
      return bind_aggregate_expression(expr, bound_expressions);
    } break;

    case ExprType::FIELD: {
      return bind_field_expression(expr, bound_expressions);
    } break;

    case ExprType::VALUE: {
      return bind_value_expression(expr, bound_expressions);
    } break;

    case ExprType::CAST: {
      return bind_cast_expression(expr, bound_expressions);
    } break;

    case ExprType::COMPARISON: {
      return bind_comparison_expression(expr, bound_expressions);
    } break;

    case ExprType::CONJUNCTION: {
      return bind_conjunction_expression(expr, bound_expressions);
    } break;

    case ExprType::ARITHMETIC: {
      return bind_arithmetic_expression(expr, bound_expressions);
    } break;

    case ExprType::AGGREGATION: {
      ASSERT(false, "shouldn't be here");
    } break;

    case ExprType::SUBQUERY: {
      // 子查询不依赖于外层表（本题约束为非关联子查询），无需在当前上下文再次绑定
      return bind_field_expression(expr, bound_expressions);
    } break;

    case ExprType::IN_LIST: {
      // IN 表达式的两个子表达式分别进行绑定
      auto *in_expr = static_cast<InExpr *>(expr.get());
      vector<unique_ptr<Expression>> tmp;
      RC rc = bind_expression(in_expr->test_expr(), tmp);
      if (OB_FAIL(rc)) return rc;
      if (tmp.size() == 1 && tmp[0].get() != in_expr->test_expr().get()) {
        in_expr->test_expr().reset(tmp[0].release());
      }
      tmp.clear();
      rc = bind_expression(in_expr->set_expr(), tmp);
      if (OB_FAIL(rc)) return rc;
      if (tmp.size() == 1 && tmp[0].get() != in_expr->set_expr().get()) {
        in_expr->set_expr().reset(tmp[0].release());
      }
      return bind_field_expression(expr, bound_expressions);
    } break;

    default: {
      LOG_WARN("unknown expression type: %d", static_cast<int>(expr->type()));
      return RC::INTERNAL;
    }
  }
  return RC::INTERNAL;
}

RC ExpressionBinder::bind_star_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto star_expr = static_cast<StarExpr *>(expr.get());

  // 禁止对通配符 * 起别名：例如 SELECT * AS alias / SELECT * alias
  if (expr->alias() != nullptr && expr->alias()[0] != '\0') {
    LOG_WARN("wildcard '*' cannot have alias");
    return RC::INVALID_ARGUMENT;
  }

  vector<Table *> tables_to_wildcard;
  vector<pair<string, SelectStmt *>> derived_tables_to_wildcard;

  const char *table_name = star_expr->table_name();
  if (!is_blank(table_name) && 0 != strcmp(table_name, "*")) {
    Table *table = context_.find_table(table_name);
    if (nullptr == table) {
      // 尝试查找派生表
      string alias_upper = table_name;
      common::str_to_upper(alias_upper);
      SelectStmt *derived_stmt = context_.find_derived_table(alias_upper);
      if (derived_stmt != nullptr) {
        derived_tables_to_wildcard.push_back({alias_upper, derived_stmt});
      } else {
        LOG_INFO("no such table in from list: %s", table_name);
        return RC::SCHEMA_TABLE_NOT_EXIST;
      }
    } else {
      tables_to_wildcard.push_back(table);
    }
  } else {
    // SELECT * - 展开所有表和派生表
    const vector<Table *> &all_tables = context_.query_tables();
    tables_to_wildcard.insert(tables_to_wildcard.end(), all_tables.begin(), all_tables.end());

    // 也包含派生表
    for (const auto &entry : context_.derived_tables()) {
      derived_tables_to_wildcard.push_back({entry.first, entry.second});
    }
  }

  // 展开物理表的字段
  for (Table *table : tables_to_wildcard) {
    std::string qualifier;
    if (!is_blank(table_name) && 0 != strcmp(table_name, "*")) {
      qualifier = table_name;
    } else {
      qualifier = table->name();
    }
    common::str_to_upper(qualifier);
    wildcard_fields(table, qualifier, bound_expressions);
  }

  // 展开派生表的字段
  for (const auto &entry : derived_tables_to_wildcard) {
    const string &alias = entry.first;
    SelectStmt *derived_stmt = entry.second;

    const auto &output_exprs = derived_stmt->query_expressions();
    for (size_t idx = 0; idx < output_exprs.size(); idx++) {
      const auto &output_expr = output_exprs[idx];
      if (!output_expr) {
        continue;
      }

      const char *col_name = output_expr->alias();
      if (is_blank(col_name)) {
        col_name = output_expr->name();
      }
      string candidate_name;
      if (!is_blank(col_name)) {
        candidate_name = col_name;
      } else {
        candidate_name = string("COLUMN_") + std::to_string(idx + 1);
      }
      common::str_to_upper(candidate_name);

      auto *field_expr = new FieldExpr();
      field_expr->set_pos(static_cast<int>(idx));
      field_expr->set_name(alias + "." + candidate_name);
      LOG_DEBUG("Wildcard bind field from derived table: %s.%s at position %zu",
                alias.c_str(), candidate_name.c_str(), idx);
      bound_expressions.emplace_back(field_expr);
    }
  }

  return RC::SUCCESS;
}

RC ExpressionBinder::bind_unbound_field_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto unbound_field_expr = static_cast<UnboundFieldExpr *>(expr.get());

  const char *table_name = unbound_field_expr->table_name();
  const char *field_name = unbound_field_expr->field_name();

  SelectStmt *inner_view_stmt = context_.inner_view_stmt();
  if (context_.query_tables().empty() && inner_view_stmt != nullptr) {
    if (!is_blank(table_name)) {
      LOG_INFO("inner view binding does not support qualified column: %s.%s", table_name, field_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
    if (is_blank(field_name)) {
      LOG_INFO("inner view binding missing column name");
      return RC::SCHEMA_FIELD_MISSING;
    }

    string target_name = field_name;
    common::str_to_upper(target_name);

    const auto &inner_exprs = inner_view_stmt->query_expressions();
    for (size_t idx = 0; idx < inner_exprs.size(); idx++) {
      const auto &inner_expr = inner_exprs[idx];
      if (!inner_expr) {
        continue;
      }
      const char *candidate = inner_expr->alias();
      if (is_blank(candidate)) {
        candidate = inner_expr->name();
      }
      string candidate_name;
      if (!is_blank(candidate)) {
        candidate_name = candidate;
      } else {
        candidate_name = string("COLUMN_") + std::to_string(idx + 1);
      }
      common::str_to_upper(candidate_name);
      if (candidate_name == target_name) {
        auto *field_expr = new FieldExpr();
        field_expr->set_pos(static_cast<int>(idx));
        field_expr->set_name(candidate_name);
        if (expr->alias() != nullptr) {
          field_expr->set_alias(expr->alias());
        }
        LOG_DEBUG("Bind field '%s' from inner view at position %zu", candidate_name.c_str(), idx);
        bound_expressions.emplace_back(field_expr);
        return RC::SUCCESS;
      }
    }

    LOG_INFO("no such field in inner view output: %s", field_name);
    return RC::SCHEMA_FIELD_MISSING;
  }

  Table *table = nullptr;
  if (is_blank(table_name)) {
    size_t total_tables = context_.query_tables().size() + context_.derived_tables().size();
    if (total_tables != 1) {
      LOG_INFO("cannot determine table for field: %s (total tables: %zu)", field_name, total_tables);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }

    if (!context_.query_tables().empty()) {
      table = context_.query_tables()[0];
    } else {
      // 只有一个派生表，从派生表中查找字段
      auto &derived_tables = context_.derived_tables();
      auto it = derived_tables.begin();
      SelectStmt *derived_stmt = it->second;
      string derived_alias = it->first;

      if (0 == strcmp(field_name, "*")) {
        LOG_WARN("SELECT * from single derived table without qualification is not supported in this context");
        return RC::INVALID_ARGUMENT;
      }

      // 查找字段
      string target_name = field_name;
      common::str_to_upper(target_name);

      const auto &output_exprs = derived_stmt->query_expressions();
      for (size_t idx = 0; idx < output_exprs.size(); idx++) {
        const auto &output_expr = output_exprs[idx];
        if (!output_expr) {
          continue;
        }

        const char *col_name = output_expr->alias();
        if (is_blank(col_name)) {
          col_name = output_expr->name();
        }
        string candidate_name;
        if (!is_blank(col_name)) {
          candidate_name = col_name;
        } else {
          candidate_name = string("COLUMN_") + std::to_string(idx + 1);
        }
        common::str_to_upper(candidate_name);

        // 提取字段名（去掉表名前缀）
        string field_name_only = candidate_name;
        size_t dot_pos = candidate_name.find('.');
        if (dot_pos != string::npos) {
          field_name_only = candidate_name.substr(dot_pos + 1);
        }

        if (candidate_name == target_name || field_name_only == target_name) {
          auto *field_expr = new FieldExpr();
          field_expr->set_pos(static_cast<int>(idx));
          field_expr->set_name(derived_alias + "." + target_name);  // 使用完整名称（表名.字段名）
          field_expr->set_relation_name(derived_alias);  // 设置relation_name以支持find_cell查找
          // 从派生表的输出表达式中提取类型信息
          field_expr->set_derived_type_info(output_expr->value_type(), output_expr->value_length());
          if (expr->alias() != nullptr) {
            field_expr->set_alias(expr->alias());
          }
          LOG_DEBUG("Bind unqualified field '%s' from single derived table '%s' at position %zu, type=%d, length=%d, relation=%s",
                    target_name.c_str(), derived_alias.c_str(), idx,
                    static_cast<int>(output_expr->value_type()), output_expr->value_length(), derived_alias.c_str());
          bound_expressions.emplace_back(field_expr);
          return RC::SUCCESS;
        }
      }

      LOG_INFO("no such field in single derived table: %s", field_name);
      return RC::SCHEMA_FIELD_MISSING;
    }
  } else {
    table = context_.find_table(table_name);
    if (nullptr == table) {
      // 尝试查找派生表（视图作为表使用）
      string alias_upper = table_name;
      common::str_to_upper(alias_upper);
      SelectStmt *derived_stmt = context_.find_derived_table(alias_upper);
      if (derived_stmt != nullptr) {
        // 找到派生表，从其输出列中查找字段
        LOG_DEBUG("Found derived table '%s', looking for field '%s'", alias_upper.c_str(), field_name);

        if (0 == strcmp(field_name, "*")) {
          // 处理 t.* 的情况
          if (expr->alias() != nullptr && expr->alias()[0] != '\0') {
            LOG_WARN("wildcard 't.*' cannot have alias");
            return RC::INVALID_ARGUMENT;
          }

          // 从派生表的输出列生成 FieldExpr
          const auto &output_exprs = derived_stmt->query_expressions();
          for (size_t idx = 0; idx < output_exprs.size(); idx++) {
            const auto &output_expr = output_exprs[idx];
            if (!output_expr) {
              continue;
            }

            const char *col_name = output_expr->alias();
            if (is_blank(col_name)) {
              col_name = output_expr->name();
            }
            string candidate_name;
            if (!is_blank(col_name)) {
              candidate_name = col_name;
            } else {
              candidate_name = string("COLUMN_") + std::to_string(idx + 1);
            }
            common::str_to_upper(candidate_name);

            // 提取字段名（去掉表名前缀）
            string field_name_only = candidate_name;
            size_t dot_pos = candidate_name.find('.');
            if (dot_pos != string::npos) {
              field_name_only = candidate_name.substr(dot_pos + 1);
            }

            auto *field_expr = new FieldExpr();
            field_expr->set_pos(static_cast<int>(idx));
            field_expr->set_name(alias_upper + "." + field_name_only);
            field_expr->set_relation_name(alias_upper);  // 设置relation_name以支持find_cell查找
            // 从派生表的输出表达式中提取类型信息
            field_expr->set_derived_type_info(output_expr->value_type(), output_expr->value_length());
            LOG_DEBUG("Bind field from derived table: %s.%s at position %zu, type=%d, length=%d, relation=%s",
                      alias_upper.c_str(), field_name_only.c_str(), idx,
                      static_cast<int>(output_expr->value_type()), output_expr->value_length(), alias_upper.c_str());
            bound_expressions.emplace_back(field_expr);
          }
          return RC::SUCCESS;
        } else {
          // 查找指定字段
          string target_name = field_name;
          common::str_to_upper(target_name);

          const auto &output_exprs = derived_stmt->query_expressions();
          for (size_t idx = 0; idx < output_exprs.size(); idx++) {
            const auto &output_expr = output_exprs[idx];
            if (!output_expr) {
              continue;
            }

            const char *col_name = output_expr->alias();
            if (is_blank(col_name)) {
              col_name = output_expr->name();
            }
            string candidate_name;
            if (!is_blank(col_name)) {
              candidate_name = col_name;
            } else {
              candidate_name = string("COLUMN_") + std::to_string(idx + 1);
            }
            common::str_to_upper(candidate_name);

            // 提取字段名（去掉表名前缀）
            string field_name_only = candidate_name;
            size_t dot_pos = candidate_name.find('.');
            if (dot_pos != string::npos) {
              field_name_only = candidate_name.substr(dot_pos + 1);
            }

            if (candidate_name == target_name || field_name_only == target_name) {
              auto *field_expr = new FieldExpr();
              field_expr->set_pos(static_cast<int>(idx));
              field_expr->set_name(alias_upper + "." + target_name);
              field_expr->set_relation_name(alias_upper);  // 设置relation_name以支持find_cell查找
              // 从派生表的输出表达式中提取类型信息
              field_expr->set_derived_type_info(output_expr->value_type(), output_expr->value_length());
              if (expr->alias() != nullptr) {
                field_expr->set_alias(expr->alias());
              }
              LOG_DEBUG("Bind field '%s' from derived table '%s' at position %zu, type=%d, length=%d, relation=%s",
                        target_name.c_str(), alias_upper.c_str(), idx,
                        static_cast<int>(output_expr->value_type()), output_expr->value_length(), alias_upper.c_str());
              bound_expressions.emplace_back(field_expr);
              return RC::SUCCESS;
            }
          }

          LOG_INFO("no such field in derived table: %s.%s", alias_upper.c_str(), field_name);
          return RC::SCHEMA_FIELD_MISSING;
        }
      }

      LOG_INFO("no such table in from list: %s", table_name);
      return RC::SCHEMA_TABLE_NOT_EXIST;
    }
  }

  if (0 == strcmp(field_name, "*")) {
    // 禁止对 t.* 起别名
    if (expr->alias() != nullptr && expr->alias()[0] != '\0') {
      LOG_WARN("wildcard 't.*' cannot have alias");
      return RC::INVALID_ARGUMENT;
    }
    std::string qualifier;
    if (!is_blank(table_name)) {
      qualifier = table_name;
    } else {
      qualifier = table->name();
    }
    common::str_to_upper(qualifier);
    LOG_DEBUG("bind unbound wildcard field: table=%s qualifier=%s", table->name(), qualifier.c_str());
    wildcard_fields(table, qualifier, bound_expressions);
  } else {
    const FieldMeta *field_meta = table->table_meta().field(field_name);
    if (nullptr == field_meta) {
      // 打印表中所有可用字段用于诊断
      LOG_WARN("[FIELD_BIND] Looking for field '%s' in table '%s' (alias: %s)",
                field_name, table->name(), table_name);
      LOG_WARN("[FIELD_BIND] Available fields in table '%s':", table->name());
      for (int i = 0; i < table->table_meta().field_num(); i++) {
        const FieldMeta *fm = table->table_meta().field(i);
        if (fm) {
          LOG_WARN("[FIELD_BIND]   - %s (type=%d)", fm->name(), static_cast<int>(fm->type()));
        }
      }
      LOG_INFO("no such field in table: %s.%s", table_name, field_name);
      return RC::SCHEMA_FIELD_MISSING;
    }

    Field      field(table, field_meta);
    std::string qualifier;
    if (!is_blank(table_name)) {
      qualifier = table_name;
    } else {
      qualifier = table->name();
    }
    common::str_to_upper(qualifier);
    FieldExpr *field_expr = new FieldExpr(field, qualifier);
    // 如果用户在 SQL 中使用了带表名限定的列名（如 t.col），
    // 则输出表头时也应保留“表.列”的形式，并与测试期望一致（大写）。
    if (!is_blank(table_name)) {
      std::string t = table_name; // 保留别名/原名
      std::string f = field_name;
      common::str_to_upper(t);
      common::str_to_upper(f);
      field_expr->set_name(t + "." + f);
    } else {
      // 未带表名限定时，保持列名本身，避免影响单表查询的表头
      field_expr->set_name(field_name);
    }
    LOG_DEBUG("bind unbound field: table=%s qualifier=%s field=%s alias=%s",
        table->name(), qualifier.c_str(), field_name, field_expr->name());
    bound_expressions.emplace_back(field_expr);
  }

  return RC::SUCCESS;
}

RC ExpressionBinder::bind_field_expression(
    unique_ptr<Expression> &field_expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  bound_expressions.emplace_back(std::move(field_expr));
  return RC::SUCCESS;
}

RC ExpressionBinder::bind_value_expression(
    unique_ptr<Expression> &value_expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  bound_expressions.emplace_back(std::move(value_expr));
  return RC::SUCCESS;
}

RC ExpressionBinder::bind_cast_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto cast_expr = static_cast<CastExpr *>(expr.get());

  vector<unique_ptr<Expression>> child_bound_expressions;
  unique_ptr<Expression>        &child_expr = cast_expr->child();

  RC rc = bind_expression(child_expr, child_bound_expressions);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  if (child_bound_expressions.size() != 1) {
    LOG_WARN("invalid children number of cast expression: %d", child_bound_expressions.size());
    return RC::INVALID_ARGUMENT;
  }

  unique_ptr<Expression> &child = child_bound_expressions[0];
  if (child.get() == child_expr.get()) {
    return RC::SUCCESS;
  }

  child_expr.reset(child.release());
  bound_expressions.emplace_back(std::move(expr));
  return RC::SUCCESS;
}

RC ExpressionBinder::bind_comparison_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto comparison_expr = static_cast<ComparisonExpr *>(expr.get());

  vector<unique_ptr<Expression>> child_bound_expressions;
  unique_ptr<Expression>        &left_expr  = comparison_expr->left();
  unique_ptr<Expression>        &right_expr = comparison_expr->right();

  RC rc = bind_expression(left_expr, child_bound_expressions);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  if (child_bound_expressions.size() != 1) {
    LOG_WARN("invalid left children number of comparison expression: %d", child_bound_expressions.size());
    return RC::INVALID_ARGUMENT;
  }

  unique_ptr<Expression> &left = child_bound_expressions[0];
  if (left.get() != left_expr.get()) {
    left_expr.reset(left.release());
  }

  child_bound_expressions.clear();
  rc = bind_expression(right_expr, child_bound_expressions);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  if (child_bound_expressions.size() != 1) {
    LOG_WARN("invalid right children number of comparison expression: %d", child_bound_expressions.size());
    return RC::INVALID_ARGUMENT;
  }

  unique_ptr<Expression> &right = child_bound_expressions[0];
  if (right.get() != right_expr.get()) {
    right_expr.reset(right.release());
  }

  bound_expressions.emplace_back(std::move(expr));
  return RC::SUCCESS;
}

RC ExpressionBinder::bind_conjunction_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto conjunction_expr = static_cast<ConjunctionExpr *>(expr.get());

  vector<unique_ptr<Expression>>  child_bound_expressions;
  vector<unique_ptr<Expression>> &children = conjunction_expr->children();

  LOG_INFO("[BIND_CONJUNCTION] BEFORE binding: num_children=%zu, type=%s",
            children.size(),
            conjunction_expr->conjunction_type() == ConjunctionExpr::Type::AND ? "AND" : "OR");

  int child_idx = 0;
  for (unique_ptr<Expression> &child_expr : children) {
    LOG_INFO("[BIND_CONJUNCTION] binding child[%d], expr_type=%d", child_idx, static_cast<int>(child_expr->type()));
    child_bound_expressions.clear();

    RC rc = bind_expression(child_expr, child_bound_expressions);
    if (rc != RC::SUCCESS) {
      LOG_WARN("[BIND_CONJUNCTION] bind child[%d] FAILED: rc=%s", child_idx, strrc(rc));
      return rc;
    }

    if (child_bound_expressions.size() != 1) {
      LOG_WARN("[BIND_CONJUNCTION] invalid children number of conjunction expression: %d", child_bound_expressions.size());
      return RC::INVALID_ARGUMENT;
    }

    unique_ptr<Expression> &child = child_bound_expressions[0];
    if (child.get() != child_expr.get()) {
      child_expr.reset(child.release());
    }
    LOG_INFO("[BIND_CONJUNCTION] child[%d] bound successfully", child_idx);
    child_idx++;
  }

  LOG_INFO("[BIND_CONJUNCTION] AFTER binding: num_children=%zu", children.size());
  bound_expressions.emplace_back(std::move(expr));

  return RC::SUCCESS;
}

RC ExpressionBinder::bind_arithmetic_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  auto arithmetic_expr = static_cast<ArithmeticExpr *>(expr.get());

  vector<unique_ptr<Expression>> child_bound_expressions;
  unique_ptr<Expression>        &left_expr  = arithmetic_expr->left();
  unique_ptr<Expression>        &right_expr = arithmetic_expr->right();

  RC rc = bind_expression(left_expr, child_bound_expressions);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (child_bound_expressions.size() != 1) {
    LOG_WARN("invalid left children number of comparison expression: %d", child_bound_expressions.size());
    return RC::INVALID_ARGUMENT;
  }

  unique_ptr<Expression> &left = child_bound_expressions[0];
  if (left.get() != left_expr.get()) {
    left_expr.reset(left.release());
  }

  // 处理右操作数（可能为空，如一元负号运算）
  if (right_expr) {
    child_bound_expressions.clear();
    rc = bind_expression(right_expr, child_bound_expressions);
    if (OB_FAIL(rc)) {
      return rc;
    }

    if (child_bound_expressions.size() != 1) {
      LOG_WARN("invalid right children number of comparison expression: %d", child_bound_expressions.size());
      return RC::INVALID_ARGUMENT;
    }

    unique_ptr<Expression> &right = child_bound_expressions[0];
    if (right.get() != right_expr.get()) {
      right_expr.reset(right.release());
    }
  }

  bound_expressions.emplace_back(std::move(expr));
  return RC::SUCCESS;
}

RC check_aggregate_expression(AggregateExpr &expression)
{
  // 必须有一个子表达式
  Expression *child_expression = expression.child().get();
  if (nullptr == child_expression) {
    LOG_WARN("child expression of aggregate expression is null");
    return RC::INVALID_ARGUMENT;
  }

  // 校验数据类型与聚合类型是否匹配
  AggregateExpr::Type aggregate_type   = expression.aggregate_type();
  AttrType            child_value_type = child_expression->value_type();
  switch (aggregate_type) {
    case AggregateExpr::Type::SUM:
    case AggregateExpr::Type::AVG: {
      // 仅支持数值类型
      if (!is_numerical_type(child_value_type)) {
        LOG_WARN("invalid child value type for aggregate expression: %d", static_cast<int>(child_value_type));
        return RC::INVALID_ARGUMENT;
      }
    } break;

    case AggregateExpr::Type::COUNT:
    case AggregateExpr::Type::MAX:
    case AggregateExpr::Type::MIN: {
      // 任何类型都支持
    } break;
  }

  // 子表达式中不能再包含聚合表达式
  function<RC(unique_ptr<Expression>&)> check_aggregate_expr = [&](unique_ptr<Expression> &expr) -> RC {
    RC rc = RC::SUCCESS;
    if (expr->type() == ExprType::AGGREGATION) {
      LOG_WARN("aggregate expression cannot be nested");
      return RC::INVALID_ARGUMENT;
    }
    rc = ExpressionIterator::iterate_child_expr(*expr, check_aggregate_expr);
    return rc;
  };

  RC rc = ExpressionIterator::iterate_child_expr(expression, check_aggregate_expr);

  return rc;
}

RC ExpressionBinder::bind_aggregate_expression(
    unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &bound_expressions)
{
  if (nullptr == expr) {
    return RC::SUCCESS;
  }

  // 检查聚合函数使用的上下文：WHERE子句中不能使用聚合函数
  if (context_.binding_context() == BinderContext::BindingContext::WHERE) {
    LOG_WARN("aggregate functions are not allowed in WHERE clause");
    return RC::INVALID_ARGUMENT;
  }

  // ORDER BY子句中对聚合函数使用进行限制（标准SQL要求ORDER BY中的聚合函数必须在SELECT子句中定义）
  if (context_.binding_context() == BinderContext::BindingContext::ORDER_BY) {
    LOG_WARN("aggregate functions in ORDER BY must be defined in SELECT clause");
    return RC::INVALID_ARGUMENT;
  }

  auto unbound_aggregate_expr = static_cast<UnboundAggregateExpr *>(expr.get());
  // 多参数聚合（如 count(*, num)）在语义阶段报错为 INVALID_ARGUMENT，而非语法错误
  if (unbound_aggregate_expr->arg_count() != 1) {
    LOG_WARN("invalid argument count for aggregate expression: %d", unbound_aggregate_expr->arg_count());
    return RC::INVALID_ARGUMENT;
  }
  const char *aggregate_name = unbound_aggregate_expr->aggregate_name();
  AggregateExpr::Type aggregate_type;
  RC rc = AggregateExpr::type_from_string(aggregate_name, aggregate_type);
  if (OB_FAIL(rc)) {
    LOG_WARN("invalid aggregate name: %s", aggregate_name);
    return rc;
  }

  unique_ptr<Expression>        &child_expr = unbound_aggregate_expr->child();
  vector<unique_ptr<Expression>> child_bound_expressions;

  if (child_expr->type() == ExprType::STAR && aggregate_type == AggregateExpr::Type::COUNT) {
    ValueExpr *value_expr = new ValueExpr(Value(1));
    child_expr.reset(value_expr);
  } else {
    rc = bind_expression(child_expr, child_bound_expressions);
    if (OB_FAIL(rc)) {
      return rc;
    }

    if (child_bound_expressions.size() != 1) {
      LOG_WARN("invalid children number of aggregate expression: %d", child_bound_expressions.size());
      return RC::INVALID_ARGUMENT;
    }

    if (child_bound_expressions[0].get() != child_expr.get()) {
      child_expr.reset(child_bound_expressions[0].release());
    }
  }

  auto aggregate_expr = make_unique<AggregateExpr>(aggregate_type, std::move(child_expr));
  aggregate_expr->set_name(unbound_aggregate_expr->name());
  rc = check_aggregate_expression(*aggregate_expr);
  if (OB_FAIL(rc)) {
    return rc;
  }

  bound_expressions.emplace_back(std::move(aggregate_expr));
  return RC::SUCCESS;
}
