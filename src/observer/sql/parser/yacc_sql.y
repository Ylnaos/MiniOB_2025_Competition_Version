
%{

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <utility>

#include "common/log/log.h"
#include "common/lang/string.h"
#include "sql/parser/parse_defs.h"
#include "sql/parser/yacc_sql.hpp"
#include "sql/parser/lex_sql.h"
#include "sql/expr/expression.h"

using namespace std;

string token_name(const char *sql_string, YYLTYPE *llocp)
{
  return string(sql_string + llocp->first_column, llocp->last_column - llocp->first_column + 1);
}

int yyerror(YYLTYPE *llocp, const char *sql_string, ParsedSqlResult *sql_result, yyscan_t scanner, const char *msg)
{
  unique_ptr<ParsedSqlNode> error_sql_node = make_unique<ParsedSqlNode>(SCF_ERROR);
  error_sql_node->error.error_msg = msg;
  error_sql_node->error.line = llocp->first_line;
  error_sql_node->error.column = llocp->first_column;
  sql_result->add_sql_node(std::move(error_sql_node));
  return 0;
}

ArithmeticExpr *create_arithmetic_expression(ArithmeticExpr::Type type,
                                             Expression *left,
                                             Expression *right,
                                             const char *sql_string,
                                             YYLTYPE *llocp)
{
  ArithmeticExpr *expr = new ArithmeticExpr(type, left, right);
  expr->set_name(token_name(sql_string, llocp));
  return expr;
}

UnboundAggregateExpr *create_aggregate_expression(const char *aggregate_name,
                                           Expression *child,
                                           const char *sql_string,
                                           YYLTYPE *llocp)
{
  UnboundAggregateExpr *expr = new UnboundAggregateExpr(aggregate_name, child);
  expr->set_name(token_name(sql_string, llocp));
  return expr;
}

// 收集 JOIN ... ON 布尔表达式（使用 AND 连接）
static Expression *g_join_on_expr = nullptr;

%}

%define api.pure full
%define parse.error verbose
/** 启用位置标识 **/
%locations
%lex-param { yyscan_t scanner }
/** 这些定义了在yyparse函数中的参数 **/
%parse-param { const char * sql_string }
%parse-param { ParsedSqlResult * sql_result }
%parse-param { void * scanner }

//标识tokens
%token  SEMICOLON
        BY
        ORDER
        HAVING
        OR
        CREATE
        VIEW
        DROP
        GROUP
        TABLE
        TABLES
        INDEX
        UNIQUE
        CALC
        SELECT
        DESC
        AS
        ASC
        SHOW
        SYNC
        INSERT
        DELETE
        UPDATE
        LBRACE
        RBRACE
        COMMA
        TRX_BEGIN
        TRX_COMMIT
        TRX_ROLLBACK
        INT_T
        STRING_T
        FLOAT_T
        DATE_T
        TEXT_T
        VECTOR_T
        HELP
        EXIT
        DOT //QUOTE
        INTO
        VALUES
        FROM
        WHERE
        AND
        SET
        ON
        LOAD
        DATA
        INFILE
        EXPLAIN
        STORAGE
        FORMAT
        PRIMARY
        KEY
        WITH
        DISTANCE_KW
        TYPE_KW
        LISTS_KW
        PROBES_KW
        IN
        NOT
        JOIN
        INNER
        IS
        ANALYZE
        FIELDS
        TERMINATED
        ENCLOSED
        EQ
        LT
        GT
        LE
        GE
        NE
        LIKE
        NULL_T
        NULLABLE
        LENGTH_F
        ROUND_F
        DATE_FORMAT_F
        L2_DISTANCE_F
        COSINE_DISTANCE_F
        INNER_PRODUCT_F

/** union 中定义各种数据类型，真实生成的代码也是union类型，所以不能有非POD类型的数据 **/
%union {
  ParsedSqlNode *                            sql_node;
  ConditionSqlNode *                         condition;
  Value *                                    value;
  enum CompOp                                comp;
  RelAttrSqlNode *                           rel_attr;
  vector<AttrInfoSqlNode> *                  attr_infos;
  AttrInfoSqlNode *                          attr_info;
  Expression *                               expression;
  vector<unique_ptr<Expression>> *           expression_list;
  vector<OrderBySqlNode> *                   order_by_list;
  OrderBySqlNode *                           order_by_item;
  vector<Value> *                            value_list;
  vector<vector<Value>> *                    rows;
  vector<ConditionSqlNode> *                 condition_list;
  vector<RelAttrSqlNode> *                   rel_attr_list;
  vector<RelationSqlNode> *                  relation_list;
  RelationSqlNode *                          relation_node;
  vector<string> *                           key_list;
  vector<pair<string, Expression*>> *        update_list;
  char *                                     cstring;
  int                                        number;
  float                                      floats;
  VectorIndexOptions *                       vector_index_options;
}

%destructor { delete $$; } <condition>
%destructor { delete $$; } <value>
%destructor { delete $$; } <rel_attr>
%destructor { delete $$; } <attr_infos>
%destructor { delete $$; } <expression>
%destructor { delete $$; } <expression_list>
%destructor { delete $$; } <value_list>
%destructor { delete $$; } <rows>
%destructor { delete $$; } <condition_list>
// %destructor { delete $$; } <rel_attr_list>
%destructor { delete $$; } <relation_list>
%destructor { delete $$; } <key_list>

%token <number> NUMBER
%token <floats> FLOAT
%token <cstring> ID
%token <cstring> SSS
%token <cstring> VECTOR_LITERAL
//非终结符

