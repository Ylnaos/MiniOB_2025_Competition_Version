/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "common/lang/string.h"

#include <gtest/gtest.h>

TEST(StringTest, StripHandlesEmptyString)
{
  std::string value;
  common::strip(value);
  ASSERT_TRUE(value.empty());
}

TEST(StringTest, StripHandlesAllWhitespaceString)
{
  std::string value = " \t\r\n ";
  common::strip(value);
  ASSERT_TRUE(value.empty());
}

TEST(StringTest, StripTrimsBothEnds)
{
  std::string value = " \t123  ";
  common::strip(value);
  ASSERT_EQ("123", value);
}
