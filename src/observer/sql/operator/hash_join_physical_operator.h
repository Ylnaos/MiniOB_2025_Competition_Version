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

#include "sql/operator/physical_operator.h"
#include "sql/parser/parse.h"
#include "sql/expr/expression.h"
#include "sql/expr/tuple.h"
#include "common/lang/unordered_map.h"
#include "common/lang/vector.h"

/**
 * @brief Hash Join 算子
 * @ingroup PhysicalOperator
 * @details Hash Join算子，通过构建哈希表实现高效连接
 */
class HashJoinPhysicalOperator : public PhysicalOperator
{
public:
  HashJoinPhysicalOperator(unique_ptr<Expression> join_condition);
  virtual ~HashJoinPhysicalOperator() = default;

  PhysicalOperatorType type() const override { return PhysicalOperatorType::HASH_JOIN; }

  OpType get_op_type() const override { return OpType::INNERHASHJOIN; }

  virtual double calculate_cost(
      LogicalProperty *prop, const vector<LogicalProperty *> &child_log_props, CostModel *cm) override
  {
    // cost_hashjoin = left * hash_cost + right * hash_probe + output * CPU
    if (child_log_props.size() != 2 || prop == nullptr || cm == nullptr) {
      return 0.0;
    }

    double left_card  = child_log_props[0]->get_card();
    double right_card = child_log_props[1]->get_card();
    double output     = prop->get_card();

    return left_card * cm->hash_cost() + right_card * cm->hash_probe() + output * cm->cpu_op();
  }

  RC     open(Trx *trx) override;
  RC     next() override;
  RC     close() override;
  Tuple *current_tuple() override;

private:
  /**
   * @brief 从tuple中提取hash key
   * @param tuple 输入tuple
   * @param expr 用于计算hash key的表达式
   * @param key_value 输出的key值
   * @return RC
   */
  RC extract_hash_key(Tuple *tuple, Expression *expr, Value &key_value);

  /**
   * @brief 构建阶段：遍历左表并构建哈希表
   */
  RC build_hash_table();

private:
  unique_ptr<Expression> join_condition_;  ///< 连接条件（等值表达式）

  // 哈希表：key -> list of ValueListTuple
  // 使用 unordered_multimap 支持多个tuple有相同的key
  struct HashKey {
    Value value;

    bool operator==(const HashKey &other) const {
      int cmp = 0;
      Value::compare(value, other.value, cmp);
      return cmp == 0;
    }
  };

  struct HashKeyHasher {
    size_t operator()(const HashKey &key) const {
      const Value &value = key.value;
      size_t hash_val = 0;

      // 根据Value的类型计算hash
      switch (value.attr_type()) {
        case AttrType::INTS:
          hash_val = std::hash<int>{}(value.get_int());
          break;
        case AttrType::BIGINTS:
          hash_val = std::hash<int64_t>{}(value.get_bigint());
          break;
        case AttrType::FLOATS:
          hash_val = std::hash<float>{}(value.get_float());
          break;
        case AttrType::BOOLEANS:
          hash_val = std::hash<bool>{}(value.get_boolean());
          break;
        case AttrType::CHARS:
          hash_val = std::hash<std::string>{}(value.get_string());
          break;
        case AttrType::DATES:
          hash_val = std::hash<int32_t>{}(value.get_date());
          break;
        case AttrType::NULLS:
          hash_val = 0;
          break;
        default:
          // 对于其他类型，使用data的指针和长度计算hash
          if (value.data() != nullptr && value.length() > 0) {
            hash_val = std::hash<std::string_view>{}(std::string_view(value.data(), value.length()));
          }
          break;
      }

      return hash_val;
    }
  };

  std::unordered_multimap<HashKey, ValueListTuple, HashKeyHasher> hash_table_;

  PhysicalOperator *left_  = nullptr;  ///< 左表算子（build端）
  PhysicalOperator *right_ = nullptr;  ///< 右表算子（probe端）

  Tuple      *right_tuple_ = nullptr;  ///< 当前右表tuple
  JoinedTuple joined_tuple_;           ///< 当前连接后的tuple

  // 探测阶段的状态
  bool probe_started_ = false;  ///< 是否已开始探测阶段
  std::unordered_multimap<HashKey, ValueListTuple, HashKeyHasher>::iterator match_begin_;  ///< 当前匹配范围的开始
  std::unordered_multimap<HashKey, ValueListTuple, HashKeyHasher>::iterator match_end_;    ///< 当前匹配范围的结束
  std::unordered_multimap<HashKey, ValueListTuple, HashKeyHasher>::iterator match_current_; ///< 当前匹配位置

  Trx *trx_ = nullptr;

  std::shared_ptr<vector<TupleCellSpec>> shared_specs_;  ///< 用于ValueListTuple的schema
};