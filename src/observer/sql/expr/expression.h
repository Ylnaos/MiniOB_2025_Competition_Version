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
// Created by Wangyunlai on 2022/07/05.
//

#pragma once

#include "common/lang/string.h"
#include "common/lang/memory.h"
#include "common/lang/unordered_set.h"
#include "common/value.h"
#include "storage/field/field.h"
#include "sql/expr/aggregator.h"
#include "storage/common/chunk.h"

class Tuple;
class ParsedSqlNode;

/**
 * @defgroup Expression
 * @brief 表达式
 */

/**
 * @brief 表达式类型
 * @ingroup Expression
 */
enum class ExprType
{
  NONE,
  STAR,                 ///< 星号，表示所有字段
  UNBOUND_FIELD,        ///< 未绑定的字段，需要在resolver阶段解析为FieldExpr
  UNBOUND_AGGREGATION,  ///< 未绑定的聚合函数，需要在resolver阶段解析为AggregateExpr

  FIELD,        ///< 字段。在实际执行时，根据行数据内容提取对应字段的值
  VALUE,        ///< 常量值
  CAST,         ///< 需要做类型转换的表达式
  COMPARISON,   ///< 需要做比较的表达式
  CONJUNCTION,  ///< 多个表达式使用同一种关系(AND或OR)来联结
  ARITHMETIC,   ///< 算术运算
  AGGREGATION,  ///< 聚合运算
  FUNCTION,     ///< 标量函数表达式（如 LENGTH/ROUND/DATE_FORMAT）
  SUBQUERY,     ///< 子查询表达式（返回单列结果）
  IN_LIST,      ///< IN/NOT IN 表达式（右侧可以为子查询）
};

/**
 * @brief 表达式的抽象描述
 * @ingroup Expression
 * @details 在SQL的元素中，任何需要得出值的元素都可以使用表达式来描述
 * 比如获取某个字段的值、比较运算、类型转换
 * 当然还有一些当前没有实现的表达式，比如算术运算。
 *
 * 通常表达式的值，是在真实的算子运算过程中，拿到具体的tuple后
 * 才能计算出来真实的值。但是有些表达式可能就表示某一个固定的
 * 值，比如ValueExpr。
 *
 * TODO 区分unbound和bound的表达式
 */
class Expression
{
public:
  Expression() = default;

  virtual ~Expression() = default;

  /**
   * @brief 复制表达式
   */
  virtual unique_ptr<Expression> copy() const = 0;

  /**
   * @brief 判断两个表达式是否相等
   */
  virtual bool equal(const Expression &other) const { return false; }
  /**
   * @brief 根据具体的tuple，来计算当前表达式的值。tuple有可能是一个具体某个表的行数据
   */
  virtual RC get_value(const Tuple &tuple, Value &value) const = 0;

  /**
   * @brief 在没有实际运行的情况下，也就是无法获取tuple的情况下，尝试获取表达式的值
   * @details 有些表达式的值是固定的，比如ValueExpr，这种情况下可以直接获取值
   */
  virtual RC try_get_value(Value &value) const { return RC::UNIMPLEMENTED; }

  /**
   * @brief 从 `chunk` 中获取表达式的计算结果 `column`
   */
  virtual RC get_column(Chunk &chunk, Column &column) { return RC::UNIMPLEMENTED; }

  /**
   * @brief 表达式的类型
   * 可以根据表达式类型来转换为具体的子类
   */
  virtual ExprType type() const = 0;

  /**
   * @brief 表达式值的类型
   * @details 一个表达式运算出结果后，只有一个值
   */
  virtual AttrType value_type() const = 0;

  /**
   * @brief 表达式值的长度
   */
  virtual int value_length() const { return -1; }

  /**
   * @brief 表达式的名字，比如是字段名称，或者用户在执行SQL语句时输入的内容
   */
  virtual const char *name() const { return alias_.empty() ? name_.c_str() : alias_.c_str(); }
  virtual void        set_name(string name) { name_ = name; }
  // 列别名：仅用于结果展示，不参与计算与比较
  virtual void        set_alias(const string &alias) { alias_ = alias; }
  virtual const char *alias() const { return alias_.empty() ? nullptr : alias_.c_str(); }

  /**
   * @brief 表达式在下层算子返回的 chunk 中的位置
   */
  virtual int  pos() const { return pos_; }
  virtual void set_pos(int pos) { pos_ = pos; }

