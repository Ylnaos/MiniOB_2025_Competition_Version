/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/index/full_text_index.h"
#include "common/log/log.h"
#include "sql/expr/jieba_tokenizer.h"
#include "storage/record/record.h"
#include "storage/record/record_manager.h"
#include "storage/record/lob_ref.h"
#include "storage/table/table.h"
#include "storage/table/table_meta.h"
#include "storage/table/table_engine.h"
#include "storage/record/record_scanner.h"
#include <cmath>
#include <cstring>

using namespace std;

namespace {
static constexpr double K1 = 1.5;
static constexpr double B  = 0.75;
}

RC FullTextIndex::initialize(Table *table, const IndexMeta &index_meta, span<const FieldMeta> field_metas)
{
  if (field_metas.size() != 1) {
    LOG_WARN("full-text index expects single column");
    return RC::INVALID_ARGUMENT;
  }
  RC rc = Index::init(index_meta, field_metas);
  if (OB_FAIL(rc)) {
    return rc;
  }
  table_       = table;
  parser_name_ = index_meta.full_text_parser().empty() ? "jieba" : index_meta.full_text_parser();
  inverted_index_.clear();
  doc_term_freqs_.clear();
  doc_lengths_.clear();
  total_docs_       = 0;
  total_doc_length_ = 0.0;
  avg_doc_length_   = 0.0;
  return RC::SUCCESS;
}

RC FullTextIndex::create(Table *table,
                         const char * /*file_name*/,
                         const IndexMeta &index_meta,
                         span<const FieldMeta> field_metas)
{
  RC rc = initialize(table, index_meta, field_metas);
  if (OB_FAIL(rc)) {
    return rc;
  }
  return rebuild_from_table();
}

RC FullTextIndex::open(Table *table,
                       const char * /*file_name*/,
                       const IndexMeta &index_meta,
                       span<const FieldMeta> field_metas)
{
  RC rc = initialize(table, index_meta, field_metas);
  if (OB_FAIL(rc)) {
    return rc;
  }
  return rebuild_from_table();
}

RC FullTextIndex::rebuild_from_table()
{
  inverted_index_.clear();
  doc_term_freqs_.clear();
  doc_lengths_.clear();
  total_doc_length_ = 0.0;
  total_docs_       = 0;
  avg_doc_length_   = 0.0;

  RecordScanner *scanner = nullptr;
  RC rc = table_->get_record_scanner(scanner, nullptr, ReadWriteMode::READ_ONLY);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open record scanner when rebuilding full-text index. table=%s rc=%s",
             table_->name(), strrc(rc));
    return rc;
  }

  Record record;
  while (RC::SUCCESS == (rc = scanner->next(record))) {
    rc = index_record(record.data(), record.rid());
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to index record during rebuild. table=%s rid=%s rc=%s",
               table_->name(), record.rid().to_string().c_str(), strrc(rc));
      break;
    }
  }
  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }
  scanner->close_scan();
  delete scanner;

  if (total_docs_ > 0) {
    avg_doc_length_ = total_doc_length_ / static_cast<double>(total_docs_);
  }
  return rc;
}

RC FullTextIndex::tokenize(const string &text, vector<string> &tokens) const
{
  return JiebaTokenizer::tokenize(text, parser_name_, tokens);
}

RC FullTextIndex::extract_document_text(const char *record, string &out) const
{
  const FieldMeta &field = field_metas_.front();
  if (field.type() == AttrType::CHARS) {
    const char *data = record + field.offset();
    int len = field.len();
    out.assign(data, strnlen(data, static_cast<size_t>(len)));
    return RC::SUCCESS;
  } else if (field.type() == AttrType::TEXTS) {
    const char *data = record + field.offset();
    LobRef ref{};
    memcpy(&ref, data, sizeof(LobRef));
    if (ref.length <= 0) {
      out.clear();
      return RC::SUCCESS;
    }
    if (table_ == nullptr || table_->lob_handler() == nullptr) {
      return RC::INTERNAL;
    }
    vector<char> buffer(static_cast<size_t>(ref.length));
    RC rc = table_->lob_handler()->get_data(ref.offset, ref.length, buffer.data());
    if (OB_FAIL(rc)) {
      return rc;
    }
    out.assign(buffer.data(), static_cast<size_t>(ref.length));
    return RC::SUCCESS;
  }
  LOG_WARN("unsupported column type for full-text index: %d", static_cast<int>(field.type()));
  return RC::INVALID_ARGUMENT;
}

