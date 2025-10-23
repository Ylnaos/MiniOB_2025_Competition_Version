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

#pragma once

#include "storage/index/index.h"
#include "common/lang/span.h"
#include "common/lang/unordered_map.h"
#include <vector>
#include <string>

/**
 * @brief 倒排索引posting条目
 */
struct PostingEntry
{
  RID rid;            ///< 文档ID（记录位置）
  int term_freq;      ///< 词频TF
  int doc_length;     ///< 文档长度（词数）

  PostingEntry() : term_freq(0), doc_length(0) {}
  PostingEntry(const RID &r, int tf, int dl) : rid(r), term_freq(tf), doc_length(dl) {}
};

/**
 * @brief 全文索引（倒排索引）
 * @ingroup Index
 */
class FullTextIndex : public Index
{
public:
  FullTextIndex() = default;
  virtual ~FullTextIndex() = default;

  RC create(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas) override;
  RC open(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas) override;

  RC insert_entry(const char *record, const RID *rid) override;
  RC delete_entry(const char *record, const RID *rid) override;

  /**
   * 全文索引不支持范围扫描，返回 nullptr
   */
  IndexScanner *create_scanner(const char *left_key, int left_len, bool left_inclusive, const char *right_key,
      int right_len, bool right_inclusive) override
  {
    return nullptr;
  }

  RC sync() override;

  /**
   * @brief 执行全文搜索并返回BM25评分
   * @param query 查询文本
   * @param results 输出：<RID, BM25评分> 列表
   */
  RC search(const std::string &query, std::vector<std::pair<RID, float>> &results);

  /**
   * @brief 计算单个文档对查询的BM25评分
   * @param rid 文档ID
   * @param query 查询文本
   * @param score 输出：BM25评分
   */
  RC calculate_bm25_score(const RID &rid, const std::string &query, float &score);

private:
  /**
   * @brief 从记录中提取文本字段内容
   */
  RC extract_text_from_record(const char *record, std::string &text);

  /**
   * @brief 对文本进行分词
   */
  RC tokenize_text(const std::string &text, std::vector<std::string> &tokens);

  /**
   * @brief 更新文档长度统计
   */
  void update_avg_doc_length();

private:
  Table *table_ = nullptr;                                                  ///< 表指针
  std::string parser_name_ = "jieba";                                       ///< 分词器名称
  unordered_map<std::string, std::vector<PostingEntry>> inverted_index_;   ///< 倒排索引
  unordered_map<RID, int> doc_lengths_;                                     ///< 文档长度映射
  int total_docs_ = 0;                                                      ///< 总文档数
  double avg_doc_length_ = 0.0;                                             ///< 平均文档长度

  // BM25 参数
  static constexpr float k1_ = 1.5f;
  static constexpr float b_  = 0.75f;
};