  /**
   * @brief 用于 ComparisonExpr 获得比较结果 `select`。
   */
  virtual RC eval(Chunk &chunk, vector<uint8_t> &select) { return RC::UNIMPLEMENTED; }

protected:
  /**
   * @brief 表达式在下层算子返回的 chunk 中的位置
   * @details 当 pos_ = -1 时表示下层算子没有在返回的 chunk 中计算出该表达式的计算结果，
   * 当 pos_ >= 0时表示在下层算子中已经计算出该表达式的值（比如聚合表达式），且该表达式对应的结果位于
   * chunk 中 下标为 pos_ 的列中。
   */
  int pos_ = -1;

private:
  string name_;
  string alias_;
};

class StarExpr : public Expression
{
public:
  StarExpr() : table_name_() {}
  StarExpr(const char *table_name) : table_name_(table_name) {}
  virtual ~StarExpr() = default;

  unique_ptr<Expression> copy() const override { return make_unique<StarExpr>(table_name_.c_str()); }

  ExprType type() const override { return ExprType::STAR; }
  AttrType value_type() const override { return AttrType::UNDEFINED; }

  RC get_value(const Tuple &tuple, Value &value) const override { return RC::UNIMPLEMENTED; }  // 不需要实现

  const char *table_name() const { return table_name_.c_str(); }

private:
  string table_name_;
};

class UnboundFieldExpr : public Expression
{
public:
  UnboundFieldExpr(const string &table_name, const string &field_name)
      : table_name_(table_name), field_name_(field_name)
  {}

  virtual ~UnboundFieldExpr() = default;

  unique_ptr<Expression> copy() const override { return make_unique<UnboundFieldExpr>(table_name_, field_name_); }

  ExprType type() const override { return ExprType::UNBOUND_FIELD; }
  AttrType value_type() const override { return AttrType::UNDEFINED; }

  RC get_value(const Tuple &tuple, Value &value) const override { return RC::INTERNAL; }

  const char *table_name() const { return table_name_.c_str(); }
  const char *field_name() const { return field_name_.c_str(); }

private:
  string table_name_;
  string field_name_;
};

/**
 * @brief 字段表达式
 * @ingroup Expression
 */
class FieldExpr : public Expression
{
public:
  FieldExpr() = default;
  FieldExpr(const Table *table, const FieldMeta *field) : field_(table, field) {}
  FieldExpr(const Field &field) : field_(field) {}

  virtual ~FieldExpr() = default;

  bool equal(const Expression &other) const override;

  unique_ptr<Expression> copy() const override { return make_unique<FieldExpr>(field_); }

  ExprType type() const override { return ExprType::FIELD; }
  AttrType value_type() const override { return field_.attr_type(); }
  int      value_length() const override { return field_.meta()->len(); }

  Field &field() { return field_; }

  const Field &field() const { return field_; }

  const char *table_name() const { return field_.table_name(); }
  const char *field_name() const { return field_.field_name(); }

  RC get_column(Chunk &chunk, Column &column) override;

  RC get_value(const Tuple &tuple, Value &value) const override;

private:
  Field field_;
};

/**
 * @brief 常量值表达式
 * @ingroup Expression
 */
class ValueExpr : public Expression
{
public:
  ValueExpr() = default;
  explicit ValueExpr(const Value &value) : value_(value) {}

  virtual ~ValueExpr() = default;

  bool equal(const Expression &other) const override;

  unique_ptr<Expression> copy() const override { return make_unique<ValueExpr>(value_); }

  RC get_value(const Tuple &tuple, Value &value) const override;
  RC get_column(Chunk &chunk, Column &column) override;
  RC try_get_value(Value &value) const override
  {
    value = value_;
    return RC::SUCCESS;
  }

  ExprType type() const override { return ExprType::VALUE; }
  AttrType value_type() const override { return value_.attr_type(); }
  int      value_length() const override { return value_.length(); }

  void         get_value(Value &value) const { value = value_; }
  const Value &get_value() const { return value_; }

private:
  Value value_;
};

/**
 * @brief 类型转换表达式
 * @ingroup Expression
 */
class CastExpr : public Expression
{
public:
  CastExpr(unique_ptr<Expression> child, AttrType cast_type);
  virtual ~CastExpr();

  unique_ptr<Expression> copy() const override { return make_unique<CastExpr>(child_->copy(), cast_type_); }

  ExprType type() const override { return ExprType::CAST; }

  RC get_value(const Tuple &tuple, Value &value) const override;
  RC get_column(Chunk &chunk, Column &column) override;

