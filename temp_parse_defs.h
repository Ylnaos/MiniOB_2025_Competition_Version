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

/**
 * @defgroup SQLParser SQL Parser
 */

/**
 * @brief 鎻忚堪涓€涓睘鎬? * @ingroup SQLParser
 * @details 灞炴€э紝鎴栬€呰瀛楁(column, field)
 * Rel -> Relation
 * Attr -> Attribute
 */
struct RelAttrSqlNode
{
  string relation_name;   ///< relation name (may be NULL) 琛ㄥ悕
  string attribute_name;  ///< attribute name              灞炴€у悕
};

/**
 * @brief FROM 瀛愬彞涓殑琛ㄥ強鍏跺彲閫夊埆鍚? */
struct RelationSqlNode
{
  string relation_name;  ///< 鐪熷疄琛ㄥ悕
  string alias;          ///< 鍙€夎〃鍒悕锛屾湭鎸囧畾鍒欎负绌?};


/**
 * @brief 鎻忚堪姣旇緝杩愮畻绗? * @ingroup SQLParser
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
  IN_OP,        ///< "IN (subquery)"
  NOT_IN_OP,    ///< "NOT IN (subquery)"
  IS_NULL,      ///< "IS NULL"
  IS_NOT_NULL,  ///< "IS NOT NULL"
  NO_OP
};

/**
 * @brief 琛ㄧず涓€涓潯浠舵瘮杈? * @ingroup SQLParser
 * @details 鏉′欢姣旇緝灏辨槸SQL鏌ヨ涓殑 where a>b 杩欑銆? * 涓€涓潯浠舵瘮杈冩槸鏈変袱閮ㄥ垎缁勬垚鐨勶紝绉颁负宸﹁竟鍜屽彸杈广€? * 宸﹁竟鍜屽彸杈圭悊璁轰笂閮藉彲浠ユ槸浠绘剰鐨勬暟鎹紝姣斿鏄瓧娈碉紙灞炴€э紝鍒楋級锛屼篃鍙互鏄暟鍊煎父閲忋€? * 杩欎釜缁撴瀯涓褰曠殑浠呬粎鏀寔瀛楁鍜屽€笺€? */
struct ConditionSqlNode
{
  int left_is_attr;              ///< TRUE if left-hand side is an attribute
                                 ///< 1鏃讹紝鎿嶄綔绗﹀乏杈规槸灞炴€у悕锛?鏃讹紝鏄睘鎬у€?  Value          left_value;     ///< left-hand side value if left_is_attr = FALSE
  RelAttrSqlNode left_attr;      ///< left-hand side attribute
  CompOp         comp;           ///< comparison operator
  int            right_is_attr;  ///< TRUE if right-hand side is an attribute
                                 ///< 1鏃讹紝鎿嶄綔绗﹀彸杈规槸灞炴€у悕锛?鏃讹紝鏄睘鎬у€?  RelAttrSqlNode right_attr;     ///< right-hand side attribute if right_is_attr = TRUE 鍙宠竟鐨勫睘鎬?  Value          right_value;    ///< right-hand side value if right_is_attr = FALSE

  // 鏂板锛氭敮鎸佽〃杈惧紡
  unique_ptr<Expression> left_expr;   ///< 宸﹁竟鐨勮〃杈惧紡
  unique_ptr<Expression> right_expr;  ///< 鍙宠竟鐨勮〃杈惧紡

  // 娣诲姞榛樿鏋勯€犲嚱鏁?  ConditionSqlNode() = default;

  // 娣诲姞鏄惧紡鏋愭瀯鍑芥暟澹版槑锛堝畾涔夊湪 parse_defs.cpp 涓級
  ~ConditionSqlNode();

  // 娣诲姞绉诲姩鏋勯€犲嚱鏁帮紙澹版槑锛屽畾涔夊湪 parse_defs.cpp 涓級
  ConditionSqlNode(ConditionSqlNode&& other) noexcept;

  // 娣诲姞绉诲姩璧嬪€艰繍绠楃锛堝０鏄庯紝瀹氫箟鍦?parse_defs.cpp 涓級
  ConditionSqlNode& operator=(ConditionSqlNode&& other) noexcept;

