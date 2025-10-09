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
#include <mutex>
#include <string>
#include <cmath>
#include <algorithm>
#include <random>
#include <queue>
#include <unordered_map>

/**
 * @brief ivfflat 向量索引
 * @ingroup Index
 * @details 实现了基于倒排文件的向量索引,使用K-Means聚类进行ANN搜索
 */
class IvfflatIndex : public Index
{
public:
  IvfflatIndex(){};
  virtual ~IvfflatIndex() noexcept;

  RC create(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas);
  RC open(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas);

  // ANN搜索接口
  vector<RID> ann_search(const vector<float> &query_vector, size_t limit);

  RC close();

  RC insert_entry(const char *record, const RID *rid) override;
  RC delete_entry(const char *record, const RID *rid) override;

  IndexScanner *create_scanner(const char *left_key, int left_len, bool left_inclusive,
      const char *right_key, int right_len, bool right_inclusive) override
  {
    // 向量索引不支持范围扫描
    return nullptr;
  }

  RC sync() override;

  // 训练索引(在插入一定数量数据后调用)
  RC train();

private:
  // 距离计算枚举
  enum class DistanceType {
    L2,
    COSINE,
    INNER_PRODUCT
  };

  // 向量+RID存储结构
  struct VectorEntry {
    std::vector<float> vector;
    RID rid;
  };

  // 聚类中心
  struct Centroid {
    std::vector<float> center;
    std::vector<VectorEntry> entries; // 该簇中的所有向量
  };

  // 距离计算函数
  float compute_distance(const std::vector<float> &v1, const std::vector<float> &v2) const;
  float l2_distance(const std::vector<float> &v1, const std::vector<float> &v2) const;
  float cosine_distance(const std::vector<float> &v1, const std::vector<float> &v2) const;
  float inner_product_distance(const std::vector<float> &v1, const std::vector<float> &v2) const;

  // K-Means聚类
  RC kmeans_clustering(const std::vector<std::vector<float>> &vectors);

  // 找到最近的簇
  std::vector<int> find_nearest_centroids(const std::vector<float> &query, int n_probes) const;

  // 解析向量数据
  std::vector<float> parse_vector(const char *record) const;

  // 持久化相关
  RC save_index();
  RC load_index();

private:
  bool   inited_ = false;
  bool   trained_ = false;
  Table *table_  = nullptr;
  int    lists_  = 245;      // 聚类簇数量
  int    probes_ = 5;        // 搜索时探测的簇数量
  int    dim_ = 0;           // 向量维度

  std::string distance_type_str_;
  DistanceType distance_type_ = DistanceType::L2;

  std::string file_name_;

  // 索引数据结构
  std::vector<Centroid> centroids_;  // 聚类中心

  // 训练前临时存储
  std::vector<VectorEntry> pending_entries_;

  // 线程安全
  mutable std::mutex mutex_;

  // 字段元信息
  std::vector<FieldMeta> field_metas_;
};