/** type 定义了各种解析后的结果输出的是什么类型。类型对应了 union 中的定义的成员变量名称 **/
%type <number>              type
%type <number>              nullable_opt
%type <condition>           condition
%type <value>               value
%type <number>              number
%type <relation_node>       relation
%type <comp>                comp_op
%type <rel_attr>            rel_attr
%type <attr_infos>          attr_def_list
%type <attr_info>           attr_def
%type <value_list>          value_list
%type <value_list>          row
%type <rows>                row_list
%type <condition_list>      where
%type <expression>          where_bool
%type <expression>          bool_term
%type <expression>          bool_factor
%type <expression>          condition_expr
%type <condition_list>      condition_list
%type <condition_list>      having
%type <update_list>         update_list
%type <cstring>             storage_format
%type <key_list>            primary_key
%type <key_list>            attr_list
%type <relation_list>       rel_list
%type <relation_list>       join_seq
%type <expression>          expression
%type <expression>          function_expression
%type <expression>          aggregate_expression
%type <vector_index_options> vector_index_with_opt
%type <vector_index_options> vector_index_option_list
%type <vector_index_options> vector_index_option
%type <expression_list>     expression_list
%type <expression>          select_item
%type <expression_list>     group_by
%type <order_by_list>       order_by
%type <order_by_list>       order_by_condition_list
%type <order_by_item>       order_by_condition
%type <cstring>             fields_terminated_by
%type <cstring>             enclosed_by
%type <sql_node>            calc_stmt
%type <sql_node>            select_stmt
%type <sql_node>            insert_stmt
%type <sql_node>            update_stmt
%type <sql_node>            delete_stmt
%type <sql_node>            create_table_stmt
%type <sql_node>            create_view_stmt
%type <sql_node>            drop_table_stmt
%type <sql_node>            analyze_table_stmt
%type <sql_node>            show_tables_stmt
%type <sql_node>            desc_table_stmt
%type <sql_node>            create_index_stmt
%type <sql_node>            drop_index_stmt
%type <sql_node>            sync_stmt
%type <sql_node>            begin_stmt
%type <sql_node>            commit_stmt
%type <sql_node>            rollback_stmt
%type <sql_node>            load_data_stmt
%type <sql_node>            explain_stmt
%type <sql_node>            set_variable_stmt
%type <sql_node>            help_stmt
%type <sql_node>            exit_stmt
%type <sql_node>            command_wrapper
// commands should be a list but I use a single command instead
%type <sql_node>            commands

%left '+' '-'
%left '*' '/'
%right UMINUS
%%

commands: command_wrapper opt_semicolon  //commands or sqls. parser starts here.
  {
    unique_ptr<ParsedSqlNode> sql_node = unique_ptr<ParsedSqlNode>($1);
    sql_result->add_sql_node(std::move(sql_node));
  }
  ;

command_wrapper:
    calc_stmt
  | select_stmt
  | insert_stmt
  | update_stmt
  | delete_stmt
  | create_table_stmt
  | create_view_stmt
  | drop_table_stmt
  | analyze_table_stmt
  | show_tables_stmt
  | desc_table_stmt
  | create_index_stmt
  | drop_index_stmt
  | sync_stmt
  | begin_stmt
  | commit_stmt
  | rollback_stmt
  | load_data_stmt
  | explain_stmt
  | set_variable_stmt
  | help_stmt
  | exit_stmt
    ;

exit_stmt:      
    EXIT {
      (void)yynerrs;  // 这么写为了消除yynerrs未使用的告警。如果你有更好的方法欢迎提PR
      $$ = new ParsedSqlNode(SCF_EXIT);
    };

help_stmt:
    HELP {
      $$ = new ParsedSqlNode(SCF_HELP);
    };

sync_stmt:
    SYNC {
      $$ = new ParsedSqlNode(SCF_SYNC);
    }
    ;

begin_stmt:
    TRX_BEGIN  {
      $$ = new ParsedSqlNode(SCF_BEGIN);
    }
    ;

commit_stmt:
    TRX_COMMIT {
      $$ = new ParsedSqlNode(SCF_COMMIT);
    }
    ;

rollback_stmt:
    TRX_ROLLBACK  {
      $$ = new ParsedSqlNode(SCF_ROLLBACK);
    }
    ;

drop_table_stmt:    /*drop table 语句的语法解析树*/
    DROP TABLE ID {
      $$ = new ParsedSqlNode(SCF_DROP_TABLE);
      $$->drop_table.relation_name = $3;
    };

create_view_stmt:
    CREATE VIEW ID AS select_stmt
    {
      $$ = new ParsedSqlNode(SCF_CREATE_VIEW);
      $$->create_view.view_name = $3;
      $$->create_view.view_select_sql = token_name(sql_string, &@5);
      $$->create_view.select_node.reset($5);
    }
    | CREATE VIEW ID LBRACE attr_list RBRACE AS select_stmt
    {
      $$ = new ParsedSqlNode(SCF_CREATE_VIEW);
      $$->create_view.view_name = $3;
      // 列清单(第5个符号)当前不强制校验/存储，语义阶段按原实现基于 SELECT 结果派生
      if ($5 != nullptr) { delete $5; }
      // 记录 select 子句的原始文本
      $$->create_view.view_select_sql = token_name(sql_string, &@8);
      $$->create_view.select_node.reset($8);
    }
    ;

analyze_table_stmt:  /* analyze table 语法的语法解析树*/
    ANALYZE TABLE ID {
      $$ = new ParsedSqlNode(SCF_ANALYZE_TABLE);
      $$->analyze_table.relation_name = $3;
    }
    ;

show_tables_stmt:
    SHOW TABLES {
      $$ = new ParsedSqlNode(SCF_SHOW_TABLES);
    }
    ;

desc_table_stmt:
    DESC ID  {
      $$ = new ParsedSqlNode(SCF_DESC_TABLE);
      $$->desc_table.relation_name = $2;
    }
    ;

create_index_stmt:    /*create index 语句的语法解析树*/
    CREATE VECTOR_T INDEX ID ON ID LBRACE attr_list RBRACE vector_index_with_opt
    {
      $$ = new ParsedSqlNode(SCF_CREATE_INDEX);
      CreateIndexSqlNode &create_index = $$->create_index;
      create_index.index_name = $4;
      create_index.relation_name = $6;
      if ($8 != nullptr) {
        create_index.attribute_names.swap(*$8);
        delete $8;
      }
      create_index.unique = false;
      create_index.vector_index = true;
      if ($10 != nullptr) {
        if ($10->has_distance) {
          create_index.distance_func = std::move($10->distance);
        }
        if ($10->has_type) {
          create_index.vector_index_type = std::move($10->index_type);
        }
        if ($10->has_lists) {
          create_index.lists = $10->lists;
        }
        if ($10->has_probes) {
          create_index.probes = $10->probes;
        }
        delete $10;
      }
    }
    | CREATE INDEX ID ON ID LBRACE attr_list RBRACE
    {
      $$ = new ParsedSqlNode(SCF_CREATE_INDEX);
      CreateIndexSqlNode &create_index = $$->create_index;
      create_index.index_name = $3;
      create_index.relation_name = $5;
      if ($7 != nullptr) {
        create_index.attribute_names.swap(*$7);
        delete $7;
      }
      create_index.unique = false;
    }
    | CREATE UNIQUE INDEX ID ON ID LBRACE attr_list RBRACE
    {
      $$ = new ParsedSqlNode(SCF_CREATE_INDEX);
      CreateIndexSqlNode &create_index = $$->create_index;
      create_index.index_name = $4;
      create_index.relation_name = $6;
      if ($8 != nullptr) {
        create_index.attribute_names.swap(*$8);
        delete $8;
      }
      create_index.unique = true;
    }
    ;