  // 鍒犻櫎鎷疯礉鏋勯€犲嚱鏁板拰鎷疯礉璧嬪€艰繍绠楃
  ConditionSqlNode(const ConditionSqlNode&) = delete;
  ConditionSqlNode& operator=(const ConditionSqlNode&) = delete;
};

/**
 * @brief 琛ㄧず涓€涓?ORDER BY 鏉＄洰锛氳〃杈惧紡 + 鍗囬檷搴? */
struct OrderBySqlNode
{
  unique_ptr<Expression> expression;  ///< 鎺掑簭鐢ㄧ殑琛ㄨ揪寮忥紙褰撳墠浠呴渶鏀寔瀛楁锛?  bool                    asc = true; ///< 鏄惁鍗囧簭锛岄粯璁ゅ崌搴?};

/**
 * @brief 鎻忚堪涓€涓猻elect璇彞
 * @ingroup SQLParser
 * @details 涓€涓甯哥殑select璇彞鎻忚堪璧锋潵姣旇繖涓澶嶆潅寰堝锛岃繖閲屽仛浜嗙畝鍖栥€? * 涓€涓猻elect璇彞鐢变笁閮ㄥ垎缁勬垚锛屽垎鍒槸select, from, where銆? * select閮ㄥ垎琛ㄧず瑕佹煡璇㈢殑瀛楁锛宖rom閮ㄥ垎琛ㄧず瑕佹煡璇㈢殑琛紝where閮ㄥ垎琛ㄧず鏌ヨ鐨勬潯浠躲€? * 姣斿 from 涓彲浠ユ槸澶氫釜琛紝涔熷彲浠ユ槸鍙︿竴涓煡璇㈣鍙ワ紝杩欓噷浠呬粎鏀寔琛紝涔熷氨鏄?relations銆? * where 鏉′欢 conditions锛岃繖閲岃〃绀轰娇鐢ˋND涓茶仈璧锋潵澶氫釜鏉′欢銆傛甯哥殑SQL璇彞浼氭湁OR锛孨OT绛夛紝
 * 鐢氳嚦鍙互鍖呭惈澶嶆潅鐨勮〃杈惧紡銆? */

struct SelectSqlNode
{
  vector<unique_ptr<Expression>> expressions;  ///< 鏌ヨ鐨勮〃杈惧紡
  vector<RelationSqlNode>                 relations;    ///< 鏌ヨ鐨勮〃
  vector<ConditionSqlNode>       conditions;   ///< 鏌ヨ鏉′欢锛屼娇鐢ˋND涓茶仈璧锋潵澶氫釜鏉′欢
  vector<unique_ptr<Expression>> group_by;     ///< group by clause
  vector<OrderBySqlNode>         order_by;     ///< order by clause

  SelectSqlNode() = default;
  ~SelectSqlNode();  // 瀹氫箟鍦?parse_defs.cpp 涓?};

/**
 * @brief 绠楁湳琛ㄨ揪寮忚绠楃殑璇硶鏍? * @ingroup SQLParser
 */
struct CalcSqlNode
{
  vector<unique_ptr<Expression>> expressions;  ///< calc clause

