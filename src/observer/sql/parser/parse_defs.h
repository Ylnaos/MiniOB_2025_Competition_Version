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
// Created by Meiyi
//

#pragma once

#include "common/lang/string.h"
#include "common/lang/vector.h"
#include "common/lang/memory.h"
#include "common/value.h"
#include "common/lang/utility.h"

class Expression;
class ParsedSqlNode;  // forward declaration for CTAS

/**
 * @defgroup SQLParser SQL Parser
 */

/**
 * @brief 描述一个属性
 * @ingroup SQLParser
 * @details 属性，或者说字段(column, field)
 * Rel -> Relation
 * Attr -> Attribute
 */
struct RelAttrSqlNode
{
  string relation_name;   ///< relation name (may be NULL) 表名
  string attribute_name;  ///< attribute name              属性名
};

/**
 * @brief FROM 子句中的表及其可选别名
 */
struct RelationSqlNode
{
  string relation_name;  ///< 真实表名
  string alias;          ///< 可选表别名，未指定则为空
};


/**
 * @brief 描述比较运算符
 * @ingroup SQLParser
 */
enum CompOp
{
  EQUAL_TO,     ///< "="
  LESS_EQUAL,   ///< "<="
  NOT_EQUAL,    ///< "<>"
  LESS_THAN,    ///< "<"
  GREAT_EQUAL,  ///< ">="
  GREAT_THAN,   ///< ">"
  LIKE_OP,      ///< "LIKE"
  NOT_LIKE,     ///< "NOT LIKE"
  IN_OP,        ///< "IN (subquery)"
  NOT_IN_OP,    ///< "NOT IN (subquery)"
  IS_NULL,      ///< "IS NULL"
  IS_NOT_NULL,  ///< "IS NOT NULL"
  NO_OP
};

/**
 * @brief 表示一个条件比较
 * @ingroup SQLParser
 * @details 条件比较就是SQL查询中的 where a>b 这种。
 * 一个条件比较是有两部分组成的，称为左边和右边。
 * 左边和右边理论上都可以是任意的数据，比如是字段（属性，列），也可以是数值常量。
 * 这个结构中记录的仅仅支持字段和值。
 */
struct ConditionSqlNode
{
  int left_is_attr;              ///< TRUE if left-hand side is an attribute
                                 ///< 1时，操作符左边是属性名，0时，是属性值
  Value          left_value;     ///< left-hand side value if left_is_attr = FALSE
  RelAttrSqlNode left_attr;      ///< left-hand side attribute
  CompOp         comp;           ///< comparison operator
  int            right_is_attr;  ///< TRUE if right-hand side is an attribute
                                 ///< 1时，操作符右边是属性名，0时，是属性值
  RelAttrSqlNode right_attr;     ///< right-hand side attribute if right_is_attr = TRUE 右边的属性
  Value          right_value;    ///< right-hand side value if right_is_attr = FALSE

  // 新增：支持表达式
  unique_ptr<Expression> left_expr;   ///< 左边的表达式
  unique_ptr<Expression> right_expr;  ///< 右边的表达式

  // 添加默认构造函数
  ConditionSqlNode() = default;

  // 添加显式析构函数声明（定义在 parse_defs.cpp 中）
  ~ConditionSqlNode();

  // 添加移动构造函数（声明，定义在 parse_defs.cpp 中）
  ConditionSqlNode(ConditionSqlNode&& other) noexcept;

  // 添加移动赋值运算符（声明，定义在 parse_defs.cpp 中）
  ConditionSqlNode& operator=(ConditionSqlNode&& other) noexcept;

  // 删除拷贝构造函数和拷贝赋值运算符
  ConditionSqlNode(const ConditionSqlNode&) = delete;
  ConditionSqlNode& operator=(const ConditionSqlNode&) = delete;
};

/**
 * @brief 表示一个 ORDER BY 条目：表达式 + 升降序
 */
struct OrderBySqlNode
{
  unique_ptr<Expression> expression;  ///< 排序用的表达式（当前仅需支持字段）
  bool                    asc = true; ///< 是否升序，默认升序
};

/**
 * @brief 描述一个select语句
 * @ingroup SQLParser
 * @details 一个正常的select语句描述起来比这个要复杂很多，这里做了简化。
 * 一个select语句由三部分组成，分别是select, from, where。
 * select部分表示要查询的字段，from部分表示要查询的表，where部分表示查询的条件。
 * 比如 from 中可以是多个表，也可以是另一个查询语句，这里仅仅支持表，也就是 relations。
 * where 条件 conditions，这里表示使用AND串联起来多个条件。正常的SQL语句会有OR，NOT等，
 * 甚至可以包含复杂的表达式。
 */

