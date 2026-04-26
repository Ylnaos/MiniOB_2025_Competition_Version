/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "common/lang/string.h"
#include "common/lang/memory.h"
#include "common/sys/rc.h"
#include "sql/parser/parse_defs.h"

class View
{
public:
  View()          = default;
  virtual ~View() = default;

  RC init(const char *name, const char *select_sql)
  {
    if (nullptr == name || nullptr == select_sql) return RC::INVALID_ARGUMENT;
    name_        = name;
    select_sql_  = select_sql;
    return RC::SUCCESS;
  }

  RC init(const char *name, const char *select_sql, const vector<string> &view_fields)
  {
    if (nullptr == name || nullptr == select_sql) return RC::INVALID_ARGUMENT;
    name_        = name;
    select_sql_  = select_sql;
    view_fields_ = view_fields;
    return RC::SUCCESS;
  }

  const char *name() const { return name_.c_str(); }
  const char *select_sql() const { return select_sql_.c_str(); }
  const vector<string> &view_fields() const { return view_fields_; }
  const ParsedSqlNode *select_node() const { return select_node_.get(); }
  void set_select_node(unique_ptr<ParsedSqlNode> select_node) { select_node_ = std::move(select_node); }

private:
  string name_;
  string select_sql_;
  vector<string> view_fields_;  ///< 视图定义的列名（可能为空）
  unique_ptr<ParsedSqlNode> select_node_;
};