  CalcSqlNode() = default;
  ~CalcSqlNode();  // 瀹氫箟鍦?parse_defs.cpp 涓?};

/**
 * @brief 鎻忚堪涓€涓猧nsert璇彞
 * @ingroup SQLParser
 * @details 浜嶴elects绫讳技锛屼篃鍋氫簡寰堝绠€鍖? */
struct InsertSqlNode
{
  string                         relation_name;  ///< Relation to insert into
  vector<vector<Value>>          rows;           ///< 澶氳鎻掑叆鐨勫€硷紝姣忎釜瀛?vector 琛ㄧず涓€琛?};

/**
 * @brief 鎻忚堪涓€涓猟elete璇彞
 * @ingroup SQLParser
 */
struct DeleteSqlNode
{
  string                   relation_name;  ///< Relation to delete from
  vector<ConditionSqlNode> conditions;
};

/**
 * @brief 鎻忚堪涓€涓猽pdate璇彞
 * @ingroup SQLParser
 */
struct UpdateSqlNode
{
  string                   relation_name;    ///< Relation to update
  vector<string>           attribute_names;  ///< 鏇存柊鐨勫瓧娈靛垪琛紝鏀寔澶氫釜瀛楁
  vector<Value>            values;           ///< 鏇存柊鐨勫€煎垪琛紝涓庡瓧娈典竴涓€瀵瑰簲
  vector<Expression *>     value_expressions;///< 鏇存柊鐨勮〃杈惧紡鍒楄〃锛屼笌瀛楁涓€涓€瀵瑰簲
  vector<ConditionSqlNode> conditions;
};

/**
 * @brief 鎻忚堪涓€涓睘鎬? * @ingroup SQLParser
 * @details 灞炴€э紝鎴栬€呰瀛楁(column, field)
 */
struct AttrInfoSqlNode
{
  AttrType type;    ///< Type of attribute
  string   name;    ///< Attribute name
  size_t   length;  ///< Length of attribute
  bool     nullable = false; ///< Whether the attribute allows NULL (default false)
};

/**
 * @brief 鎻忚堪涓€涓猚reate table璇彞
 * @ingroup SQLParser
 * @details 杩欓噷涔熷仛浜嗗緢澶氱畝鍖栥€? */
struct CreateTableSqlNode
{
  string                  relation_name;  ///< Relation name
  vector<AttrInfoSqlNode> attr_infos;     ///< attributes
  vector<string>          primary_keys;   ///< primary keys
  // TODO: integrate to CreateTableOptions
  string storage_format;  ///< storage format
  string storage_engine;  ///< storage engine
};

/**
 * @brief 鎻忚堪涓€涓猟rop table璇彞
 * @ingroup SQLParser
 */
struct DropTableSqlNode
{
  string relation_name;  ///< 瑕佸垹闄ょ殑琛ㄥ悕
};

/**
 * @brief 鎻忚堪涓€涓猘nalyze table璇彞
 * @ingroup SQLParser
 */
struct AnalyzeTableSqlNode
{
  string relation_name;  ///< 瑕佸垎鏋愮殑琛ㄥ悕
};

/**
 * @brief 鎻忚堪涓€涓猚reate index璇彞
 * @ingroup SQLParser
 * @details 鍒涘缓绱㈠紩鏃讹紝闇€瑕佹寚瀹氱储寮曞悕锛岃〃鍚嶏紝瀛楁鍚嶃€? * 姝ｅ父鐨凷QL璇彞涓紝涓€涓储寮曞彲鑳藉寘鍚簡澶氫釜瀛楁锛岃繖閲屼粎鏀寔涓€涓瓧娈点€? */
struct CreateIndexSqlNode
{
  string              index_name;     ///< Index name
  string              relation_name;  ///< Relation name
  vector<string>      attribute_names;///< Attribute names (support multi-column)
  bool                unique = false; ///< Whether the index is UNIQUE
};

/**
 * @brief 鎻忚堪涓€涓猟rop index璇彞
 * @ingroup SQLParser
 */
struct DropIndexSqlNode
{
  string index_name;     ///< Index name
  string relation_name;  ///< Relation name
};

/**
 * @brief 鎻忚堪涓€涓猟esc table璇彞
 * @ingroup SQLParser
 * @details desc table 鏄煡璇㈣〃缁撴瀯淇℃伅鐨勮鍙? */
struct DescTableSqlNode
{
  string relation_name;
};

/**
 * @brief 鎻忚堪涓€涓猯oad data璇彞
 * @ingroup SQLParser
 * @details 浠庢枃浠跺鍏ユ暟鎹埌琛ㄤ腑銆傛枃浠朵腑鐨勬瘡涓€琛屽氨鏄竴鏉℃暟鎹紝姣忚鐨勬暟鎹被鍨嬨€佸瓧娈典釜鏁伴兘涓庤〃淇濇寔涓€鑷? */
struct LoadDataSqlNode
{
  string relation_name;
  string file_name;
  string terminated = ",";
  string enclosed   = "\"";
};

/**
 * @brief 璁剧疆鍙橀噺鐨勫€? * @ingroup SQLParser
 * @note 褰撳墠杩樻病鏈夋煡璇㈠彉閲? */
struct SetVariableSqlNode
{
  string name;
  Value  value;
};

class ParsedSqlNode;

/**
 * @brief 鎻忚堪涓€涓猠xplain璇彞
 * @ingroup SQLParser
 * @details 浼氬垱寤簅perator鐨勮鍙ワ紝鎵嶈兘鐢╡xplain杈撳嚭鎵ц璁″垝銆? * 涓€涓猚ommand灏辨槸涓€涓鍙ワ紝姣斿select璇彞锛宨nsert璇彞绛夈€? * 鍙兘鏀规垚SqlCommand鏇村悎閫傘€? */
struct ExplainSqlNode
{
  unique_ptr<ParsedSqlNode> sql_node;
};

/**
 * @brief 瑙ｆ瀽SQL璇彞鍑虹幇浜嗛敊璇? * @ingroup SQLParser
 * @details 褰撳墠瑙ｆ瀽鏃跺苟娌℃湁澶勭悊閿欒鐨勮鍙峰拰鍒楀彿
 */
struct ErrorSqlNode
{
  string error_msg;
  int    line;
  int    column;
};

/**
 * @brief 琛ㄧず涓€涓猄QL璇彞鐨勭被鍨? * @ingroup SQLParser
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
  SCF_DROP_TABLE,
  SCF_ANALYZE_TABLE,
  SCF_CREATE_INDEX,
  SCF_DROP_INDEX,
  SCF_SYNC,
  SCF_SHOW_TABLES,
  SCF_DESC_TABLE,
  SCF_BEGIN,  ///< 浜嬪姟寮€濮嬭鍙ワ紝鍙互鍦ㄨ繖閲屾墿灞曞彧璇讳簨鍔?  SCF_COMMIT,
  SCF_CLOG_SYNC,
  SCF_ROLLBACK,
  SCF_LOAD_DATA,
  SCF_HELP,
  SCF_EXIT,
  SCF_EXPLAIN,
  SCF_SET_VARIABLE,  ///< 璁剧疆鍙橀噺
};
/**
 * @brief 琛ㄧず涓€涓猄QL璇彞
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
 * @brief 琛ㄧず璇硶瑙ｆ瀽鍚庣殑鏁版嵁
 * @ingroup SQLParser
 */
class ParsedSqlResult
{
public:
  void add_sql_node(unique_ptr<ParsedSqlNode> sql_node);

  vector<unique_ptr<ParsedSqlNode>> &sql_nodes() { return sql_nodes_; }

private:
  vector<unique_ptr<ParsedSqlNode>> sql_nodes_;  ///< 杩欓噷璁板綍SQL鍛戒护銆傝櫧鐒剁湅璧锋潵鏀寔澶氫釜锛屼絾鏄綋鍓嶄粎澶勭悊涓€涓?};