  RC try_get_value(Value &value) const override;

  AttrType value_type() const override { return cast_type_; }

  unique_ptr<Expression> &child() { return child_; }

private:
  RC cast(const Value &value, Value &cast_value) const;

private:
  unique_ptr<Expression> child_;      ///< 从这个表达式转换
  AttrType               cast_type_;  ///< 想要转换成这个类型
};

/**
 * @brief 比较表达式
 * @ingroup Expression
 */
class ComparisonExpr : public Expression
{
public:
  ComparisonExpr(CompOp comp, unique_ptr<Expression> left, unique_ptr<Expression> right);
  virtual ~ComparisonExpr();

  ExprType type() const override { return ExprType::COMPARISON; }
  RC       get_value(const Tuple &tuple, Value &value) const override;
  AttrType value_type() const override { return AttrType::BOOLEANS; }
  CompOp   comp() const { return comp_; }

  unique_ptr<Expression> copy() const override
  {
    return make_unique<ComparisonExpr>(comp_, left_->copy(), right_->copy());
  }

  /**
   * @brief 根据 ComparisonExpr 获得 `select` 结果。
   * select 的长度与chunk 的行数相同，表示每一行在ComparisonExpr 计算后是否会被输出。
   */
  RC eval(Chunk &chunk, vector<uint8_t> &select) override;

  unique_ptr<Expression> &left() { return left_; }
  unique_ptr<Expression> &right() { return right_; }

  /**
   * 尝试在没有tuple的情况下获取当前表达式的值
   * 在优化的时候，可能会使用到
   */
  RC try_get_value(Value &value) const override;

  /**
   * compare the two tuple cells
   * @param value the result of comparison
   */
  RC compare_value(const Value &left, const Value &right, bool &value) const;

  /**
   * perform LIKE pattern matching
   * @param text the text to match
   * @param pattern the pattern with % and _ wildcards
   * @return true if text matches pattern
   */
  bool like_match(const char *text, const char *pattern) const;

  template <typename T>
  RC compare_column(const Column &left, const Column &right, vector<uint8_t> &result) const;

private:
  CompOp                 comp_;
  unique_ptr<Expression> left_;
  unique_ptr<Expression> right_;
};

/**
 * @brief 联结表达式
 * @ingroup Expression
 * 多个表达式使用同一种关系(AND或OR)来联结
 * 当前miniob仅有AND操作
 */
class ConjunctionExpr : public Expression
{
public:
  enum class Type
  {
    AND,
    OR
  };

public:
  ConjunctionExpr(Type type, vector<unique_ptr<Expression>> &children);
  virtual ~ConjunctionExpr() = default;

  unique_ptr<Expression> copy() const override
  {
    vector<unique_ptr<Expression>> children;
    for (auto &child : children_) {
      children.emplace_back(child->copy());
    }
    return make_unique<ConjunctionExpr>(conjunction_type_, children);
  }

  ExprType type() const override { return ExprType::CONJUNCTION; }
  AttrType value_type() const override { return AttrType::BOOLEANS; }
  RC       get_value(const Tuple &tuple, Value &value) const override;

  Type conjunction_type() const { return conjunction_type_; }

  vector<unique_ptr<Expression>> &children() { return children_; }

private:
  Type                           conjunction_type_;
  vector<unique_ptr<Expression>> children_;
};

/**
 * @brief 算术表达式
 * @ingroup Expression
 */
class ArithmeticExpr : public Expression
{
public:
  enum class Type
  {
    ADD,
    SUB,
    MUL,
    DIV,
    NEGATIVE,
  };

public:
  ArithmeticExpr(Type type, Expression *left, Expression *right);
  ArithmeticExpr(Type type, unique_ptr<Expression> left, unique_ptr<Expression> right);
  virtual ~ArithmeticExpr() = default;

  unique_ptr<Expression> copy() const override
  {
    if (right_) {
      return make_unique<ArithmeticExpr>(arithmetic_type_, left_->copy(), right_->copy());
    } else {
      return make_unique<ArithmeticExpr>(arithmetic_type_, left_->copy(), nullptr);
    }
  }

  bool     equal(const Expression &other) const override;
  ExprType type() const override { return ExprType::ARITHMETIC; }

  AttrType value_type() const override;
  int value_length() const override { return std::max(left_->value_length(), right_ ? right_->value_length() : 0); };

  RC get_value(const Tuple &tuple, Value &value) const override;

  RC get_column(Chunk &chunk, Column &column) override;

  RC try_get_value(Value &value) const override;