vector_index_with_opt:
    /* empty */
    {
      $$ = nullptr;
    }
    | WITH LBRACE vector_index_option_list RBRACE
    {
      $$ = $3;
    }
    ;

vector_index_option_list:
    vector_index_option
    {
      $$ = $1;
    }
    | vector_index_option_list COMMA vector_index_option
    {
      if ($1 == nullptr) {
        $$ = $3;
      } else {
        if ($3 != nullptr) {
          if ($3->has_distance) {
            $1->has_distance = true;
            $1->distance = std::move($3->distance);
          }
          if ($3->has_type) {
            $1->has_type = true;
            $1->index_type = std::move($3->index_type);
          }
          if ($3->has_lists) {
            $1->has_lists = true;
            $1->lists = $3->lists;
          }
          if ($3->has_probes) {
            $1->has_probes = true;
            $1->probes = $3->probes;
          }
          delete $3;
        }
        $$ = $1;
      }
    }
    ;

vector_index_option:
    DISTANCE_KW EQ L2_DISTANCE_F
    {
      auto *opt = new VectorIndexOptions();
      opt->has_distance = true;
      opt->distance = "l2_distance";
      $$ = opt;
    }
    | DISTANCE_KW EQ COSINE_DISTANCE_F
    {
      auto *opt = new VectorIndexOptions();
      opt->has_distance = true;
      opt->distance = "cosine_distance";
      $$ = opt;
    }
    | DISTANCE_KW EQ INNER_PRODUCT_F
    {
      auto *opt = new VectorIndexOptions();
      opt->has_distance = true;
      opt->distance = "inner_product";
      $$ = opt;
    }
    | DISTANCE_KW EQ ID
    {
      auto *opt = new VectorIndexOptions();
      opt->has_distance = true;
      string tmp($3);
      free($3);
      common::str_to_lower(tmp);
      opt->distance = std::move(tmp);
      $$ = opt;
    }
    | TYPE_KW EQ ID
    {
      auto *opt = new VectorIndexOptions();
      opt->has_type = true;
      string tmp($3);
      free($3);
      common::str_to_lower(tmp);
      opt->index_type = std::move(tmp);
      $$ = opt;
    }
    | LISTS_KW EQ number
    {
      auto *opt = new VectorIndexOptions();
      opt->has_lists = true;
      opt->lists = $3;
      $$ = opt;
    }
    | PROBES_KW EQ number
    {
      auto *opt = new VectorIndexOptions();
      opt->has_probes = true;
      opt->probes = $3;
      $$ = opt;
    }
    ;

drop_index_stmt:      /*drop index 语句的语法解析树*/
    DROP INDEX ID ON ID
    {
      $$ = new ParsedSqlNode(SCF_DROP_INDEX);
      $$->drop_index.index_name = $3;
      $$->drop_index.relation_name = $5;
    }
    ;
create_table_stmt:    /*create table 语句的语法解析树*/
    CREATE TABLE ID LBRACE attr_def_list primary_key RBRACE storage_format
    {
      $$ = new ParsedSqlNode(SCF_CREATE_TABLE);
      CreateTableSqlNode &create_table = $$->create_table;
      create_table.relation_name = $3;
      //free($3);

      create_table.attr_infos.swap(*$5);
      delete $5;

      if ($6 != nullptr) {
        create_table.primary_keys.swap(*$6);
        delete $6;
      }
      if ($8 != nullptr) {
        create_table.storage_format = $8;
      }
    }
    | CREATE TABLE ID LBRACE attr_def_list primary_key RBRACE AS select_stmt
    {
      $$ = new ParsedSqlNode(SCF_CREATE_TABLE);
      CreateTableSqlNode &create_table = $$->create_table;
      create_table.relation_name = $3;
      // keep user-specified columns
      create_table.attr_infos.swap(*$5);
      delete $5;
      if ($6 != nullptr) {
        create_table.primary_keys.swap(*$6);
        delete $6;
      }
      if ($9 == nullptr || $9->flag != SCF_SELECT) {
        delete $$;
        $$ = nullptr;
      } else {
        create_table.as_select.reset($9);
      }
    }
    | CREATE TABLE ID LBRACE attr_def_list primary_key RBRACE select_stmt
    {
      $$ = new ParsedSqlNode(SCF_CREATE_TABLE);
      CreateTableSqlNode &create_table = $$->create_table;
      create_table.relation_name = $3;
      // keep user-specified columns
      create_table.attr_infos.swap(*$5);
      delete $5;
      if ($6 != nullptr) {
        create_table.primary_keys.swap(*$6);
        delete $6;
      }
      // fix: the select_stmt is at position 8 in this rule
      if ($8 == nullptr || $8->flag != SCF_SELECT) {
        delete $$;
        $$ = nullptr;
      } else {
        create_table.as_select.reset($8);
      }
    }
    | CREATE TABLE ID AS select_stmt
    {
      $$ = new ParsedSqlNode(SCF_CREATE_TABLE);
      CreateTableSqlNode &create_table = $$->create_table;
      create_table.relation_name = $3;
      if ($5 == nullptr || $5->flag != SCF_SELECT) {
        // 不应发生：select_stmt 规约保证 SCF_SELECT
        delete $$;
        $$ = nullptr;
      } else {
        create_table.as_select.reset($5);
      }
    }
    ;
    
attr_def_list:
    attr_def
    {
      $$ = new vector<AttrInfoSqlNode>;
      $$->emplace_back(*$1);
      delete $1;
    }
    | attr_def_list COMMA attr_def
    {
      $$ = $1;
      $$->emplace_back(*$3);
      delete $3;
    }
    ;
    
attr_def:
    ID type LBRACE number RBRACE nullable_opt
    {
      $$ = new AttrInfoSqlNode;
      $$->type = (AttrType)$2;
      $$->name = $1;
      $$->length = $4;
      $$->nullable = ($6 != 0);
    }
    | ID type nullable_opt
    {
      $$ = new AttrInfoSqlNode;
      $$->type = (AttrType)$2;
      $$->name = $1;
      $$->length = 4;
      $$->nullable = ($3 != 0);
    }
    ;
number:
    NUMBER {$$ = $1;}
    ;
type:
    INT_T      { $$ = static_cast<int>(AttrType::INTS); }
    | STRING_T { $$ = static_cast<int>(AttrType::CHARS); }
    | TEXT_T   { $$ = static_cast<int>(AttrType::TEXTS); }
    | FLOAT_T  { $$ = static_cast<int>(AttrType::FLOATS); }
    | DATE_T   { $$ = static_cast<int>(AttrType::DATES); }
    | VECTOR_T { $$ = static_cast<int>(AttrType::VECTORS); }
    ;