struct SelectSqlNode
{
  vector<unique_ptr<Expression>> expressions;  ///< 查询的表达式
  vector<RelationSqlNode>                 relations;    ///< 查询的表
  vector<ConditionSqlNode>       conditions;   ///< 查询条件，使用AND串联起来多个条件
  unique_ptr<Expression>         where_expr;   ///< 可选：布尔表达式（支持 AND/OR），与 conditions 互斥
  vector<unique_ptr<Expression>> group_by;     ///< group by clause
  vector<ConditionSqlNode>       having;       ///< having clause (AND chained conditions)
  vector<OrderBySqlNode>         order_by;     ///< order by clause
  int                            limit_num = -1; ///< LIMIT count, -1 means no limit

  SelectSqlNode() = default;
  ~SelectSqlNode();  // 定义在 parse_defs.cpp 中
};

/**
 * @brief 算术表达式计算的语法树
 * @ingroup SQLParser
 */
struct CalcSqlNode
{
  vector<unique_ptr<Expression>> expressions;  ///< calc clause

  CalcSqlNode() = default;
  ~CalcSqlNode();  // 定义在 parse_defs.cpp 中
};

/**
 * @brief 描述一个insert语句
 * @ingroup SQLParser
 * @details 于Selects类似，也做了很多简化
 */
struct InsertSqlNode
{
  string                         relation_name;  ///< Relation to insert into
  vector<string>                 attribute_names;///< 可选：指定插入的列名列表（为空表示插入所有列）
  vector<vector<Value>>          rows;           ///< 多行插入的值，每个子 vector 表示一行
};

/**
 * @brief 描述一个delete语句
 * @ingroup SQLParser
 */
struct DeleteSqlNode
{
  string                   relation_name;  ///< Relation to delete from
  vector<ConditionSqlNode> conditions;
};

/**
 * @brief 描述一个update语句
 * @ingroup SQLParser
 */
struct UpdateSqlNode
{
  string                   relation_name;    ///< Relation to update
  vector<string>           attribute_names;  ///< 更新的字段列表，支持多个字段
  vector<Value>            values;           ///< 更新的值列表，与字段一一对应
  vector<Expression *>     value_expressions;///< 更新的表达式列表，与字段一一对应
  vector<ConditionSqlNode> conditions;
};

/**
 * @brief 描述一个属性
 * @ingroup SQLParser
 * @details 属性，或者说字段(column, field)
 */
struct AttrInfoSqlNode
{
  AttrType type;    ///< Type of attribute
  string   name;    ///< Attribute name
  size_t   length;  ///< Length of attribute
  bool     nullable = false; ///< Whether the attribute allows NULL (default false)
};

/**
 * @brief 描述一个create table语句
 * @ingroup SQLParser
 * @details 这里也做了很多简化。
 */
struct CreateTableSqlNode
{
  string                  relation_name;  ///< Relation name
  vector<AttrInfoSqlNode> attr_infos;     ///< attributes
  vector<string>          primary_keys;   ///< primary keys
  // TODO: integrate to CreateTableOptions
  string storage_format;  ///< storage format
  string storage_engine;  ///< storage engine
  // CTAS: optional subquery to build schema and data
  std::unique_ptr<ParsedSqlNode> as_select;  ///< non-null when using `CREATE TABLE ... AS SELECT ...`
};

/**
 * @brief 描述一个drop table语句
 * @ingroup SQLParser
 */
struct DropTableSqlNode
{
  string relation_name;  ///< 要删除的表名
};

/**
 * @brief 描述一个analyze table语句
 * @ingroup SQLParser
 */
struct AnalyzeTableSqlNode
{
  string relation_name;  ///< 要分析的表名
};

/**
 * @brief 描述一个create index语句
 * @ingroup SQLParser
 * @details 创建索引时，需要指定索引名，表名，字段名。
 * 正常的SQL语句中，一个索引可能包含了多个字段，这里仅支持一个字段。
 */
struct CreateIndexSqlNode
{
  string              index_name;     ///< Index name
  string              relation_name;  ///< Relation name
  vector<string>      attribute_names;///< Attribute names (support multi-column)
  bool                unique = false; ///< Whether the index is UNIQUE

  // Vector index specific parameters
  bool                is_vector_index = false;  ///< Whether this is a vector index
  string              distance_type;            ///< Distance type: l2_distance, cosine_distance, inner_product
  string              index_type;               ///< Index type: ivfflat
  int                 lists = 0;                ///< Number of clusters for IVF-Flat
  int                 probes = 0;               ///< Number of probes during search
};

