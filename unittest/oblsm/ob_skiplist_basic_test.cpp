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

#include "common/math/random_generator.h"
#include "oblsm/memtable/ob_skiplist.h"

using namespace oceanbase;

using Key = uint64_t;

struct BasicComparator {
  int operator()(const Key &a, const Key &b) const
  {
    if (a < b) {
      return -1;
    }
    if (a > b) {
      return 1;
    }
    return 0;
  }
};

TEST(skiplist_basic_test, insert_and_contains_random_keys)
{
  common::RandomGenerator rnd;
  constexpr int           N = 2000;
  constexpr int           R = 5000;

  std::set<Key> keys;
  ObSkipList<Key, BasicComparator> list{BasicComparator()};
  for (int i = 0; i < N; i++) {
    Key key = rnd.next() % R;
    if (keys.insert(key).second) {
      list.insert(key);
    }
  }

  for (int i = 0; i < R; i++) {
    ASSERT_EQ(keys.count(i) != 0, list.contains(i));
  }
}

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
