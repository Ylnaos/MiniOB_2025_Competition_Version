/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "oblsm/compaction/ob_compaction_picker.h"
#include "common/log/log.h"
#include "oblsm/util/ob_coding.h"

namespace oceanbase {

// TODO: put it in options
unique_ptr<ObCompaction> TiredCompactionPicker::pick(SSTablesPtr sstables)
{
  if (sstables->size() < options_->default_run_num) {
    return nullptr;
  }
  unique_ptr<ObCompaction> compaction(new ObCompaction(0));
  // TODO(opt): a tricky compaction picker, just pick all sstables if enough sstables.
  for (size_t i = 0; i < sstables->size(); ++i) {
    size_t tire_i_size = (*sstables)[i].size();
    for (size_t j = 0; j < tire_i_size; ++j) {
      compaction->inputs_[0].emplace_back((*sstables)[i][j]);
    }
  }
  // TODO: LOG_DEBUG for debug
  return compaction;
}

static bool range_overlap(
    const shared_ptr<ObSSTable> &a, const shared_ptr<ObSSTable> &b, const ObComparator *user_comparator)
{
  string a_first = a->first_key();
  string a_last  = a->last_key();
  string b_first = b->first_key();
  string b_last  = b->last_key();
  if (a_first.empty() || a_last.empty() || b_first.empty() || b_last.empty()) {
    return false;
  }

  return user_comparator->compare(extract_user_key(a_first), extract_user_key(b_last)) <= 0 &&
         user_comparator->compare(extract_user_key(b_first), extract_user_key(a_last)) <= 0;
}

unique_ptr<ObCompaction> LeveledCompactionPicker::pick(SSTablesPtr sstables)
{
  if (sstables == nullptr || sstables->size() < 2) {
    return nullptr;
  }

  int    best_level = -1;
  double best_score = 1.0;
  for (size_t level = 0; level + 1 < sstables->size(); ++level) {
    double score = 0;
    if (level == 0) {
      score = static_cast<double>((*sstables)[0].size()) / options_->default_l0_file_num;
    } else {
      size_t level_limit = options_->default_l1_level_size;
      for (size_t i = 1; i < level; ++i) {
        level_limit *= options_->default_level_ratio;
      }

      size_t level_size = 0;
      for (const auto &sstable : (*sstables)[level]) {
        level_size += sstable->size();
      }
      score = static_cast<double>(level_size) / level_limit;
    }

    if (score > best_score) {
      best_score = score;
      best_level = static_cast<int>(level);
    }
  }

  if (best_level < 0) {
    return nullptr;
  }

  unique_ptr<ObCompaction> compaction(new ObCompaction(best_level));
  auto                   &level_inputs = (*sstables)[best_level];
  auto                   &next_inputs  = (*sstables)[best_level + 1];
  if (best_level == 0) {
    compaction->inputs_[0] = level_inputs;
    compaction->inputs_[1] = next_inputs;
  } else {
    if (level_inputs.empty()) {
      return nullptr;
    }

    auto picked = level_inputs.back();
    compaction->inputs_[0].emplace_back(picked);
    ObDefaultComparator user_comparator;
    for (const auto &sstable : next_inputs) {
      if (range_overlap(picked, sstable, &user_comparator)) {
        compaction->inputs_[1].emplace_back(sstable);
      }
    }
  }

  return compaction;
}

ObCompactionPicker *ObCompactionPicker::create(CompactionType type, ObLsmOptions *options)
{

  switch (type) {
    case CompactionType::TIRED: return new TiredCompactionPicker(options);
    case CompactionType::LEVELED: return new LeveledCompactionPicker(options);
    default: return nullptr;
  }
  return nullptr;
}

}  // namespace oceanbase
