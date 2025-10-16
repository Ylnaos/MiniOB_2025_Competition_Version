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
#include <vector>
#include <string>
#include <memory>

/**
 * @brief 倒排列表项：存储RID和对应的向量
 */
struct IvfEntry
{
  RID           rid;
  vector<float> vector_data;

  IvfEntry() = default;
  IvfEntry(const RID &r, const vector<float> &v) : rid(r), vector_data(v) {}
};

/**
 * @brief ivfflat 向量索引
 * @ingroup Index
 * @details 实现基于倒排文件的向量索引，使用K-Means聚类加速ANN搜索
 */
class IvfflatIndex : public Index
{
public:
  IvfflatIndex() = default;
  virtual ~IvfflatIndex() noexcept;

  /**
   * @brief 创建索引：扫描表数据，执行K-Means聚类，构建倒排索引
   */
  RC create(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas) override;

  /**
   * @brief 打开已存在的索引：加载聚类中心和倒排列表
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
   * @brief 插入向量条目到索引
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
   * @brief K-Means聚类算法
   * @param vectors 所有向量数据
   * @param k 聚类数量（lists参数）
   * @param max_iter 最大迭代次数
   */
  void kmeans_clustering(const vector<vector<float>> &vectors, int k, int max_iter = 100);

  /**
   * @brief 计算L2距离
   */
  float compute_l2_distance(const vector<float> &a, const vector<float> &b) const;

  /**
   * @brief 找到距离查询向量最近的n个聚类中心
   */
  vector<int> find_nearest_clusters(const vector<float> &query_vector, int n) const;

  /**
   * @brief 从记录中提取向量数据
   */
  RC extract_vector_from_record(const char *record, vector<float> &vec) const;

  /**
   * @brief 保存索引到文件
   */
  RC save_to_file();

  /**
   * @brief 从文件加载索引
   */
  RC load_from_file();

  void apply_meta_config(const IndexMeta &index_meta);

private:
  bool   inited_ = false;
  Table *table_  = nullptr;
  string file_name_;

  // 索引参数
  int lists_  = 100;   // 聚类数量
  int probes_ = 10;    // 查询时探测的聚类数量
  int dimension_ = 0;  // 向量维度
  string distance_type_;
  string index_type_;

  // 向量字段信息
  string vector_field_name_;  // 保存字段名而不是指针，避免悬空指针

  /**
   * @brief 从table中安全地获取FieldMeta
   */
  const FieldMeta *get_vector_field_meta() const;

  // 核心数据结构
  vector<vector<float>>        centroids_;       // 聚类中心 [lists][dimension]
  vector<vector<IvfEntry>>     inverted_lists_;  // 倒排列表 [lists][entries]
};