  Type arithmetic_type() const { return arithmetic_type_; }

  unique_ptr<Expression> &left() { return left_; }
  unique_ptr<Expression> &right() { return right_; }

private:
  RC calc_value(const Value &left_value, const Value &right_value, Value &value) const;

  RC calc_column(const Column &left_column, const Column &right_column, Column &column) const;

  template <bool LEFT_CONSTANT, bool RIGHT_CONSTANT>
  RC execute_calc(const Column &left, const Column &right, Column &result, Type type, AttrType attr_type) const;

private:
  Type                   arithmetic_type_;
  unique_ptr<Expression> left_;
  unique_ptr<Expression> right_;
};

class UnboundAggregateExpr : public Expression
{
public:
  UnboundAggregateExpr(const char *aggregate_name, Expression *child);
  UnboundAggregateExpr(const char *aggregate_name, unique_ptr<Expression> child);
  virtual ~UnboundAggregateExpr() = default;

  ExprType type() const override { return ExprType::UNBOUND_AGGREGATION; }

  unique_ptr<Expression> copy() const override
  {
    auto ptr = make_unique<UnboundAggregateExpr>(aggregate_name_.c_str(), child_->copy());
    ptr->set_arg_count(arg_count_);
    return ptr;
  }

  const char *aggregate_name() const { return aggregate_name_.c_str(); }

  unique_ptr<Expression> &child() { return child_; }

  RC       get_value(const Tuple &tuple, Value &value) const override { return RC::INTERNAL; }
  AttrType value_type() const override { return child_->value_type(); }

  // 记录聚合实参个数（用于在 binder 中做校验）。默认 1
  void set_arg_count(int n) { arg_count_ = n; }
  int  arg_count() const { return arg_count_; }

private:
  string                 aggregate_name_;
  unique_ptr<Expression> child_;
  int                    arg_count_ {1};
};

class AggregateExpr : public Expression
{
public:
  enum class Type
  {
    COUNT,
    SUM,
    AVG,
    MAX,
    MIN,
  };

public:
  AggregateExpr(Type type, Expression *child);
  AggregateExpr(Type type, unique_ptr<Expression> child);
  virtual ~AggregateExpr() = default;

  bool equal(const Expression &other) const override;

  unique_ptr<Expression> copy() const override { return make_unique<AggregateExpr>(aggregate_type_, child_->copy()); }

  ExprType type() const override { return ExprType::AGGREGATION; }

  AttrType value_type() const override
  {
    if (aggregate_type_ == Type::COUNT) {
      return AttrType::INTS;
    } else if (aggregate_type_ == Type::AVG) {
      return AttrType::FLOATS;
    } else {
      return child_->value_type();
    }
  }
  int value_length() const override
  {
    if (aggregate_type_ == Type::COUNT) {
      return sizeof(int);
    } else if (aggregate_type_ == Type::AVG) {
      return sizeof(float);
    } else {
      return child_->value_length();
    }
  }

  RC get_value(const Tuple &tuple, Value &value) const override;

  RC get_column(Chunk &chunk, Column &column) override;

  Type aggregate_type() const { return aggregate_type_; }

  unique_ptr<Expression> &child() { return child_; }

  const unique_ptr<Expression> &child() const { return child_; }

  unique_ptr<Aggregator> create_aggregator() const;

public:
  static RC type_from_string(const char *type_str, Type &type);

private:
  Type                   aggregate_type_;
  unique_ptr<Expression> child_;
};

/**
 * @brief 子查询表达式，表示形如 (select ...) 的结果
 * @details 当前仅支持返回单列结果。对于标量上下文，要求返回至多一行；
 *          对于 IN 上下文，允许返回多行（单列）用于集合判断。
 */
class SubqueryExpr : public Expression
{
public:
  // 使用解析好的子查询节点构造
  explicit SubqueryExpr(std::unique_ptr<ParsedSqlNode> subquery_node);
  // 仅使用已执行结果构造（用于 copy）
  SubqueryExpr(const std::vector<Value> &cached_results, AttrType result_type, int result_len);
  virtual ~SubqueryExpr() = default;

  unique_ptr<Expression> copy() const override;

  ExprType type() const override { return ExprType::SUBQUERY; }
  AttrType value_type() const override { return result_type_; }
  int      value_length() const override { return result_len_; }

  RC get_value(const Tuple &tuple, Value &value) const override;

  // 子查询不支持列式向量化，必要时可扩展
  RC get_column(Chunk &chunk, Column &column) override { return RC::UNIMPLEMENTED; }