/**
 * @brief 描述一个drop index语句
 * @ingroup SQLParser
 */
struct DropIndexSqlNode
{
  string index_name;     ///< Index name
  string relation_name;  ///< Relation name
};

/**
 * @brief 描述一个desc table语句
 * @ingroup SQLParser
 * @details desc table 是查询表结构信息的语句
 */
struct DescTableSqlNode
{
  string relation_name;
};

/**
 * @brief 描述一个load data语句
 * @ingroup SQLParser
 * @details 从文件导入数据到表中。文件中的每一行就是一条数据，每行的数据类型、字段个数都与表保持一致
 */
struct LoadDataSqlNode
{
  string relation_name;
  string file_name;
  string terminated = ",";
  string enclosed   = "\"";
};

/**
 * @brief 设置变量的值
 * @ingroup SQLParser
 * @note 当前还没有查询变量
 */
struct SetVariableSqlNode
{
  string name;
  Value  value;
};

class ParsedSqlNode;

/**
 * @brief 描述一个explain语句
 * @ingroup SQLParser
 * @details 会创建operator的语句，才能用explain输出执行计划。
 * 一个command就是一个语句，比如select语句，insert语句等。
 * 可能改成SqlCommand更合适。
 */
struct ExplainSqlNode
{
  unique_ptr<ParsedSqlNode> sql_node;
};

/**
 * @brief 描述一个 create view 语句
 */
struct CreateViewSqlNode
{
  string                    view_name;       ///< 视图名称
  vector<string>            view_fields;     ///< 视图定义的列名（如果有的话）
  string                    view_select_sql; ///< 视图定义的 SELECT 子句文本（改写后文本）
  unique_ptr<ParsedSqlNode> select_node;     ///< 解析好的 SELECT 语法树（可选）
};

/**
 * @brief 解析SQL语句出现了错误
 * @ingroup SQLParser
 * @details 当前解析时并没有处理错误的行号和列号
 */
struct ErrorSqlNode
{
  string error_msg;
  int    line;
  int    column;
};

/**
 * @brief 表示一个SQL语句的类型
 * @ingroup SQLParser
 */
enum SqlCommandFlag
{
  SCF_ERROR = 0,
  SCF_CALC,
  SCF_SELECT,
  SCF_INSERT,
  SCF_UPDATE,
  SCF_DELETE,
  SCF_CREATE_TABLE,
  SCF_CREATE_VIEW,
  SCF_DROP_TABLE,
  SCF_ANALYZE_TABLE,
  SCF_CREATE_INDEX,
  SCF_DROP_INDEX,
  SCF_SYNC,
  SCF_SHOW_TABLES,
  SCF_DESC_TABLE,
  SCF_BEGIN,  ///< 事务开始语句，可以在这里扩展只读事务
  SCF_COMMIT,
  SCF_CLOG_SYNC,
  SCF_ROLLBACK,
  SCF_LOAD_DATA,
  SCF_HELP,
  SCF_EXIT,
  SCF_EXPLAIN,
  SCF_SET_VARIABLE,  ///< 设置变量
};
/**
 * @brief 表示一个SQL语句
 * @ingroup SQLParser
 */
class ParsedSqlNode
{
public:
  enum SqlCommandFlag flag;
  ErrorSqlNode        error;
  CalcSqlNode         calc;
  SelectSqlNode       selection;
  InsertSqlNode       insertion;
  DeleteSqlNode       deletion;
  UpdateSqlNode       update;
  CreateTableSqlNode  create_table;
  CreateViewSqlNode   create_view;
  DropTableSqlNode    drop_table;
  AnalyzeTableSqlNode analyze_table;
  CreateIndexSqlNode  create_index;
  DropIndexSqlNode    drop_index;
  DescTableSqlNode    desc_table;
  LoadDataSqlNode     load_data;
  ExplainSqlNode      explain;
  SetVariableSqlNode  set_variable;

public:
  ParsedSqlNode();
  explicit ParsedSqlNode(SqlCommandFlag flag);
};

/**
 * @brief 表示语法解析后的数据
 * @ingroup SQLParser
 */
class ParsedSqlResult
{
public:
  void add_sql_node(unique_ptr<ParsedSqlNode> sql_node);

  vector<unique_ptr<ParsedSqlNode>> &sql_nodes() { return sql_nodes_; }

private:
  vector<unique_ptr<ParsedSqlNode>> sql_nodes_;  ///< 这里记录SQL命令。虽然看起来支持多个，但是当前仅处理一个
};
