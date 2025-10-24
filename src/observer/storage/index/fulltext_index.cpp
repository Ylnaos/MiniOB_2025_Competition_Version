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
// Created for full-text index implementation
//

#include "storage/index/fulltext_index.h"
#include "storage/table/table.h"
#include "storage/record/record_manager.h"
#include "storage/record/record_scanner.h"
#include "storage/record/lob_ref.h"
#include "common/log/log.h"
#include <cmath>
#include <algorithm>
#include <cstring>

// 包含表达式头文件以使用全局分词函数
#include "sql/expr/expression.h"

RC FullTextIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas)
{
  if (table == nullptr) {
    LOG_WARN("table is nullptr");
    return RC::INVALID_ARGUMENT;
  }

  table_ = table;
  RC rc = init(index_meta, field_metas);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to init index. rc=%s", strrc(rc));
    return rc;
  }

  // 获取分词器名称
  parser_name_ = index_meta.full_text_parser();
  if (parser_name_.empty()) {
    parser_name_ = "jieba";
  }

  LOG_INFO("Creating full-text index on table %s, parser=%s", table->name(), parser_name_.c_str());

  // 遍历表中所有记录，构建倒排索引
  RecordScanner *scanner = nullptr;
  rc = table->get_record_scanner(scanner, nullptr, ReadWriteMode::READ_ONLY);
  if (rc != RC::SUCCESS || !scanner) {
    LOG_WARN("failed to create scanner. rc=%s", strrc(rc));
    return rc;
  }

  Record record;
  while (RC::SUCCESS == (rc = scanner->next(record))) {
    rc = insert_entry(record.data(), &record.rid());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to insert entry. rc=%s", strrc(rc));
      scanner->close_scan();
      delete scanner;
      return rc;
    }
  }

  scanner->close_scan();
  delete scanner;

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("failed to scan table. rc=%s", strrc(rc));
    return rc;
  }

  LOG_INFO("Full-text index created. total_docs=%d, avg_doc_length=%.2f", total_docs_, avg_doc_length_);
  return RC::SUCCESS;
}

RC FullTextIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas)
{
  // 全文索引不使用磁盘文件，每次启动时重新构建索引
  // 这样可以避免文件不存在的问题
  if (table == nullptr) {
    LOG_WARN("table is nullptr");
    return RC::INVALID_ARGUMENT;
  }

  table_ = table;
  RC rc = init(index_meta, field_metas);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to init index. rc=%s", strrc(rc));
    return rc;
  }

  // 获取分词器名称
  parser_name_ = index_meta.full_text_parser();
  if (parser_name_.empty()) {
    parser_name_ = "jieba";
  }

  LOG_INFO("Opening full-text index on table %s, parser=%s, rebuilding index...", table->name(), parser_name_.c_str());

  // 清空现有索引数据
  inverted_index_.clear();
  doc_lengths_.clear();
  total_docs_ = 0;
  avg_doc_length_ = 0.0;

  // 遍历表中所有记录，重新构建倒排索引
  RecordScanner *scanner = nullptr;
  rc = table->get_record_scanner(scanner, nullptr, ReadWriteMode::READ_ONLY);
  if (rc != RC::SUCCESS || !scanner) {
    LOG_WARN("failed to create scanner. rc=%s", strrc(rc));
    return rc;
  }

  Record record;
  while (RC::SUCCESS == (rc = scanner->next(record))) {
    rc = insert_entry(record.data(), &record.rid());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to insert entry. rc=%s", strrc(rc));
      scanner->close_scan();
      delete scanner;
      return rc;
    }
  }

  scanner->close_scan();
  delete scanner;

  if (rc != RC::RECORD_EOF) {
    LOG_WARN("failed to scan table. rc=%s", strrc(rc));
    return rc;
  }

  LOG_INFO("Full-text index opened and rebuilt. total_docs=%d, avg_doc_length=%.2f", total_docs_, avg_doc_length_);
  return RC::SUCCESS;
}