primary_key:
    /* empty */
    {
      $$ = nullptr;
    }
    | COMMA PRIMARY KEY LBRACE attr_list RBRACE
    {
      $$ = $5;
    }
    ;

attr_list:
    ID {
      $$ = new vector<string>();
      $$->push_back($1);
    }
    | ID COMMA attr_list {
      if ($3 != nullptr) {
        $$ = $3;
      } else {
        $$ = new vector<string>;
      }

      $$->insert($$->begin(), $1);
    }
    ;

insert_stmt:        /*insert   语句的语法解析树*/
    INSERT INTO ID VALUES row_list
    {
      $$ = new ParsedSqlNode(SCF_INSERT);
      $$->insertion.relation_name = $3;
      $$->insertion.rows.swap(*$5);
      delete $5;
    }
    ;

value_list:
    value
    {
      $$ = new vector<Value>;
      $$->emplace_back(*$1);
      delete $1;
    }
    | value_list COMMA value { 
      $$ = $1;
      $$->emplace_back(*$3);
      delete $3;
    }
    ;
/* 一行：括号包裹的 value_list */
row:
    LBRACE value_list RBRACE
    {
      $$ = $2;
    }
    ;

/* 多行：逗号分隔多个 row */
row_list:
    row
    {
      $$ = new vector<vector<Value>>();
      $$->emplace_back(std::move(*$1));
      delete $1;
    }
    | row_list COMMA row
    {
      $$ = $1;
      $$->emplace_back(std::move(*$3));
      delete $3;
    }
    ;
value:
    NUMBER {
      $$ = new Value((int)$1);
      @$ = @1;
    }
    | '-' NUMBER {
      int val = -(int)$2;
      if (val == 0) val = 0;  // 处理-0的情况，确保为正0
      $$ = new Value(val);
      @$ = @1;
    }
    | FLOAT {
      $$ = new Value((float)$1);
      @$ = @1;
    }
    | '-' FLOAT {
      float val = -(float)$2;
      if (val == 0.0f) val = 0.0f;  // 处理-0.0的情况，确保为正0
      $$ = new Value(val);
      @$ = @1;
    }
    | NULL_T {
      $$ = new Value();
      $$->set_null();
      @$ = @1;
    }
    | SSS {
      char *tmp = common::substr($1,1,strlen($1)-2);
      $$ = new Value(tmp);
      free(tmp);
    }
    | VECTOR_LITERAL {
      $$ = new Value($1);
      @$ = @1;
    }
    ;
nullable_opt:
    /* empty */
    {
      $$ = 0;
    }
    | NULLABLE
    {
      $$ = 1;
    }
    | NULL_T
    {
      // 兼容列定义中的 "NULL" 写法，表示该列允许为 NULL
      $$ = 1;
    }
    | NOT NULL_T
    {
      // 显式声明 NOT NULL（默认即 NOT NULL，这里仅作语法兼容）
      $$ = 0;
    }
    ;
storage_format:
    /* empty */
    {
      $$ = nullptr;
    }
    | STORAGE FORMAT EQ ID
    {
      $$ = $4;
    }
    ;
    
delete_stmt:    /*  delete 语句的语法解析树*/
    DELETE FROM ID where 
    {
      $$ = new ParsedSqlNode(SCF_DELETE);
      $$->deletion.relation_name = $3;
      if ($4 != nullptr) {
        $$->deletion.conditions.swap(*$4);
        delete $4;
      }
    }
    ;
update_stmt:      /*  update 语句的语法解析树*/
    UPDATE ID SET update_list where
    {
      $$ = new ParsedSqlNode(SCF_UPDATE);
      $$->update.relation_name = $2;

      // 从update_list中提取字段名和表达式
      auto* update_pairs = $4;
      if (update_pairs != nullptr) {
        for (auto& pair : *update_pairs) {
          $$->update.attribute_names.push_back(pair.first);
          $$->update.value_expressions.push_back(pair.second);
        }
        delete update_pairs;
      }

      if ($5 != nullptr) {
        $$->update.conditions.swap(*$5);
        delete $5;
      }
    }
    ;

update_list:
    ID EQ expression
    {
      $$ = new vector<pair<string, Expression*>>;
      $$->emplace_back($1, $3);
    }
    | ID EQ expression COMMA update_list
    {
      $$ = $5;
      $$->emplace_back($1, $3);
    }
    ;
select_stmt:        /*  select 语句的语法解析树*/
    SELECT expression_list FROM rel_list where group_by having order_by
    {
      $$ = new ParsedSqlNode(SCF_SELECT);
      if ($2 != nullptr) {
        $$->selection.expressions.swap(*$2);
        delete $2;
      }

      if ($4 != nullptr) {
        $$->selection.relations.swap(*$4);
        delete $4;
      }

      if ($5 != nullptr) {
        $$->selection.conditions.swap(*$5);
        delete $5;
      }

      // 合并 JOIN ... ON 条件到 where_expr（与 where 条件共同生效）
      if (g_join_on_expr != nullptr) {
        if ($$->selection.where_expr) {
          vector<unique_ptr<Expression>> children;
          children.emplace_back($$->selection.where_expr.release());
          children.emplace_back(g_join_on_expr);
          $$->selection.where_expr.reset(new ConjunctionExpr(ConjunctionExpr::Type::AND, children));
        } else {
          $$->selection.where_expr.reset(g_join_on_expr);
        }
        g_join_on_expr = nullptr;
      }

      if ($6 != nullptr) {
        $$->selection.group_by.swap(*$6);
        delete $6;
      }

      if ($7 != nullptr) {
        $$->selection.having.swap(*$7);
        delete $7;
      }

      if ($8 != nullptr) {
        $$->selection.order_by.swap(*$8);
        delete $8;
      }
    }
    | SELECT expression_list FROM rel_list where_bool group_by having order_by
    {
      $$ = new ParsedSqlNode(SCF_SELECT);
      if ($2 != nullptr) {
        $$->selection.expressions.swap(*$2);
        delete $2;
      }

      if ($4 != nullptr) {
        $$->selection.relations.swap(*$4);
        delete $4;
      }

      // where_bool 解析到布尔表达式
      $$->selection.where_expr.reset($5);

      // 合并 JOIN ... ON 条件（AND）
      if (g_join_on_expr != nullptr) {
        if ($$->selection.where_expr) {
          vector<unique_ptr<Expression>> children;
          children.emplace_back($$->selection.where_expr.release());
          children.emplace_back(g_join_on_expr);
          $$->selection.where_expr.reset(new ConjunctionExpr(ConjunctionExpr::Type::AND, children));
        } else {
          $$->selection.where_expr.reset(g_join_on_expr);
        }
        g_join_on_expr = nullptr;
      }

      if ($6 != nullptr) {
        $$->selection.group_by.swap(*$6);
        delete $6;
      }

      if ($7 != nullptr) {
        $$->selection.having.swap(*$7);
        delete $7;
      }

      if ($8 != nullptr) {
        $$->selection.order_by.swap(*$8);
        delete $8;
      }
    }
    | SELECT expression_list
    {
      $$ = new ParsedSqlNode(SCF_SELECT);
      if ($2 != nullptr) {
        $$->selection.expressions.swap(*$2);
        delete $2;
      }
    }
    ;