RC FullTextIndex::index_record(const char *record, const RID &rid)
{
  remove_document(rid);

  string text;
  RC rc = extract_document_text(record, text);
  if (OB_FAIL(rc)) {
    return rc;
  }

  vector<string> tokens;
  rc = tokenize(text, tokens);
  if (OB_FAIL(rc)) {
    return rc;
  }

  TermFreqMap term_freq;
  term_freq.reserve(tokens.size());
  int doc_len = 0;
  for (const string &token : tokens) {
    if (token.empty()) {
      continue;
    }
    doc_len++;
    term_freq[token] += 1;
  }

  doc_term_freqs_[rid] = term_freq;
  doc_lengths_[rid]    = doc_len;
  total_doc_length_   += doc_len;
  total_docs_          = doc_term_freqs_.size();
  avg_doc_length_      = (total_docs_ > 0) ? (total_doc_length_ / static_cast<double>(total_docs_)) : 0.0;

  for (const auto &entry : term_freq) {
    inverted_index_[entry.first][rid] = entry.second;
  }

  return RC::SUCCESS;
}

RC FullTextIndex::remove_document(const RID &rid)
{
  auto iter = doc_term_freqs_.find(rid);
  if (iter == doc_term_freqs_.end()) {
    return RC::SUCCESS;
  }
  const TermFreqMap &terms = iter->second;
  for (const auto &kv : terms) {
    auto inv_iter = inverted_index_.find(kv.first);
    if (inv_iter != inverted_index_.end()) {
      inv_iter->second.erase(rid);
      if (inv_iter->second.empty()) {
        inverted_index_.erase(inv_iter);
      }
    }
  }

  auto len_iter = doc_lengths_.find(rid);
  if (len_iter != doc_lengths_.end()) {
    total_doc_length_ -= len_iter->second;
    doc_lengths_.erase(len_iter);
  }

  doc_term_freqs_.erase(iter);
  total_docs_ = doc_term_freqs_.size();
  avg_doc_length_ = (total_docs_ > 0) ? (total_doc_length_ / static_cast<double>(total_docs_)) : 0.0;
  return RC::SUCCESS;
}

RC FullTextIndex::insert_entry(const char *record, const RID *rid)
{
  if (rid == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  return index_record(record, *rid);
}

RC FullTextIndex::delete_entry(const char * /*record*/, const RID *rid)
{
  if (rid == nullptr) {
    return RC::INVALID_ARGUMENT;
  }
  return remove_document(*rid);
}

IndexScanner *FullTextIndex::create_scanner(const char * /*left_key*/,
                                            int /*left_len*/,
                                            bool /*left_inclusive*/,
                                            const char * /*right_key*/,
                                            int /*right_len*/,
                                            bool /*right_inclusive*/)
{
  // 全文索引暂不支持范围扫描
  return nullptr;
}

RC FullTextIndex::sync()
{
  // 内存索引，无需单独同步
  return RC::SUCCESS;
}

double FullTextIndex::score(const RID &rid, const vector<string> &query_tokens) const
{
  auto doc_tf_iter = doc_term_freqs_.find(rid);
  if (doc_tf_iter == doc_term_freqs_.end()) {
    return 0.0;
  }
  auto len_iter = doc_lengths_.find(rid);
  const int doc_len = (len_iter != doc_lengths_.end()) ? len_iter->second : 0;
  return score_tokens(doc_tf_iter->second, doc_len, query_tokens);
}

double FullTextIndex::score_tokens(const unordered_map<string, int> &term_freq,
                                   int doc_length,
                                   const vector<string> &query_tokens) const
{
  if (total_docs_ == 0 || avg_doc_length_ <= 0.0 || doc_length <= 0) {
    return 0.0;
  }

  unordered_map<string, int> query_tf;
  query_tf.reserve(query_tokens.size());
  for (const string &token : query_tokens) {
    if (!token.empty()) {
      query_tf[token] += 1;
    }
  }

  if (query_tf.empty()) {
    return 0.0;
  }

  double score = 0.0;
  for (const auto &entry : query_tf) {
    const string &token = entry.first;
    auto inv_iter = inverted_index_.find(token);
    if (inv_iter == inverted_index_.end()) {
      continue;
    }
    auto tf_iter = term_freq.find(token);
    if (tf_iter == term_freq.end() || tf_iter->second <= 0) {
      continue;
    }
    const int tf = tf_iter->second;
    const size_t df = inv_iter->second.size();
    if (df == 0) {
      continue;
    }
    const double idf = log((static_cast<double>(total_docs_) - static_cast<double>(df) + 0.5) /
                           (static_cast<double>(df) + 0.5) + 1.0);
    const double numerator   = static_cast<double>(tf) * (K1 + 1.0);
    const double denominator = static_cast<double>(tf) +
                               K1 * (1.0 - B + B * (static_cast<double>(doc_length) / avg_doc_length_));
    if (denominator <= 0.0) {
      continue;
    }
    score += idf * (numerator / denominator);
  }
  return score;
}

