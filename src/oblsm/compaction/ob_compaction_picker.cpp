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

namespace {
size_t level_target_size(const ObLsmOptions *options, size_t level)
{
  size_t target = options->default_l1_level_size;
  for (size_t i = 1; i < level; ++i) {
    target *= options->default_level_ratio;
  }
  return target;
}

size_t level_size(const vector<shared_ptr<ObSSTable>> &level)
{
  size_t size = 0;
  for (const auto &sstable : level) {
    size += sstable->size();
  }
  return size;
}

bool ranges_overlap(const ObDefaultComparator &comparator, const string_view &a_first, const string_view &a_last,
    const string_view &b_first, const string_view &b_last)
{
  return comparator.compare(a_first, b_last) <= 0 && comparator.compare(b_first, a_last) <= 0;
}

void input_user_range(const vector<shared_ptr<ObSSTable>> &inputs, string &first, string &last)
{
  ObDefaultComparator comparator;
  bool                initialized = false;
  for (const auto &sstable : inputs) {
    string      sst_first_key = sstable->first_key();
    string      sst_last_key  = sstable->last_key();
    string_view sst_first     = extract_user_key(sst_first_key);
    string_view sst_last      = extract_user_key(sst_last_key);
    if (!initialized) {
      first.assign(sst_first.data(), sst_first.size());
      last.assign(sst_last.data(), sst_last.size());
      initialized = true;
      continue;
    }
    if (comparator.compare(sst_first, first) < 0) {
      first.assign(sst_first.data(), sst_first.size());
    }
    if (comparator.compare(sst_last, last) > 0) {
      last.assign(sst_last.data(), sst_last.size());
    }
  }
}
}  // namespace

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

unique_ptr<ObCompaction> LeveledCompactionPicker::pick(SSTablesPtr sstables)
{
  if (sstables == nullptr || sstables->size() < 2) {
    return nullptr;
  }

  int    picked_level = -1;
  double picked_score = 1.0;
  for (size_t level = 0; level + 1 < sstables->size(); ++level) {
    double score = 0;
    if (level == 0) {
      score = static_cast<double>((*sstables)[0].size()) / options_->default_l0_file_num;
    } else {
      score = static_cast<double>(level_size((*sstables)[level])) / level_target_size(options_, level);
    }
    if (score > picked_score) {
      picked_score = score;
      picked_level = static_cast<int>(level);
    }
  }

  if (picked_level < 0) {
    return nullptr;
  }

  unique_ptr<ObCompaction> compaction(new ObCompaction(picked_level));
  auto &level_inputs = (*sstables)[picked_level];
  if (picked_level == 0) {
    compaction->inputs_[0] = level_inputs;
  } else if (!level_inputs.empty()) {
    compaction->inputs_[0].emplace_back(level_inputs.back());
  }

  if (compaction->inputs_[0].empty()) {
    return nullptr;
  }

  string first;
  string last;
  input_user_range(compaction->inputs_[0], first, last);

  ObDefaultComparator comparator;
  for (const auto &sstable : (*sstables)[picked_level + 1]) {
    string      sst_first_key = sstable->first_key();
    string      sst_last_key  = sstable->last_key();
    string_view sst_first     = extract_user_key(sst_first_key);
    string_view sst_last      = extract_user_key(sst_last_key);
    if (ranges_overlap(comparator, first, last, sst_first, sst_last)) {
      compaction->inputs_[1].emplace_back(sstable);
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