calc_stmt:
    CALC expression_list
    {
      $$ = new ParsedSqlNode(SCF_CALC);
      $$->calc.expressions.swap(*$2);
      delete $2;
    }
    ;

expression_list:
    select_item
    {
      $$ = new vector<unique_ptr<Expression>>;
      $$->emplace_back($1);
    }
    | select_item COMMA expression_list
    {
      if ($3 != nullptr) {
        $$ = $3;
      } else {
        $$ = new vector<unique_ptr<Expression>>;
      }
      $$->emplace($$->begin(), $1);
    }
    ;
expression:
    function_expression
    {
      $$ = $1;
    }
    |
    expression '+' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::ADD, $1, $3, sql_string, &@$);
    }
    | expression '-' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::SUB, $1, $3, sql_string, &@$);
    }
    | expression '*' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::MUL, $1, $3, sql_string, &@$);
    }
    | expression '/' expression {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::DIV, $1, $3, sql_string, &@$);
    }
    | LBRACE expression RBRACE {
      $$ = $2;
      $$->set_name(token_name(sql_string, &@$));
    }
    | '-' expression %prec UMINUS {
      $$ = create_arithmetic_expression(ArithmeticExpr::Type::NEGATIVE, $2, nullptr, sql_string, &@$);
    }
    | '*' {
      $$ = new StarExpr();
    }
    | value {
      $$ = new ValueExpr(*$1);
      $$->set_name(token_name(sql_string, &@$));
      delete $1;
    }
    | rel_attr {
      RelAttrSqlNode *node = $1;
      $$ = new UnboundFieldExpr(node->relation_name, node->attribute_name);
      $$->set_name(token_name(sql_string, &@$));
      delete $1;
    }
    | aggregate_expression {
      $$ = $1;
    }
    | LBRACE select_stmt RBRACE {
      // 子查询表达式：将select子句作为一个表达式节点
      if ($2 == nullptr || $2->flag != SCF_SELECT) {
        $$ = nullptr;
      } else {
        // 使用表达式来持有子查询
        $$ = new SubqueryExpr(std::unique_ptr<ParsedSqlNode>($2));
        $$->set_name(token_name(sql_string, &@$));
      }
    }
    ;


select_item:
    expression
    {
      $$ = $1;
    }
    | expression ID
    {
      $1->set_alias($2);
      $$ = $1;
    }
    | expression AS ID
    {
      $1->set_alias($3);
      $$ = $1;
    }
    ;

// scalar function calls
function_expression:
      LENGTH_F LBRACE expression RBRACE
      {
        $$ = new ScalarFunctionExpr(ScalarFunctionExpr::FuncType::LENGTH, $3);
        $$->set_name(token_name(sql_string, &@$));
      }
    | ROUND_F LBRACE expression RBRACE
      {
        $$ = new ScalarFunctionExpr(ScalarFunctionExpr::FuncType::ROUND, $3);
        $$->set_name(token_name(sql_string, &@$));
      }
    | ROUND_F LBRACE expression COMMA expression RBRACE
      {
        // 支持 ROUND(expr, scale) 第二个参数（保留小数位数）
        $$ = new ScalarFunctionExpr(ScalarFunctionExpr::FuncType::ROUND, $3, $5);
        $$->set_name(token_name(sql_string, &@$));
      }
    | DATE_FORMAT_F LBRACE expression RBRACE
      {
        $$ = new ScalarFunctionExpr(ScalarFunctionExpr::FuncType::DATE_FORMAT, $3);
        $$->set_name(token_name(sql_string, &@$));
      }
    | DATE_FORMAT_F LBRACE expression COMMA expression RBRACE
      {
        // DATE_FORMAT(date_expr, format_expr)
        // 保留第二个参数(format)传入表达式，供执行阶段使用
        $$ = new ScalarFunctionExpr(ScalarFunctionExpr::FuncType::DATE_FORMAT, $3, $5);
        $$->set_name(token_name(sql_string, &@$));
      }
    | L2_DISTANCE_F LBRACE expression COMMA expression RBRACE
      {
        $$ = new ScalarFunctionExpr(ScalarFunctionExpr::FuncType::L2_DISTANCE, $3, $5);
        $$->set_name(token_name(sql_string, &@$));
      }
    | COSINE_DISTANCE_F LBRACE expression COMMA expression RBRACE
      {
        $$ = new ScalarFunctionExpr(ScalarFunctionExpr::FuncType::COSINE_DISTANCE, $3, $5);
        $$->set_name(token_name(sql_string, &@$));
      }
    | INNER_PRODUCT_F LBRACE expression COMMA expression RBRACE
      {
        $$ = new ScalarFunctionExpr(ScalarFunctionExpr::FuncType::INNER_PRODUCT, $3, $5);
        $$->set_name(token_name(sql_string, &@$));
      }
    ;

aggregate_expression:
    /* 允许空参数聚合在语法阶段通过，但记录为0个参数，
       在语义阶段（binder）统一返回 INVALID_ARGUMENT，从而对外呈现为 FAILURE。 */
    ID LBRACE RBRACE {
      UnboundAggregateExpr *agg = create_aggregate_expression($1, new ValueExpr(Value()), sql_string, &@$);
      agg->set_arg_count(0);
      $$ = agg;
    }
    | ID LBRACE expression RBRACE {
      $$ = create_aggregate_expression($1, $3, sql_string, &@$);
    }
    | ID LBRACE expression COMMA expression_list RBRACE {
      // 语法上允许多参数，以便进入语义阶段做更友好的错误（避免语法错误）
      // 仅保留第一个参数表达式，其余仅用于记录实参个数
      UnboundAggregateExpr *agg = create_aggregate_expression($1, $3, sql_string, &@$);
      if ($5 != nullptr) {
        agg->set_arg_count(1 + (int)$5->size());
        delete $5;
      } else {
        agg->set_arg_count(2); // 至少 2 个
      }
      $$ = agg;
    }
    ;

