/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/expr/aggregate_state.h"
#include <stdint.h>

#ifdef USE_SIMD
#include "common/math/simd_util.h"
#endif
template <typename T>
void SumState<T>::update(const T *values, int size)
{
#ifdef USE_SIMD
  if constexpr (is_same<T, float>::value) {
    value += mm256_sum_ps(values, size);
  } else if constexpr (is_same<T, int>::value) {
    value += mm256_sum_epi32(values, size);
  }
#else
  for (int i = 0; i < size; ++i) {
	  value += values[i];
  }
#endif
  if (size > 0) {
    has_value = true;
  }
}

template <typename T>
void AvgState<T>::update(const T *values, int size)
{
  for (int i = 0; i < size; ++i) {
 	  value += values[i];
  }
  count += size;
}

template <typename T>
void CountState<T>::update(const T *values, int size)
{
  value += size;
}

void* create_aggregate_state(AggregateExpr::Type aggr_type, AttrType attr_type)
{
  void* state_ptr = nullptr;
  if (aggr_type == AggregateExpr::Type::SUM) {
    if (attr_type == AttrType::INTS) {
      state_ptr = malloc(sizeof(SumState<int>));
      new (state_ptr) SumState<int>();
    } else if (attr_type == AttrType::FLOATS) {
      state_ptr = malloc(sizeof(SumState<float>));
      new (state_ptr) SumState<float>();
    } else {
      LOG_WARN("unsupported aggregate value type");
    }
  } else if (aggr_type == AggregateExpr::Type::COUNT) {
    state_ptr = malloc(sizeof(CountState<int>));
    new (state_ptr) CountState<int>();
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    if (attr_type == AttrType::INTS) {
      state_ptr = malloc(sizeof(AvgState<int>));
      new (state_ptr) AvgState<int>();
    } else if (attr_type == AttrType::FLOATS) {
      state_ptr = malloc(sizeof(AvgState<float>));
      new (state_ptr) AvgState<float>();
    } else {
      LOG_WARN("unsupported aggregate value type");
    }
  } else if (aggr_type == AggregateExpr::Type::MAX || aggr_type == AggregateExpr::Type::MIN) {
    // MIN/MAX support any comparable type via Value container
    state_ptr = malloc(sizeof(MinMaxState));
    new (state_ptr) MinMaxState();
  } else {
    LOG_WARN("unsupported aggregator type");
  }
  return state_ptr;
}

