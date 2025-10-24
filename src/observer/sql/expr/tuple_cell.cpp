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
// Created by WangYunlai on 2022/07/05.
//

#include "sql/expr/tuple_cell.h"
#include "common/lang/string.h"

using namespace std;

TupleCellSpec::TupleCellSpec(const char *table_name, const char *field_name, const char *alias)
{
  if (table_name) {
    table_name_ = table_name;
  }
  if (field_name) {
    field_name_ = field_name;
  }
  // 修复：当alias为空字符串时，也自动填充，保持与双参数构造函数一致
  if (alias && alias[0] != '\0') {
    alias_ = alias;
  } else {
    if (table_name_.empty()) {
      alias_ = field_name_;
    } else {
      alias_ = table_name_ + "." + field_name_;
    }
  }
}

TupleCellSpec::TupleCellSpec(const char *alias)
{
  if (alias) {
    string alias_str(alias);
    // 尝试解析 "table.field" 格式
    size_t dot_pos = alias_str.find('.');
    if (dot_pos != string::npos && dot_pos > 0 && dot_pos < alias_str.length() - 1) {
      // 找到点号,且不在首尾,分解为table_name和field_name
      table_name_ = alias_str.substr(0, dot_pos);
      field_name_ = alias_str.substr(dot_pos + 1);
      // 设置alias为原始输入,保持与三参数构造函数行为一致
      alias_ = alias_str;
    } else {
      // 没有点号或格式不对,只设置alias
      alias_ = alias_str;
    }
  }
}

TupleCellSpec::TupleCellSpec(const string &alias)
{
  // 尝试解析 "table.field" 格式
  size_t dot_pos = alias.find('.');
  if (dot_pos != string::npos && dot_pos > 0 && dot_pos < alias.length() - 1) {
    // 找到点号,且不在首尾,分解为table_name和field_name
    table_name_ = alias.substr(0, dot_pos);
    field_name_ = alias.substr(dot_pos + 1);
    // 设置alias为原始输入,保持与三参数构造函数行为一致
    alias_ = alias;
  } else {
    // 没有点号或格式不对,只设置alias
    alias_ = alias;
  }
}