rel_attr:
    ID {
      $$ = new RelAttrSqlNode;
      $$->attribute_name = $1;
    }
    | ID DOT ID {
      $$ = new RelAttrSqlNode;
      $$->relation_name  = $1;
      $$->attribute_name = $3;
    }
    | ID DOT '*' {
      $$ = new RelAttrSqlNode;
      $$->relation_name  = $1;
      $$->attribute_name = "*";
    }
    ;

relation:
    ID {
      $$ = new RelationSqlNode{ $1, "" };
    }
    | ID ID {
      $$ = new RelationSqlNode{ $1, $2 };
    }
    | ID AS ID {
      $$ = new RelationSqlNode{ $1, $3 };
    }
    ;

rel_list:
    relation {
      $$ = new vector<RelationSqlNode>();
      $$->push_back(*$1);
      delete $1;
    }
    | relation COMMA rel_list {
      if ($3 != nullptr) {
        $$ = $3;
      } else {
        $$ = new vector<RelationSqlNode>;
      }
      $$->emplace($$->begin(), *$1);
      delete $1;
    }
    | join_seq {
      $$ = $1;
    }
    ;

// 支持 INNER JOIN / JOIN 语法，ON 后接布尔表达式（AND/OR）
join_seq:
    relation {
      $$ = new vector<RelationSqlNode>();
      $$->push_back(*$1);
      delete $1;
      // 清理上次残留
      g_join_on_expr = nullptr;
    }
    | join_seq INNER JOIN relation ON bool_term {
      $$ = $1;
      $$->push_back(*$4);
      delete $4;
      // 累积 ON 条件（AND 连接）
      if (g_join_on_expr == nullptr) {
        g_join_on_expr = $6;
      } else {
        vector<unique_ptr<Expression>> children;
        children.emplace_back(g_join_on_expr);
        children.emplace_back($6);
        g_join_on_expr = new ConjunctionExpr(ConjunctionExpr::Type::AND, children);
      }
    }
    | join_seq JOIN relation ON bool_term {
      $$ = $1;
      $$->push_back(*$3);
      delete $3;
      if (g_join_on_expr == nullptr) {
        g_join_on_expr = $5;
      } else {
        vector<unique_ptr<Expression>> children;
        children.emplace_back(g_join_on_expr);
        children.emplace_back($5);
        g_join_on_expr = new ConjunctionExpr(ConjunctionExpr::Type::AND, children);
      }
    }
    ;

where:
    /* empty */
    {
      $$ = nullptr;
    }
    | WHERE condition_list {
      $$ = $2;  
    }
    ;
condition_list:
    /* empty */
    {
      $$ = nullptr;
    }
    | condition {
      $$ = new vector<ConditionSqlNode>;
      $$->emplace_back(std::move(*$1));
      delete $1;
    }
    | condition AND condition_list {
      $$ = $3;
      $$->emplace_back(std::move(*$1));
      delete $1;
    }
    ;
condition:
    expression comp_op expression
    {
      $$ = new ConditionSqlNode;
      $$->left_expr.reset($1);
      $$->right_expr.reset($3);
      $$->comp = $2;
      $$->left_is_attr = -1;  // 标记使用表达式
      $$->right_is_attr = -1; // 标记使用表达式
    }
    | expression IS NULL_T
    {
      $$ = new ConditionSqlNode;
      $$->left_expr.reset($1);
      // 右侧使用一个 NULL 常量占位，便于后续统一处理
      Value __v; __v.set_null();
      $$->right_expr.reset(new ValueExpr(__v));
      $$->comp = IS_NULL;
      $$->left_is_attr  = -1;
      $$->right_is_attr = -1;
    }
    | expression IS NOT NULL_T
    {
      $$ = new ConditionSqlNode;
      $$->left_expr.reset($1);
      Value __v; __v.set_null();
      $$->right_expr.reset(new ValueExpr(__v));
      $$->comp = IS_NOT_NULL;
      $$->left_is_attr  = -1;
      $$->right_is_attr = -1;
    }
    | expression IN LBRACE select_stmt RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_expr.reset($1);
      // 将子查询封装为表达式
      $$->right_expr.reset(new SubqueryExpr(std::unique_ptr<ParsedSqlNode>($4)));
      $$->comp = IN_OP;
      $$->left_is_attr = -1;
      $$->right_is_attr = -1;
    }
    | expression NOT IN LBRACE select_stmt RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_expr.reset($1);
      $$->right_expr.reset(new SubqueryExpr(std::unique_ptr<ParsedSqlNode>($5)));
      $$->comp = NOT_IN_OP;
      $$->left_is_attr = -1;
      $$->right_is_attr = -1;
    }
    | expression IN LBRACE value_list RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_expr.reset($1);
      std::vector<Value> vals;
      if ($4 != nullptr) { vals = std::move(*$4); delete $4; }
      AttrType rt = AttrType::UNDEFINED; int rlen = -1;
      if (!vals.empty()) { rt = vals[0].attr_type(); rlen = vals[0].length(); }
      $$->right_expr.reset(new SubqueryExpr(vals, rt, rlen));
      $$->comp = IN_OP;
      $$->left_is_attr = -1; $$->right_is_attr = -1;
    }
    | expression NOT IN LBRACE value_list RBRACE
    {
      $$ = new ConditionSqlNode;
      $$->left_expr.reset($1);
      std::vector<Value> vals;
      if ($5 != nullptr) { vals = std::move(*$5); delete $5; }
      AttrType rt = AttrType::UNDEFINED; int rlen = -1;
      if (!vals.empty()) { rt = vals[0].attr_type(); rlen = vals[0].length(); }
      $$->right_expr.reset(new SubqueryExpr(vals, rt, rlen));
      $$->comp = NOT_IN_OP;
      $$->left_is_attr = -1; $$->right_is_attr = -1;
    }
    ;

// WHERE 的布尔表达式解析（支持 AND/OR 与括号）
where_bool:
    /* empty */
    {
      $$ = nullptr;
    }
    | WHERE bool_term
    {
      $$ = $2;
    }
    ;

