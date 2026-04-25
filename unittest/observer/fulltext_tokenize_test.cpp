/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "gtest/gtest.h"

#include "sql/expr/expression.h"

using namespace std;

TEST(FullTextTokenizeTest, FiltersStopWordBeforeValue)
{
  vector<string> tokens;
  ASSERT_EQ(RC::SUCCESS, jieba_tokenize("Unit的状态有哪些可能的值，它们各自代表什么意思？", "jieba", tokens));

  const vector<string> expected {"Unit", "状态", "可能", "值", "代表", "意思"};
  EXPECT_EQ(expected, tokens);
}
