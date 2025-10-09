/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/index/ivfflat_index.h"
#include "common/log/log.h"
#include "storage/table/table.h"
#include "storage/field/field_meta.h"
#include <fstream>
#include <cstring>
#include <limits>
#include <cassert>

IvfflatIndex::~IvfflatIndex() noexcept
{
  close();
}

RC IvfflatIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas)
{
  if (table == nullptr || file_name == nullptr || field_metas.empty()) {
    LOG_WARN("Invalid parameters for creating ivfflat index");
    return RC::INVALID_ARGUMENT;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  table_ = table;
  file_name_ = file_name;

  // 获取向量索引参数
  lists_ = index_meta.lists();
  probes_ = index_meta.probes();
  distance_type_str_ = index_meta.distance_type();

  // 设置距离类型
  if (strcasecmp(distance_type_str_.c_str(), "L2_DISTANCE") == 0) {
    distance_type_ = DistanceType::L2;
  } else if (strcasecmp(distance_type_str_.c_str(), "COSINE_DISTANCE") == 0) {
    distance_type_ = DistanceType::COSINE;
  } else if (strcasecmp(distance_type_str_.c_str(), "INNER_PRODUCT") == 0) {
    distance_type_ = DistanceType::INNER_PRODUCT;
  } else {
    distance_type_ = DistanceType::L2; // 默认L2距离
  }

  // 保存字段元信息
  field_metas_.assign(field_metas.begin(), field_metas.end());

  // 获取向量维度
  if (!field_metas.empty() && field_metas[0].type() == AttrType::VECTORS) {
    dim_ = field_metas[0].len() / sizeof(float);
  }

  inited_ = true;
  trained_ = false;

  LOG_INFO("Created ivfflat index with lists=%d, probes=%d, dim=%d, distance=%s",
           lists_, probes_, dim_, distance_type_str_.c_str());

  return RC::SUCCESS;
}

RC IvfflatIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas)
{
  // 先创建索引结构
  RC rc = create(table, file_name, index_meta, field_metas);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  // 尝试加载已有索引
  rc = load_index();
  if (rc == RC::SUCCESS) {
    LOG_INFO("Loaded existing ivfflat index from file: %s", file_name);
  } else {
    LOG_INFO("No existing index file found, will build new index");
  }

  return RC::SUCCESS;
}

RC IvfflatIndex::close()
{
  std::lock_guard<std::mutex> lock(mutex_);

  if (inited_ && trained_) {
    save_index();
  }

  centroids_.clear();
  pending_entries_.clear();
  inited_ = false;
  trained_ = false;

  return RC::SUCCESS;
}