bool_term:
    bool_term OR bool_factor
    {
      vector<unique_ptr<Expression>> children;
      children.emplace_back($1);
      children.emplace_back($3);
      $$ = new ConjunctionExpr(ConjunctionExpr::Type::OR, children);
    }
    | bool_term AND bool_factor
    {
      vector<unique_ptr<Expression>> children;
      children.emplace_back($1);
      children.emplace_back($3);
      $$ = new ConjunctionExpr(ConjunctionExpr::Type::AND, children);
    }
    | bool_factor
    {
      $$ = $1;
    }
    ;

bool_factor:
    LBRACE bool_term RBRACE { $$ = $2; }
    | condition_expr          { $$ = $1; }
    ;

condition_expr:
    expression comp_op expression
    {
      $$ = new ComparisonExpr($2, std::unique_ptr<Expression>($1), std::unique_ptr<Expression>($3));
    }
    | expression IS NULL_T
    {
      Value __v; __v.set_null();
      $$ = new ComparisonExpr(IS_NULL, std::unique_ptr<Expression>($1), std::make_unique<ValueExpr>(__v));
    }
    | expression IS NOT NULL_T
    {
      Value __v; __v.set_null();
      $$ = new ComparisonExpr(IS_NOT_NULL, std::unique_ptr<Expression>($1), std::make_unique<ValueExpr>(__v));
    }
    | expression IN LBRACE select_stmt RBRACE
    {
      $$ = new InExpr(std::unique_ptr<Expression>($1), std::make_unique<SubqueryExpr>(std::unique_ptr<ParsedSqlNode>($4)), false);
    }
    | expression NOT IN LBRACE select_stmt RBRACE
    {
      $$ = new InExpr(std::unique_ptr<Expression>($1), std::make_unique<SubqueryExpr>(std::unique_ptr<ParsedSqlNode>($5)), true);
    }
    | expression IN LBRACE value_list RBRACE
    {
      std::vector<Value> vals; if ($4) { vals = std::move(*$4); delete $4; }
      AttrType rt = AttrType::UNDEFINED; int rlen = -1; if (!vals.empty()) { rt = vals[0].attr_type(); rlen = vals[0].length(); }
      $$ = new InExpr(std::unique_ptr<Expression>($1), std::make_unique<SubqueryExpr>(vals, rt, rlen), false);
    }
    | expression NOT IN LBRACE value_list RBRACE
    {
      std::vector<Value> vals; if ($5) { vals = std::move(*$5); delete $5; }
      AttrType rt = AttrType::UNDEFINED; int rlen = -1; if (!vals.empty()) { rt = vals[0].attr_type(); rlen = vals[0].length(); }
      $$ = new InExpr(std::unique_ptr<Expression>($1), std::make_unique<SubqueryExpr>(vals, rt, rlen), true);
    }
    ;

comp_op:
      EQ { $$ = EQUAL_TO; }
    | LT { $$ = LESS_THAN; }
    | GT { $$ = GREAT_THAN; }
    | LE { $$ = LESS_EQUAL; }
    | GE { $$ = GREAT_EQUAL; }
    | NE { $$ = NOT_EQUAL; }
    | LIKE { $$ = LIKE_OP; }
    | NOT LIKE { $$ = NOT_LIKE; }
    ;

// your code here
group_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | GROUP BY expression_list
    {
      // group by 的表达式范围与select查询值的表达式范围是不同的，比如group by不支持 *
      // 但是这里没有处理。
      $$ = $3;
    }
    ;

having:
    /* empty */
    {
      $$ = nullptr;
    }
    | HAVING condition_list
    {
      $$ = $2;
    }
    ;

order_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | ORDER BY order_by_condition_list
    {
      $$ = $3;
    }
    ;

order_by_condition_list:
    order_by_condition
    {
      $$ = new vector<OrderBySqlNode>();
      $$->emplace_back(std::move(*$1));
      delete $1;
    }
    | order_by_condition COMMA order_by_condition_list
    {
      $$ = $3;
      $$->emplace($$->begin(), std::move(*$1));
      delete $1;
    }
    ;

order_by_condition:
    expression
    {
      $$ = new OrderBySqlNode();
      $$->expression.reset($1);
      $$->asc = true; // 默认升序
    }
    | expression ASC
    {
      $$ = new OrderBySqlNode();
      $$->expression.reset($1);
      $$->asc = true;
    }
    | expression DESC
    {
      $$ = new OrderBySqlNode();
      $$->expression.reset($1);
      $$->asc = false;
    }
    ;
load_data_stmt:
    LOAD DATA INFILE SSS INTO TABLE ID fields_terminated_by enclosed_by
    {
      char *tmp_file_name = common::substr($4, 1, strlen($4) - 2);
      
      $$ = new ParsedSqlNode(SCF_LOAD_DATA);
      $$->load_data.relation_name = $7;
      $$->load_data.file_name = tmp_file_name;
      if ($8 != nullptr) {
        char *tmp = common::substr($8,1,strlen($8)-2);
        $$->load_data.terminated = $8;
        free(tmp);
      }
      if ($9 != nullptr) {
        char *tmp = common::substr($9,1,strlen($9)-2);
        $$->load_data.enclosed = $9;
        free(tmp);
      }
      free(tmp_file_name);
    }
    ;

fields_terminated_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | FIELDS TERMINATED BY SSS
    {
      $$ = $4;
    };

enclosed_by:
    /* empty */
    {
      $$ = nullptr;
    }
    | ENCLOSED BY SSS
    {
      $$ = $3;
    };

explain_stmt:
    EXPLAIN command_wrapper
    {
      $$ = new ParsedSqlNode(SCF_EXPLAIN);
      $$->explain.sql_node = unique_ptr<ParsedSqlNode>($2);
    }
    ;

set_variable_stmt:
    SET ID EQ value
    {
      $$ = new ParsedSqlNode(SCF_SET_VARIABLE);
      $$->set_variable.name  = $2;
      $$->set_variable.value = *$4;
      delete $4;
    }
    ;

opt_semicolon: /*empty*/
    | SEMICOLON
    ;
%%
//_____________________________________________________________________
extern void scan_string(const char *str, yyscan_t scanner);

