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

  // 如果平均文档长度为0（索引为空），直接返回0分
  if (total_docs_ == 0 || avg_doc_length_ <= 0.0) {
    LOG_DEBUG("Index is empty or avg_doc_length is 0. total_docs=%d, avg_doc_length=%.2f", total_docs_, avg_doc_length_);
    score = 0.0f;
    return RC::SUCCESS;
  }

  // 对查询文本分词
  std::vector<std::string> query_tokens;
  RC rc = tokenize_text(query, query_tokens);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  LOG_DEBUG("Query tokenized: query='%s', tokens=%zu", query.c_str(), query_tokens.size());

  // 获取文档长度
  auto doc_len_it = doc_lengths_.find(rid);
  if (doc_len_it == doc_lengths_.end()) {
    LOG_WARN("Document RID not found in index. rid=%s, total_docs=%d, doc_lengths_size=%zu",
             rid.to_string().c_str(), total_docs_, doc_lengths_.size());
    return RC::RECORD_NOT_EXIST;
  }
  int doc_len = doc_len_it->second;

  // 对每个查询词计算BM25评分
  for (const auto &query_term : query_tokens) {
    auto it = inverted_index_.find(query_term);
    if (it == inverted_index_.end()) {
      continue;
    }

    const auto &posting_list = it->second;
    int df = static_cast<int>(posting_list.size());

    // 查找该文档在posting list中的条目
    int tf = 0;
    for (const auto &entry : posting_list) {
      if (entry.rid == rid) {
        tf = entry.term_freq;
        break;
      }
    }

    if (tf == 0) {
      continue;  // 该文档不包含这个查询词
    }

    // 计算IDF
    float idf = std::log((total_docs_ - df + 0.5) / (df + 0.5));

    // 计算BM25评分贡献
    float numerator = static_cast<float>(tf) * (k1_ + 1.0f);
    float denominator = static_cast<float>(tf) + k1_ * (1.0f - b_ + b_ * doc_len / avg_doc_length_);
    score += idf * (numerator / denominator);
  }

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