RC IvfflatIndex::insert_entry(const char *record, const RID *rid)
{
  if (!inited_ || record == nullptr || rid == nullptr) {
    return RC::INVALID_ARGUMENT;
  }

  std::vector<float> vector = parse_vector(record);
  if (vector.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  VectorEntry entry;
  entry.vector = std::move(vector);
  entry.rid = *rid;

  if (!trained_) {
    // 索引未训练,先暂存
    pending_entries_.push_back(std::move(entry));

    // 积累足够数据后自动训练
    if (pending_entries_.size() >= static_cast<size_t>(lists_ * 10)) {
      LOG_INFO("Auto-training index with %zu entries", pending_entries_.size());
      return train();
    }
  } else {
    // 已训练,直接插入到最近的簇
    float min_dist = std::numeric_limits<float>::max();
    int best_cluster = 0;

    for (size_t i = 0; i < centroids_.size(); i++) {
      float dist = compute_distance(entry.vector, centroids_[i].center);
      if (dist < min_dist) {
        min_dist = dist;
        best_cluster = i;
      }
    }

    centroids_[best_cluster].entries.push_back(std::move(entry));
  }

  return RC::SUCCESS;
}

RC IvfflatIndex::delete_entry(const char *record, const RID *rid)
{
  // 向量索引的删除暂不实现
  return RC::UNIMPLEMENTED;
}

RC IvfflatIndex::sync()
{
  if (!inited_) {
    return RC::SUCCESS;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (trained_) {
    return save_index();
  }

  return RC::SUCCESS;
}

RC IvfflatIndex::train()
{
  if (pending_entries_.empty()) {
    LOG_WARN("No data to train ivfflat index");
    return RC::SUCCESS;
  }

  LOG_INFO("Training ivfflat index with %zu vectors", pending_entries_.size());

  // 提取所有向量
  std::vector<std::vector<float>> vectors;
  vectors.reserve(pending_entries_.size());
  for (const auto &entry : pending_entries_) {
    vectors.push_back(entry.vector);
  }

  // 执行K-Means聚类
  RC rc = kmeans_clustering(vectors);
  if (rc != RC::SUCCESS) {
    LOG_WARN("K-Means clustering failed");
    return rc;
  }

  // 将待处理的数据分配到各个簇
  for (auto &entry : pending_entries_) {
    float min_dist = std::numeric_limits<float>::max();
    int best_cluster = 0;

    for (size_t i = 0; i < centroids_.size(); i++) {
      float dist = compute_distance(entry.vector, centroids_[i].center);
      if (dist < min_dist) {
        min_dist = dist;
        best_cluster = i;
      }
    }

    centroids_[best_cluster].entries.push_back(std::move(entry));
  }

  pending_entries_.clear();
  trained_ = true;

  LOG_INFO("Ivfflat index training completed with %zu centroids", centroids_.size());

  return RC::SUCCESS;
}

std::vector<RID> IvfflatIndex::ann_search(const std::vector<float> &query_vector, size_t limit)
{
  std::vector<RID> results;

  if (!inited_ || query_vector.size() != static_cast<size_t>(dim_)) {
    LOG_WARN("Invalid query vector or index not initialized");
    return results;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // 如果索引未训练,使用暴力搜索
  if (!trained_) {
    if (pending_entries_.empty()) {
      return results;
    }

    // 暴力搜索
    using CandidatePair = std::pair<float, RID>;
    std::priority_queue<CandidatePair> candidates;

    for (const auto &entry : pending_entries_) {
      float dist = compute_distance(query_vector, entry.vector);
      candidates.push({dist, entry.rid});

      if (candidates.size() > limit) {
        candidates.pop();
      }
    }

    // 提取结果(需要反转顺序)
    results.reserve(candidates.size());
    while (!candidates.empty()) {
      results.push_back(candidates.top().second);
      candidates.pop();
    }
    std::reverse(results.begin(), results.end());

    return results;
  }

  // 使用IVF索引搜索
  // 1. 找到最近的n_probes个簇
  std::vector<int> nearest_clusters = find_nearest_centroids(query_vector, probes_);

  // 2. 在这些簇中搜索最近的向量
  using CandidatePair = std::pair<float, RID>;
  std::priority_queue<CandidatePair> candidates;

  for (int cluster_id : nearest_clusters) {
    if (cluster_id < 0 || cluster_id >= static_cast<int>(centroids_.size())) {
      continue;
    }

    const auto &cluster = centroids_[cluster_id];
    for (const auto &entry : cluster.entries) {
      float dist = compute_distance(query_vector, entry.vector);
      candidates.push({dist, entry.rid});

      if (candidates.size() > limit) {
        candidates.pop();
      }
    }
  }

  // 3. 提取结果(需要反转顺序,因为priority_queue是大顶堆)
  results.reserve(candidates.size());
  while (!candidates.empty()) {
    results.push_back(candidates.top().second);
    candidates.pop();
  }
  std::reverse(results.begin(), results.end());

  return results;
}

// ============= 私有辅助函数实现 =============

float IvfflatIndex::compute_distance(const std::vector<float> &v1, const std::vector<float> &v2) const
{
  switch (distance_type_) {
    case DistanceType::L2:
      return l2_distance(v1, v2);
    case DistanceType::COSINE:
      return cosine_distance(v1, v2);
    case DistanceType::INNER_PRODUCT:
      return inner_product_distance(v1, v2);
    default:
      return l2_distance(v1, v2);
  }
}

float IvfflatIndex::l2_distance(const std::vector<float> &v1, const std::vector<float> &v2) const
{
  assert(v1.size() == v2.size());

  float sum = 0.0f;
  for (size_t i = 0; i < v1.size(); i++) {
    float diff = v1[i] - v2[i];
    sum += diff * diff;
  }
  return std::sqrt(sum);
}

float IvfflatIndex::cosine_distance(const std::vector<float> &v1, const std::vector<float> &v2) const
{
  assert(v1.size() == v2.size());

  float dot_product = 0.0f;
  float norm1 = 0.0f;
  float norm2 = 0.0f;

  for (size_t i = 0; i < v1.size(); i++) {
    dot_product += v1[i] * v2[i];
    norm1 += v1[i] * v1[i];
    norm2 += v2[i] * v2[i];
  }

  norm1 = std::sqrt(norm1);
  norm2 = std::sqrt(norm2);

  if (norm1 < 1e-10f || norm2 < 1e-10f) {
    return 1.0f;  // 零向量,返回最大距离
  }

  float cosine_sim = dot_product / (norm1 * norm2);
  return 1.0f - cosine_sim;
}

float IvfflatIndex::inner_product_distance(const std::vector<float> &v1, const std::vector<float> &v2) const
{
  assert(v1.size() == v2.size());

  float dot_product = 0.0f;
  for (size_t i = 0; i < v1.size(); i++) {
    dot_product += v1[i] * v2[i];
  }

  // 内积越大越相似,所以返回负值作为"距离"
  return -dot_product;
}

RC IvfflatIndex::kmeans_clustering(const std::vector<std::vector<float>> &vectors)
{
  if (vectors.empty() || vectors[0].empty()) {
    return RC::INVALID_ARGUMENT;
  }

  const int n_vectors = vectors.size();
  const int n_clusters = std::min(lists_, n_vectors); // 簇数不能超过向量数
  const int max_iterations = 100;

  // 初始化聚类中心(使用K-Means++算法)
  centroids_.clear();
  centroids_.resize(n_clusters);

  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<> dis(0, n_vectors - 1);

  // 第一个中心随机选择
  int first_idx = dis(gen);
  centroids_[0].center = vectors[first_idx];

  // 后续中心使用K-Means++选择
  for (int k = 1; k < n_clusters; k++) {
    std::vector<float> min_distances(n_vectors, std::numeric_limits<float>::max());

    // 计算每个点到最近中心的距离
    for (int i = 0; i < n_vectors; i++) {
      for (int j = 0; j < k; j++) {
        float dist = compute_distance(vectors[i], centroids_[j].center);
        min_distances[i] = std::min(min_distances[i], dist);
      }
    }

    // 以距离的平方为权重随机选择下一个中心
    std::vector<float> weights(n_vectors);
    float sum_weights = 0.0f;
    for (int i = 0; i < n_vectors; i++) {
      weights[i] = min_distances[i] * min_distances[i];
      sum_weights += weights[i];
    }

    if (sum_weights < 1e-10f) {
      // 所有点都已经被覆盖,随机选择剩余中心
      for (int j = k; j < n_clusters; j++) {
        centroids_[j].center = vectors[dis(gen)];
      }
      break;
    }

    // 使用轮盘赌选择
    std::uniform_real_distribution<float> uniform(0.0f, sum_weights);
    float r = uniform(gen);
    float cumsum = 0.0f;
    int selected_idx = 0;
    for (int i = 0; i < n_vectors; i++) {
      cumsum += weights[i];
      if (cumsum >= r) {
        selected_idx = i;
        break;
      }
    }

    centroids_[k].center = vectors[selected_idx];
  }

  // K-Means迭代
  std::vector<int> assignments(n_vectors, -1);
  bool changed = true;
  int iteration = 0;

  while (changed && iteration < max_iterations) {
    changed = false;
    iteration++;

    // E步:分配每个向量到最近的簇
    for (int i = 0; i < n_vectors; i++) {
      float min_dist = std::numeric_limits<float>::max();
      int best_cluster = 0;

      for (int k = 0; k < n_clusters; k++) {
        float dist = compute_distance(vectors[i], centroids_[k].center);
        if (dist < min_dist) {
          min_dist = dist;
          best_cluster = k;
        }
      }

      if (assignments[i] != best_cluster) {
        assignments[i] = best_cluster;
        changed = true;
      }
    }

    // M步:更新聚类中心
    for (int k = 0; k < n_clusters; k++) {
      std::vector<float> new_center(dim_, 0.0f);
      int count = 0;

      for (int i = 0; i < n_vectors; i++) {
        if (assignments[i] == k) {
          for (int d = 0; d < dim_; d++) {
            new_center[d] += vectors[i][d];
          }
          count++;
        }
      }

      if (count > 0) {
        for (int d = 0; d < dim_; d++) {
          new_center[d] /= count;
        }
        centroids_[k].center = std::move(new_center);
      }
    }

    // 检查收敛
    if (iteration > 10 && !changed) {
      break;
    }
  }

  LOG_INFO("K-Means completed after %d iterations", iteration);

  return RC::SUCCESS;
}

std::vector<int> IvfflatIndex::find_nearest_centroids(const std::vector<float> &query, int n_probes) const
{
  std::vector<int> result;

  if (centroids_.empty()) {
    return result;
  }

  // 计算到所有中心的距离
  using DistPair = std::pair<float, int>;
  std::vector<DistPair> distances;
  distances.reserve(centroids_.size());

  for (size_t i = 0; i < centroids_.size(); i++) {
    float dist = compute_distance(query, centroids_[i].center);
    distances.push_back({dist, static_cast<int>(i)});
  }

  // 部分排序,找到前n_probes个最近的
  int k = std::min(n_probes, static_cast<int>(distances.size()));
  std::partial_sort(distances.begin(), distances.begin() + k, distances.end(),
                    [](const DistPair &a, const DistPair &b) { return a.first < b.first; });

  result.reserve(k);
  for (int i = 0; i < k; i++) {
    result.push_back(distances[i].second);
  }

  return result;
}

std::vector<float> IvfflatIndex::parse_vector(const char *record) const
{
  std::vector<float> result;

  if (record == nullptr || field_metas_.empty()) {
    return result;
  }

  // 获取向量字段在记录中的偏移
  const FieldMeta &field_meta = field_metas_[0];
  int offset = field_meta.offset();
  int length = field_meta.len();

  // 向量数据直接按float数组存储
  const float *data = reinterpret_cast<const float *>(record + offset);
  int vector_size = length / sizeof(float);

  result.assign(data, data + vector_size);

  return result;
}

RC IvfflatIndex::save_index()
{
  if (file_name_.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  std::string index_file = file_name_ + ".ivfflat";
  std::ofstream ofs(index_file, std::ios::binary);
  if (!ofs.is_open()) {
    LOG_WARN("Failed to open index file for writing: %s", index_file.c_str());
    return RC::IOERR_WRITE;
  }

  try {
    // 写入元数据
    ofs.write(reinterpret_cast<const char *>(&trained_), sizeof(trained_));
    ofs.write(reinterpret_cast<const char *>(&dim_), sizeof(dim_));
    int n_centroids = centroids_.size();
    ofs.write(reinterpret_cast<const char *>(&n_centroids), sizeof(n_centroids));

    // 写入每个簇的数据
    for (const auto &centroid : centroids_) {
      // 写入中心向量
      int center_size = centroid.center.size();
      ofs.write(reinterpret_cast<const char *>(&center_size), sizeof(center_size));
      ofs.write(reinterpret_cast<const char *>(centroid.center.data()), center_size * sizeof(float));

      // 写入该簇的向量条目
      int n_entries = centroid.entries.size();
      ofs.write(reinterpret_cast<const char *>(&n_entries), sizeof(n_entries));

      for (const auto &entry : centroid.entries) {
        // 写入RID
        ofs.write(reinterpret_cast<const char *>(&entry.rid), sizeof(RID));

        // 写入向量
        int vec_size = entry.vector.size();
        ofs.write(reinterpret_cast<const char *>(&vec_size), sizeof(vec_size));
        ofs.write(reinterpret_cast<const char *>(entry.vector.data()), vec_size * sizeof(float));
      }
    }

    ofs.close();
    LOG_INFO("Successfully saved ivfflat index to %s", index_file.c_str());
    return RC::SUCCESS;

  } catch (const std::exception &e) {
    LOG_WARN("Exception while saving index: %s", e.what());
    return RC::IOERR_WRITE;
  }
}

RC IvfflatIndex::load_index()
{
  if (file_name_.empty()) {
    return RC::INVALID_ARGUMENT;
  }

  std::string index_file = file_name_ + ".ivfflat";
  std::ifstream ifs(index_file, std::ios::binary);
  if (!ifs.is_open()) {
    return RC::IOERR_READ;
  }

  try {
    // 读取元数据
    ifs.read(reinterpret_cast<char *>(&trained_), sizeof(trained_));
    ifs.read(reinterpret_cast<char *>(&dim_), sizeof(dim_));
    int n_centroids = 0;
    ifs.read(reinterpret_cast<char *>(&n_centroids), sizeof(n_centroids));

    centroids_.clear();
    centroids_.resize(n_centroids);

    // 读取每个簇的数据
    for (int i = 0; i < n_centroids; i++) {
      // 读取中心向量
      int center_size = 0;
      ifs.read(reinterpret_cast<char *>(&center_size), sizeof(center_size));
      centroids_[i].center.resize(center_size);
      ifs.read(reinterpret_cast<char *>(centroids_[i].center.data()), center_size * sizeof(float));

      // 读取该簇的向量条目
      int n_entries = 0;
      ifs.read(reinterpret_cast<char *>(&n_entries), sizeof(n_entries));
      centroids_[i].entries.reserve(n_entries);

      for (int j = 0; j < n_entries; j++) {
        VectorEntry entry;

        // 读取RID
        ifs.read(reinterpret_cast<char *>(&entry.rid), sizeof(RID));

        // 读取向量
        int vec_size = 0;
        ifs.read(reinterpret_cast<char *>(&vec_size), sizeof(vec_size));
        entry.vector.resize(vec_size);
        ifs.read(reinterpret_cast<char *>(entry.vector.data()), vec_size * sizeof(float));

        centroids_[i].entries.push_back(std::move(entry));
      }
    }

    ifs.close();
    LOG_INFO("Successfully loaded ivfflat index from %s", index_file.c_str());
    return RC::SUCCESS;

  } catch (const std::exception &e) {
    LOG_WARN("Exception while loading index: %s", e.what());
    return RC::IOERR_READ;
  }
}
