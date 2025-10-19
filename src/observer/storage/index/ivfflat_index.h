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
#include <memory>
#include <string>
#include <vector>
#include <fstream>

/**
 * @brief ivfflat 向量索引（使用Annoy库实现）
 * @ingroup Index
 * @details 使用Spotify的Annoy库实现高性能ANN搜索，替代原有的K-Means聚类实现
 */
class IvfflatIndex : public Index
{
public:
  IvfflatIndex() = default;
  virtual ~IvfflatIndex() noexcept;

  /**
   * @brief 创建索引：扫描表数据，使用Annoy构建索引
   */
  RC create(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas) override;

  /**
   * @brief 打开已存在的索引：加载Annoy索引和RID映射
   */
  RC open(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas) override;

  /**
   * @brief ANN搜索：返回最近的k个向量的RID
   */
  vector<RID> ann_search(const vector<float> &query_vector, size_t limit);

  /**
   * @brief 关闭索引
   */
  RC close();

  /**
   * @brief 插入向量条目到索引（暂不支持动态插入，需要重建索引）
   */
  RC insert_entry(const char *record, const RID *rid) override;

  /**
   * @brief 删除向量条目（暂不实现）
   */
  RC delete_entry(const char *record, const RID *rid) override;

  /**
   * @brief 同步索引到磁盘
   */
  RC sync() override;

  /**
   * @brief 向量索引不支持范围扫描
   */
  IndexScanner *create_scanner(const char *left_key, int left_len, bool left_inclusive,
                                 const char *right_key, int right_len, bool right_inclusive) override;

  /**
   * @brief 标识这是向量索引
   */
  bool is_vector_index() override { return true; }

  /**
   * @brief 判断索引是否已经完成初始化并具备查询能力
   */
  bool ready() const;

  /**
   * @brief 获取索引配置的向量维度（未就绪时返回0）
   */
  int dimension() const { return dimension_; }

private:
  /**
   * @brief 从记录中提取向量数据
   */
  RC extract_vector_from_record(const char *record, vector<float> &vec) const;

  /**
   * @brief 保存RID映射到辅助文件
   */
  RC save_rid_mapping();

  /**
   * @brief 加载RID映射（使用mmap）
   */
  RC load_rid_mapping();

  /**
   * @brief 从table中安全地获取FieldMeta
   */
  const FieldMeta *get_vector_field_meta() const;

  /**
   * @brief 应用IndexMeta中的配置参数
   */
  void apply_meta_config(const IndexMeta &index_meta);

private:
  bool   inited_ = false;
  Table *table_  = nullptr;
  string file_name_;

  // Annoy索引参数
  int lists_  = 100;   // Annoy中对应n_trees（构建的树数量）
  int probes_ = -1;    // Annoy中对应search_k（搜索参数，-1表示auto）
  int dimension_ = 0;  // 向量维度
  string distance_type_;
  string index_type_;

  // 向量字段信息
  string vector_field_name_;

  // Annoy核心数据
  void *annoy_index_ = nullptr;  // Annoy索引对象（void*避免模板暴露）
  std::vector<RID> rid_map_;     // RID映射数组（构建时使用）
  int item_count_ = 0;           // 已添加的向量数量

  // 辅助文件（存储RID映射）
  std::ofstream aux_file_;       // 写入时使用
  RID *rid_map_mmap_ = nullptr;  // mmap加载的RID映射（查询时使用）
};