RC aggregate_state_update_by_value(void *state, AggregateExpr::Type aggr_type, AttrType attr_type, const Value& val)
{
  RC rc = RC::SUCCESS;
  // 忽略 NULL 值
  if (val.is_null()) {
    // COUNT(*) 在 binder 阶段被替换成常量 1，不会走到这里的 NULL
    return RC::SUCCESS;
  }
  if (aggr_type == AggregateExpr::Type::SUM) {
    if (attr_type == AttrType::INTS) {
      static_cast<SumState<int>*>(state)->update(val.get_int());
    } else if (attr_type == AttrType::FLOATS) {
      static_cast<SumState<float>*>(state)->update(val.get_float());
    } else {
      LOG_WARN("unsupported aggregate value type");
      return RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::COUNT) {
    static_cast<CountState<int>*>(state)->update(1);
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    if (attr_type == AttrType::INTS) {
      static_cast<AvgState<int>*>(state)->update(val.get_int());
    } else if (attr_type == AttrType::FLOATS) {
      static_cast<AvgState<float>*>(state)->update(val.get_float());
    } else {
      LOG_WARN("unsupported aggregate value type");
      return RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::MAX || aggr_type == AggregateExpr::Type::MIN) {
    auto *st = reinterpret_cast<MinMaxState *>(state);
    if (val.is_null()) {
      return RC::SUCCESS; // ignore NULLs
    }
    if (!st->has_value) {
      st->value     = val;
      st->has_value = true;
      return RC::SUCCESS;
    }
    int cmp = 0;
    Value::compare(st->value, val, cmp);
    if ((aggr_type == AggregateExpr::Type::MAX && cmp < 0) || (aggr_type == AggregateExpr::Type::MIN && cmp > 0)) {
      st->value = val;
    }
    return RC::SUCCESS;
  } else {
    LOG_WARN("unsupported aggregator type");
    return RC::UNIMPLEMENTED;
  }
  return rc;
}

template <class STATE, typename T>
void append_to_column(void *state, Column &column)
{
  STATE *state_ptr = reinterpret_cast<STATE *>(state);
  T res = state_ptr->template finalize<T>();
  column.append_one((char *)&res);
}

RC finialize_aggregate_state(void *state, AggregateExpr::Type aggr_type, AttrType attr_type, Column& col)
{
  RC rc = RC::SUCCESS;
  if ( aggr_type == AggregateExpr::Type::SUM) {
    if (attr_type == AttrType::INTS) {
      auto *st = reinterpret_cast<SumState<int> *>(state);
      if (!st->has_value) {
        Value v; v.set_null();
        col.set_attr_type(AttrType::NULLS);
        col.append_value(v);
      } else {
        append_to_column<SumState<int>, int>(state, col);
      }
    } else if (attr_type == AttrType::FLOATS) {
      auto *st = reinterpret_cast<SumState<float> *>(state);
      if (!st->has_value) {
        Value v; v.set_null();
        col.set_attr_type(AttrType::NULLS);
        col.append_value(v);
      } else {
        append_to_column<SumState<float>, float>(state, col);
      }
    } else {
      rc = RC::UNIMPLEMENTED;
      LOG_WARN("unsupported aggregate value type");
    }
  } else if (aggr_type == AggregateExpr::Type::COUNT) {
    append_to_column<CountState<int>, int>(state, col);
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    if (attr_type == AttrType::INTS) {
      auto *st = reinterpret_cast<AvgState<int> *>(state);
      if (st->count == 0) {
        Value v; v.set_null();
        col.set_attr_type(AttrType::NULLS);
        col.append_value(v);
      } else {
        append_to_column<AvgState<int>, float>(state, col);
      }
    } else if (attr_type == AttrType::FLOATS) {
      auto *st = reinterpret_cast<AvgState<float> *>(state);
      if (st->count == 0) {
        Value v; v.set_null();
        col.set_attr_type(AttrType::NULLS);
        col.append_value(v);
      } else {
        append_to_column<AvgState<float>, float>(state, col);
      }
    } else {
      rc = RC::UNIMPLEMENTED;
      LOG_WARN("unsupported aggregate value type");
    }// 
  } else if (aggr_type == AggregateExpr::Type::MAX || aggr_type == AggregateExpr::Type::MIN) {
    auto *st = reinterpret_cast<MinMaxState *>(state);
    if (!st->has_value) {
      // All NULLs -> result NULL
      Value v; v.set_null();
      col.set_attr_type(AttrType::NULLS);
      col.append_value(v);
    } else {
      col.append_value(st->value);
    }
  } else {
    rc = RC::UNIMPLEMENTED;
    LOG_WARN("unsupported aggregator type");
  }
  return rc;
}

void reset_aggregate_state(void *state, AggregateExpr::Type aggr_type, AttrType attr_type)
{
  if (state == nullptr) {
    return;
  }

  if (aggr_type == AggregateExpr::Type::SUM) {
    if (attr_type == AttrType::INTS) {
      auto *st = static_cast<SumState<int> *>(state);
      st->value     = 0;
      st->has_value = false;
    } else if (attr_type == AttrType::FLOATS) {
      auto *st = static_cast<SumState<float> *>(state);
      st->value     = 0.0f;
      st->has_value = false;
    }
  } else if (aggr_type == AggregateExpr::Type::COUNT) {
    auto *st = static_cast<CountState<int> *>(state);
    st->value = 0;
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    if (attr_type == AttrType::INTS) {
      auto *st = static_cast<AvgState<int> *>(state);
      st->value = 0;
      st->count = 0;
    } else if (attr_type == AttrType::FLOATS) {
      auto *st = static_cast<AvgState<float> *>(state);
      st->value = 0.0f;
      st->count = 0;
    }
  } else if (aggr_type == AggregateExpr::Type::MAX || aggr_type == AggregateExpr::Type::MIN) {
    auto *st = static_cast<MinMaxState *>(state);
    st->has_value = false;
  }
}

template <class STATE, typename T>
void update_aggregate_state(void *state, const Column &column)
{
  STATE *state_ptr = reinterpret_cast<STATE *>(state);
  T *    data      = (T *)column.data();
  state_ptr->update(data, column.count());
}

RC aggregate_state_update_by_column(void *state, AggregateExpr::Type aggr_type, AttrType attr_type, Column& col)
{
  RC rc = RC::SUCCESS;
  if (aggr_type == AggregateExpr::Type::SUM) {
    if (attr_type == AttrType::INTS) {
      update_aggregate_state<SumState<int>, int>(state, col);
    } else if (attr_type == AttrType::FLOATS) {
      update_aggregate_state<SumState<float>, float>(state, col);
    } else {
      LOG_WARN("unsupported aggregate value type");
      rc = RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::COUNT) {
    // COUNT(expr) 忽略 NULL，逐行检查以规避列向量缺少 NULL 位图的问题
    const int rows = col.count();
    for (int i = 0; i < rows; ++i) {
      Value v = col.get_value(i);
      if (!v.is_null()) {
        static_cast<CountState<int> *>(state)->update(1);
      }
    }
  } else if (aggr_type == AggregateExpr::Type::AVG) {
    if (attr_type == AttrType::INTS) {
      update_aggregate_state<AvgState<int>, int>(state, col);
    } else if (attr_type == AttrType::FLOATS) {
      update_aggregate_state<AvgState<float>, float>(state, col);
    } else {
      LOG_WARN("unsupported aggregate value type");
      rc = RC::UNIMPLEMENTED;
    }
  } else if (aggr_type == AggregateExpr::Type::MAX || aggr_type == AggregateExpr::Type::MIN) {
    // Generic path: iterate row by row using Value wrapper
    const int rows = col.count();
    for (int i = 0; i < rows; ++i) {
      Value v = col.get_value(i);
      RC rc2 = aggregate_state_update_by_value(state, aggr_type, attr_type, v);
      if (rc2 != RC::SUCCESS) {
        return rc2;
      }
    }
  } else {
    LOG_WARN("unsupported aggregator type");
    rc = RC::UNIMPLEMENTED;
  }
  return rc;
}

template class SumState<int>;
template class SumState<float>;

template class CountState<int>;

template class AvgState<int>;
template class AvgState<float>;