  // 执行子查询（懒执行，缓存结果），提取首列所有值
  RC execute_once() const;

  // 带外层元组上下文执行子查询；如存在相关引用，每次调用都会重新执行
  RC execute_with_context(const Tuple *outer_tuple) const;

  // 获取缓存的所有结果（首列）
  const std::vector<Value> &results() const { return results_; }

private:
  // 深拷贝 ParsedSqlNode（当前仅支持 SELECT）
  std::unique_ptr<ParsedSqlNode> deep_copy_parsed_node(const ParsedSqlNode &node) const;

  // 深拷贝并在需要时将对外层表的引用替换为常量
  std::unique_ptr<ParsedSqlNode> deep_copy_parsed_node_with_ctx(
      const ParsedSqlNode &node, const Tuple &outer_tuple, bool &did_substitute) const;

  // 复制并替换表达式树中对外层表字段的引用
  std::unique_ptr<Expression> copy_and_substitute_outer_refs(
      const Expression &expr,
      const std::vector<std::string> &inner_relations,
      const Tuple &outer_tuple,
      bool &did_substitute,
      RC &rc) const;

private:
  mutable bool                         executed_   = false;
  mutable std::vector<Value>           results_;      ///< 首列所有结果
  mutable AttrType                     result_type_ = AttrType::UNDEFINED;
  mutable int                          result_len_  = -1;
  std::unique_ptr<ParsedSqlNode>       subquery_node_;
};

/**
 * @brief IN/NOT IN 表达式
 */
class InExpr : public Expression
{
public:
  InExpr(std::unique_ptr<Expression> test_expr, std::unique_ptr<Expression> set_expr, bool not_in)
      : test_expr_(std::move(test_expr)), set_expr_(std::move(set_expr)), not_in_(not_in)
  {}

  virtual ~InExpr() = default;

  unique_ptr<Expression> copy() const override
  {
    return make_unique<InExpr>(test_expr_->copy(), set_expr_->copy(), not_in_);
  }

  ExprType type() const override { return ExprType::IN_LIST; }
  AttrType value_type() const override { return AttrType::BOOLEANS; }

  RC get_value(const Tuple &tuple, Value &value) const override;

  unique_ptr<Expression> &test_expr() { return test_expr_; }
  unique_ptr<Expression> &set_expr() { return set_expr_; }
  bool not_in() const { return not_in_; }

private:
  std::unique_ptr<Expression> test_expr_;
  std::unique_ptr<Expression> set_expr_;  // 期望为 SubqueryExpr
  bool                         not_in_ = false;
};

/**
 * @brief 标量函数表达式：支持 LENGTH/ROUND/DATE_FORMAT 三个函数
 */
class ScalarFunctionExpr : public Expression
{
public:
  enum class FuncType { LENGTH, ROUND, DATE_FORMAT };

  explicit ScalarFunctionExpr(FuncType func_type, std::unique_ptr<Expression> child)
      : func_type_(func_type), child_(std::move(child))
  {}
  explicit ScalarFunctionExpr(FuncType func_type, Expression *child)
      : func_type_(func_type), child_(child)
  {}

  virtual ~ScalarFunctionExpr() = default;

  unique_ptr<Expression> copy() const override
  {
    return make_unique<ScalarFunctionExpr>(func_type_, child_ ? child_->copy().release() : nullptr);
  }

  ExprType type() const override { return ExprType::FUNCTION; }

  AttrType value_type() const override
  {
    switch (func_type_) {
      case FuncType::LENGTH: return AttrType::INTS;
      case FuncType::ROUND: return AttrType::FLOATS;
      case FuncType::DATE_FORMAT: return AttrType::CHARS;
    }
    return AttrType::UNDEFINED;
  }

  int value_length() const override
  {
    switch (func_type_) {
      case FuncType::LENGTH: return sizeof(int);
      case FuncType::ROUND: return sizeof(float);
      case FuncType::DATE_FORMAT: return 10; // YYYY-MM-DD
    }
    return -1;
  }

  RC get_value(const Tuple &tuple, Value &value) const override;
  RC get_column(Chunk &chunk, Column &column) override;
  RC try_get_value(Value &value) const override;

  unique_ptr<Expression> &child() { return child_; }
  const unique_ptr<Expression> &child() const { return child_; }
  FuncType function_type() const { return func_type_; }

private:
  FuncType                 func_type_;
  unique_ptr<Expression>   child_;
};