RC FullTextIndex::insert_entry(const char *record, const RID *rid)
{
  if (record == nullptr || rid == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  // 提取文本内容
  std::string text;
  RC rc = extract_text_from_record(record, text);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  // 分词
  std::vector<std::string> tokens;
  rc = tokenize_text(text, tokens);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  // 统计词频
  unordered_map<std::string, int> term_freq_map;
  for (const auto &token : tokens) {
    term_freq_map[token]++;
  }

  int doc_length = static_cast<int>(tokens.size());

  // 更新倒排索引
  for (const auto &pair : term_freq_map) {
    const std::string &term = pair.first;
    int tf = pair.second;

    PostingEntry entry(*rid, tf, doc_length);
    inverted_index_[term].push_back(entry);
  }

  // 更新文档长度映射
  doc_lengths_[*rid] = doc_length;
  total_docs_++;

  // 更新平均文档长度
  update_avg_doc_length();

  return RC::SUCCESS;
}

RC FullTextIndex::delete_entry(const char *record, const RID *rid)
{
  if (rid == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  // 从文档长度映射中删除
  auto it = doc_lengths_.find(*rid);
  if (it == doc_lengths_.end()) {
    return RC::RECORD_NOT_EXIST;
  }

  doc_lengths_.erase(it);
  total_docs_--;

  // 从倒排索引中删除该文档的所有条目
  for (auto &pair : inverted_index_) {
    auto &posting_list = pair.second;
    posting_list.erase(
      std::remove_if(posting_list.begin(), posting_list.end(),
        [rid](const PostingEntry &entry) { return entry.rid == *rid; }),
      posting_list.end()
    );
  }

  // 更新平均文档长度
  update_avg_doc_length();

  return RC::SUCCESS;
}

RC FullTextIndex::sync()
{
  // TODO: 实现倒排索引持久化
  // 目前暂时不支持持久化
  return RC::SUCCESS;
}

RC FullTextIndex::search(const std::string &query, std::vector<std::pair<RID, float>> &results)
{
  results.clear();

  // 对查询文本分词
  std::vector<std::string> query_tokens;
  RC rc = tokenize_text(query, query_tokens);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  if (query_tokens.empty()) {
    return RC::SUCCESS;
  }

  // 收集包含任一查询词的所有文档
  unordered_map<RID, float> doc_scores;

  for (const auto &query_term : query_tokens) {
    auto it = inverted_index_.find(query_term);
    if (it == inverted_index_.end()) {
      continue;  // 该词不存在于倒排索引中
    }

    const auto &posting_list = it->second;
    int df = static_cast<int>(posting_list.size());  // 文档频率

    // 计算IDF
    // IDF(qi) = log((N - df + 0.5) / (df + 0.5))
    float idf = std::log((total_docs_ - df + 0.5) / (df + 0.5));

    // 对每个包含该词的文档计算BM25评分贡献
    for (const auto &entry : posting_list) {
      int tf = entry.term_freq;
      int doc_len = entry.doc_length;

      // BM25 公式: IDF * (TF * (k1 + 1)) / (TF + k1 * (1 - b + b * doc_len / avg_doc_len))
      float numerator = static_cast<float>(tf) * (k1_ + 1.0f);
      float denominator = static_cast<float>(tf) + k1_ * (1.0f - b_ + b_ * doc_len / avg_doc_length_);
      float score_contribution = idf * (numerator / denominator);

      doc_scores[entry.rid] += score_contribution;
    }
  }

  // 将结果转换为vector
  for (const auto &pair : doc_scores) {
    if (pair.second > 0) {
      results.emplace_back(pair.first, pair.second);
    }
  }

  return RC::SUCCESS;
}

RC FullTextIndex::calculate_bm25_score(const RID &rid, const std::string &query, float &score)
{
  score = 0.0f;

  LOG_INFO("BM25: Starting score calculation for RID{%d:%d}, query='%s'",
           rid.page_num, rid.slot_num, query.c_str());

  // 对查询文本分词
  std::vector<std::string> query_tokens;
  RC rc = tokenize_text(query, query_tokens);
  if (rc != RC::SUCCESS || query_tokens.empty()) {
    LOG_INFO("BM25: tokenization failed or empty tokens for query='%s', rc=%s, tokens_size=%zu",
             query.c_str(), strrc(rc), query_tokens.size());
    return RC::SUCCESS;
  }

  LOG_INFO("BM25: query='%s' tokenized into %zu tokens", query.c_str(), query_tokens.size());
  for (size_t i = 0; i < query_tokens.size(); i++) {
    LOG_INFO("BM25:   token[%zu]='%s'", i, query_tokens[i].c_str());
  }

  // 获取文档长度
  LOG_INFO("BM25: Looking for RID{%d:%d} in doc_lengths_, total_docs=%d",
           rid.page_num, rid.slot_num, total_docs_);
  LOG_INFO("BM25: doc_lengths_ size=%zu", doc_lengths_.size());

  // 打印所有文档RID用于调试
  int count = 0;
  for (const auto &pair : doc_lengths_) {
    if (count < 5) {  // 只打印前5个避免日志过多
      LOG_INFO("BM25:   doc_lengths_[%d:%d] = %d",
               pair.first.page_num, pair.first.slot_num, pair.second);
    }
    count++;
  }
  if (count > 5) {
    LOG_INFO("BM25:   ... and %d more documents", count - 5);
  }

  auto doc_len_it = doc_lengths_.find(rid);
  if (doc_len_it == doc_lengths_.end()) {
    LOG_INFO("BM25: ERROR - document RID{%d:%d} not found in doc_lengths_!",
             rid.page_num, rid.slot_num);
    return RC::SUCCESS;
  }
  int doc_len = doc_len_it->second;

  LOG_INFO("BM25: found document RID{%d:%d} with length=%d, avg_doc_length=%.2f, total_docs=%d",
           rid.page_num, rid.slot_num, doc_len, avg_doc_length_, total_docs_);

  // 防止除零错误
  if (avg_doc_length_ <= 0.0 || total_docs_ <= 0) {
    LOG_INFO("BM25: ERROR - invalid parameters, avg_doc_length=%.2f, total_docs=%d",
             avg_doc_length_, total_docs_);
    return RC::SUCCESS;
  }

  float total_score = 0.0f;
  int matched_terms = 0;

  LOG_INFO("BM25: inverted_index_ size=%zu", inverted_index_.size());

  // 对每个查询词计算BM25评分
  for (const auto &query_term : query_tokens) {
    LOG_INFO("BM25: processing query_term='%s'", query_term.c_str());
    auto it = inverted_index_.find(query_term);
    if (it == inverted_index_.end()) {
      LOG_INFO("BM25: query_term='%s' not found in inverted_index_", query_term.c_str());
      continue;
    }

    const auto &posting_list = it->second;
    int df = static_cast<int>(posting_list.size());
    LOG_INFO("BM25: query_term='%s' found in %d documents", query_term.c_str(), df);
    LOG_INFO("BM25: posting_list has %d entries", df);

    // 如果文档频率为0，跳过
    if (df <= 0) {
      continue;
    }

    // 查找该文档在posting list中的条目
    int tf = 0;
    LOG_INFO("BM25: searching for RID{%d:%d} in posting_list for term='%s' (size=%d)",
             rid.page_num, rid.slot_num, query_term.c_str(), df);

    for (int i = 0; i < posting_list.size() && i < 3; i++) {  // 只检查前3个条目避免日志过多
      const auto &entry = posting_list[i];
      LOG_INFO("BM25:   posting_list[%d]: RID{%d:%d}, tf=%d",
               i, entry.rid.page_num, entry.rid.slot_num, entry.term_freq);
    }
    if (posting_list.size() > 3) {
      LOG_INFO("BM25:   ... and %d more entries", posting_list.size() - 3);
    }

    for (const auto &entry : posting_list) {
      if (entry.rid == rid) {
        tf = entry.term_freq;
        LOG_INFO("BM25: *** FOUND document RID{%d:%d} in posting list for term='%s', tf=%d ***",
                 rid.page_num, rid.slot_num, query_term.c_str(), tf);
        break;
      }
    }

    if (tf == 0) {
      LOG_INFO("BM25: *** NOT FOUND document RID{%d:%d} in posting list for term='%s' ***",
               rid.page_num, rid.slot_num, query_term.c_str());
      continue;  // 该文档不包含这个查询词
    }

    // 计算IDF - 使用标准BM25的IDF公式
    // log((N - df + 0.5) / (df + 0.5))，其中N是总文档数
    float idf = std::log((total_docs_ - df + 0.5f) / (df + 0.5f));

    // 确保IDF不为负数
    if (idf < 0.0f) {
      idf = 0.0f;
    }

    // 计算BM25评分贡献
    // BM25 = IDF * (tf * (k1 + 1)) / (tf + k1 * (1 - b + b * doc_len / avg_doc_len))
    float numerator = static_cast<float>(tf) * (k1_ + 1.0f);
    float denominator = static_cast<float>(tf) + k1_ * (1.0f - b_ + b_ * static_cast<float>(doc_len) / avg_doc_length_);

    // 防止除零错误
    if (denominator > 0.0f) {
      float term_score = idf * (numerator / denominator);
      total_score += term_score;
      matched_terms++;
      LOG_INFO("BM25: term='%s' tf=%d idf=%.3f numerator=%.3f denominator=%.3f term_score=%.3f total_score=%.3f",
             query_term.c_str(), tf, idf, numerator, denominator, term_score, total_score);
    } else {
      LOG_INFO("BM25: term='%s' denominator <= 0, skipping", query_term.c_str());
    }
  }

  score = total_score;
  LOG_INFO("BM25: *** FINAL SCORE=%.6f for RID{%d:%d}, matched_terms=%d/%zu ***",
           score, rid.page_num, rid.slot_num, matched_terms, query_tokens.size());

  return RC::SUCCESS;
}

RC FullTextIndex::extract_text_from_record(const char *record, std::string &text)
{
  // 从记录中提取第一个字段的文本内容
  // 全文索引只支持单字段
  if (field_metas_.empty()) {
    return RC::INTERNAL;
  }

  const FieldMeta &field = field_metas_[0];
  const char *field_data = record + field.offset();

  if (field.type() == AttrType::CHARS) {
    // CHAR类型
    int len = field.len();
    text.assign(field_data, len);
    // 移除尾部的空字符
    size_t pos = text.find('\0');
    if (pos != std::string::npos) {
      text.resize(pos);
    }
  } else if (field.type() == AttrType::TEXTS) {
    // TEXT类型 - 存储为LobRef，需要通过LOB handler获取实际数据
    LobRef ref;
    memcpy(&ref, field_data, sizeof(LobRef));

    const int32_t length = ref.length;
    const int64_t offset = ref.offset;

    if (length <= 0) {
      // 空文本
      text = "";
      return RC::SUCCESS;
    }

    if (table_ == nullptr || table_->lob_handler() == nullptr) {
      LOG_WARN("table or lob_handler is null");
      return RC::INTERNAL;
    }

    std::vector<char> buf;
    buf.resize(static_cast<size_t>(length));
    RC rc = table_->lob_handler()->get_data(offset, length, buf.data());
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get lob data. rc=%s", strrc(rc));
      return rc;
    }

    text.assign(buf.data(), length);
  } else {
    LOG_WARN("unsupported field type for full-text index: %d", static_cast<int>(field.type()));
    return RC::SCHEMA_FIELD_TYPE_MISMATCH;
  }

  return RC::SUCCESS;
}

RC FullTextIndex::tokenize_text(const std::string &text, std::vector<std::string> &tokens)
{
  // 调用全局 jieba_tokenize 函数进行分词
  return jieba_tokenize(text, parser_name_, tokens);
}

void FullTextIndex::update_avg_doc_length()
{
  if (total_docs_ == 0) {
    avg_doc_length_ = 0.0;
    return;
  }

  double total_length = 0.0;
  for (const auto &pair : doc_lengths_) {
    total_length += pair.second;
  }

  avg_doc_length_ = total_length / total_docs_;
}
