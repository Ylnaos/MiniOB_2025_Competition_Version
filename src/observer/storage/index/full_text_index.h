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

#include "storage/index/index.h"
#include "storage/record/record.h"
#include "sql/expr/jieba_tokenizer.h"
#include <unordered_map>
#include <string>
#include <vector>

class RecordScanner;
class Table;

/**
 * @brief 基于倒排表的全文索引，实现 MATCH ... AGAINST() 所需的 BM25 打分
 */
class FullTextIndex : public Index
{
public:
  FullTextIndex()          = default;
  virtual ~FullTextIndex() = default;

  RC create(Table *table,
            const char *file_name,
            const IndexMeta &index_meta,
            span<const FieldMeta> field_metas) override;

  RC open(Table *table,
          const char *file_name,
          const IndexMeta &index_meta,
          span<const FieldMeta> field_metas) override;

  RC insert_entry(const char *record, const RID *rid) override;
  RC delete_entry(const char *record, const RID *rid) override;

  IndexScanner *create_scanner(const char *left_key,
                               int left_len,
                               bool left_inclusive,
                               const char *right_key,
                               int right_len,
                               bool right_inclusive) override;

  RC sync() override;

  /**
   * @brief 计算指定记录与查询词列表之间的 BM25 分值
   */
  double score(const RID &rid, const std::vector<std::string> &query_tokens) const;

  const std::string &parser_name() const { return parser_name_; }
  const FieldMeta   &target_field() const { return field_metas_.front(); }
  Table             *table() const { return table_; }

  double score_tokens(const std::unordered_map<std::string, int> &term_freq,
                      int doc_length,
                      const std::vector<std::string> &query_tokens) const;

  bool is_full_text_index() const override { return true; }

private:
  RC initialize(Table *table, const IndexMeta &index_meta, span<const FieldMeta> field_metas);
  RC rebuild_from_table();
  RC index_record(const char *record, const RID &rid);
  RC remove_document(const RID &rid);

  RC extract_document_text(const char *record, std::string &out) const;
  RC tokenize(const std::string &text, std::vector<std::string> &tokens) const;

private:
  Table *table_ = nullptr;
  std::string parser_name_ = "jieba";

  using TermFreqMap = std::unordered_map<std::string, int>;
  std::unordered_map<std::string, std::unordered_map<RID, int, RIDHash>> inverted_index_;
  std::unordered_map<RID, TermFreqMap, RIDHash>                          doc_term_freqs_;
  std::unordered_map<RID, int, RIDHash>                                  doc_lengths_;

  size_t total_docs_        = 0;
  double total_doc_length_  = 0.0;
  double avg_doc_length_    = 0.0;
};
