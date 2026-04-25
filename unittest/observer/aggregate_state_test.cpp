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

#include "common/sys/rc.h"
#include "common/value.h"
#include "sql/expr/aggregate_state.h"
#include "storage/common/column.h"

using namespace std;

namespace {

Column make_null_column(int rows)
{
  Value null_value;
  null_value.set_null();

  Column column;
  column.init(null_value, rows);
  return column;
}

Value finalize_single_value(void *state, AggregateExpr::Type aggr_type, AttrType attr_type)
{
  Column result(attr_type, sizeof(int), 1);
  RC     rc = finialize_aggregate_state(state, aggr_type, attr_type, result);
  EXPECT_EQ(RC::SUCCESS, rc);
  return result.get_value(0);
}

}  // namespace

TEST(AggregateStateTest, SumColumnIgnoresNullValues)
{
  void *state = create_aggregate_state(AggregateExpr::Type::SUM, AttrType::INTS);
  ASSERT_NE(nullptr, state);

  Column nulls = make_null_column(3);
  ASSERT_EQ(RC::SUCCESS,
      aggregate_state_update_by_column(state, AggregateExpr::Type::SUM, AttrType::INTS, nulls));

  Value result = finalize_single_value(state, AggregateExpr::Type::SUM, AttrType::INTS);
  EXPECT_TRUE(result.is_null());

  free(state);
}

TEST(AggregateStateTest, AvgColumnIgnoresNullValues)
{
  void *state = create_aggregate_state(AggregateExpr::Type::AVG, AttrType::INTS);
  ASSERT_NE(nullptr, state);

  Column nulls = make_null_column(3);
  ASSERT_EQ(RC::SUCCESS,
      aggregate_state_update_by_column(state, AggregateExpr::Type::AVG, AttrType::INTS, nulls));

  Value result = finalize_single_value(state, AggregateExpr::Type::AVG, AttrType::INTS);
  EXPECT_TRUE(result.is_null());

  free(state);
}

TEST(AggregateStateTest, CountColumnIgnoresNullValues)
{
  void *state = create_aggregate_state(AggregateExpr::Type::COUNT, AttrType::INTS);
  ASSERT_NE(nullptr, state);

  Column nulls = make_null_column(3);
  ASSERT_EQ(RC::SUCCESS,
      aggregate_state_update_by_column(state, AggregateExpr::Type::COUNT, AttrType::INTS, nulls));

  Value result = finalize_single_value(state, AggregateExpr::Type::COUNT, AttrType::INTS);
  EXPECT_EQ(0, result.get_int());

  free(state);
}

TEST(AggregateStateTest, SumColumnKeepsNonNullValues)
{
  void *state = create_aggregate_state(AggregateExpr::Type::SUM, AttrType::INTS);
  ASSERT_NE(nullptr, state);

  Column values(AttrType::INTS, sizeof(int), 3);
  int    one   = 1;
  int    two   = 2;
  int    three = 3;
  ASSERT_EQ(RC::SUCCESS, values.append_one(reinterpret_cast<char *>(&one)));
  ASSERT_EQ(RC::SUCCESS, values.append_one(reinterpret_cast<char *>(&two)));
  ASSERT_EQ(RC::SUCCESS, values.append_one(reinterpret_cast<char *>(&three)));
  ASSERT_EQ(RC::SUCCESS,
      aggregate_state_update_by_column(state, AggregateExpr::Type::SUM, AttrType::INTS, values));

  Value result = finalize_single_value(state, AggregateExpr::Type::SUM, AttrType::INTS);
  EXPECT_EQ(6, result.get_int());

  free(state);
}