int sql_parse(const char *s, ParsedSqlResult *sql_result) {
  yyscan_t scanner;
  std::vector<char *> allocated_strings;
  yylex_init_extra(static_cast<void*>(&allocated_strings), &scanner);

  // 预处理：将简单的 INNER JOIN/JOIN 语法改写为逗号连接 + WHERE 条件，
  // 以复用现有的语法（relation_list 与 condition_list）。
  // 仅支持基础形式：
  //   SELECT ... FROM t1 [INNER] JOIN t2 ON cond [JOIN t3 ON cond] ... [WHERE ...] [GROUP BY ...] [ORDER BY ...]
  // 改写为：
  //   SELECT ... FROM t1, t2, t3 WHERE cond AND cond ... [GROUP BY ...] [ORDER BY ...]
  std::string orig_sql = s ? std::string(s) : std::string();
  std::string lower    = orig_sql;
  for (auto &ch : lower) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));

  auto find_ci = [&](const std::string &pat, size_t pos) -> size_t {
    std::string p = pat;
    for (auto &c : p) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return lower.find(p, pos);
  };

  auto trim = [](const std::string &str) -> std::string {
    size_t i = 0, j = str.size();
    while (i < j && isspace(static_cast<unsigned char>(str[i]))) i++;
    while (j > i && isspace(static_cast<unsigned char>(str[j - 1]))) j--;
    return str.substr(i, j - i);
  };

  std::string rewritten = orig_sql; // 默认不改写
  size_t      from_pos  = find_ci(" from ", 0);
  size_t      join_pos  = std::string::npos;
  if (from_pos != std::string::npos) {
    // 找到 FROM 子句的边界（在 WHERE/GROUP/ORDER 之前，或语句结尾）
    size_t where_pos = find_ci(" where ", from_pos + 6);
    size_t group_pos = find_ci(" group ", from_pos + 6);
    size_t order_pos = find_ci(" order ", from_pos + 6);
    size_t after_from_end = orig_sql.size();
    if (where_pos != std::string::npos) after_from_end = std::min(after_from_end, where_pos);
    if (group_pos != std::string::npos) after_from_end = std::min(after_from_end, group_pos);
    if (order_pos != std::string::npos) after_from_end = std::min(after_from_end, order_pos);

    // FROM 子句中是否存在 JOIN。优先匹配最早出现的 " inner join " 或 " join "
    size_t first_inner = find_ci(" inner join ", from_pos + 6);
    size_t first_join  = find_ci(" join ", from_pos + 6);
    size_t earliest    = std::min(first_inner == std::string::npos ? SIZE_MAX : first_inner,
                                  first_join  == std::string::npos ? SIZE_MAX : first_join);
    join_pos = (earliest == SIZE_MAX) ? std::string::npos : earliest;

    if (join_pos != std::string::npos && join_pos < after_from_end) {
      // 解析基础表名（FROM 之后、JOIN 之前的部分）
      size_t      base_start = from_pos + 6; // 跳过 " from "
      std::string base_rel   = trim(orig_sql.substr(base_start, join_pos - base_start));
      std::vector<std::string> relations;
      std::vector<std::string> join_conds;
      if (!base_rel.empty()) relations.push_back(base_rel);

      // 迭代 FROM 子句中的所有 JOIN ... ON ...
      size_t p = base_start + base_rel.size();
      while (p < after_from_end) {
        size_t inner_join_pos = find_ci(" inner join ", p);
        size_t just_join_pos  = find_ci(" join ", p);
        size_t this_join_pos  = std::min(inner_join_pos == std::string::npos ? SIZE_MAX : inner_join_pos,
                                         just_join_pos  == std::string::npos ? SIZE_MAX : just_join_pos);
        if (this_join_pos == SIZE_MAX || this_join_pos >= after_from_end) {
          break;
        }

        // 跳过关键字
        size_t after_kw = this_join_pos;
        if (inner_join_pos != std::string::npos && inner_join_pos == this_join_pos) {
          after_kw += std::string(" inner join ").size();
        } else {
          after_kw += std::string(" join ").size();
        }

        // 读取下一个关系名（直到 " on "）
        size_t on_pos = find_ci(" on ", after_kw);
        if (on_pos == std::string::npos || on_pos >= after_from_end) {
          break; // 结构异常，放弃改写
        }
        std::string rel = trim(orig_sql.substr(after_kw, on_pos - after_kw));
        if (!rel.empty()) relations.push_back(rel);

        // 读取 ON 条件（直到下一个 JOIN 或 FROM 子句结束）
        size_t next_inner = find_ci(" inner join ", on_pos + 4);
        size_t next_join  = find_ci(" join ", on_pos + 4);
        size_t next_pos   = std::min(next_inner == std::string::npos ? SIZE_MAX : next_inner,
                                     next_join  == std::string::npos ? SIZE_MAX : next_join);
        if (next_pos == SIZE_MAX || next_pos > after_from_end) {
          next_pos = after_from_end;
        }
        std::string cond = trim(orig_sql.substr(on_pos + 4, next_pos - (on_pos + 4)));
        if (!cond.empty()) join_conds.push_back(std::string("(") + cond + ")");

        p = next_pos;
      }

      // 重新拼接 SQL：保留 FROM 之前的头部，FROM 后拼接逗号分隔的关系；
      // WHERE 合并原有 WHERE 与各个 ON 条件（用 AND 连接）。
      std::string head = orig_sql.substr(0, from_pos + 6);
      std::string tail; // 保留原 SQL 中 WHERE/ GROUP / ORDER 及其之后的部分
      std::string orig_where;
      if (where_pos != std::string::npos) {
        size_t where_end = orig_sql.size();
        if (group_pos != std::string::npos) where_end = std::min(where_end, group_pos);
        if (order_pos != std::string::npos) where_end = std::min(where_end, order_pos);
        orig_where = trim(orig_sql.substr(where_pos + 7, where_end - (where_pos + 7)));
        // 去掉末尾分号，避免拼接到 WHERE 内部
        if (!orig_where.empty() && orig_where.back() == ';') {
          orig_where.pop_back();
          // 可能去除分号后留有末尾空白，再做一次 trim
          orig_where = trim(orig_where);
        }
        tail       = orig_sql.substr(where_end);
      } else {
        tail = orig_sql.substr(after_from_end);
      }

      std::string rels;
      for (size_t i = 0; i < relations.size(); ++i) {
        if (i > 0) rels += ", ";
        rels += relations[i];
      }

      std::string new_where = orig_where; // 沿用原有 WHERE 条件
      for (const auto &c : join_conds) {
        if (!new_where.empty()) new_where += " AND ";
        if (!c.empty() && c.front() == '(' && c.back() == ')') {
          new_where += c.substr(1, c.size() - 2);
        } else {
          new_where += c;
        }
      }

      rewritten = head + rels;
      if (!new_where.empty()) {
        rewritten += " WHERE ";
        rewritten += new_where;
      }
      rewritten += tail;
    }
  }

  scan_string(rewritten.c_str(), scanner);
  int result = yyparse(rewritten.c_str(), sql_result, scanner);

  for (char *ptr : allocated_strings) {
    free(ptr);
  }
  allocated_strings.clear();

  yylex_destroy(scanner);
  return result;
}
