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
#include "common/lang/string.h"
#include "common/log/log.h"
#include "storage/table/table.h"
#include "storage/record/record_manager.h"
#include "storage/record/record_scanner.h"
#include "storage/record/lob_ref.h"
#include <fstream>
#include <algorithm>
#include <random>
#include <queue>
#include <cmath>
#include <limits>

IvfflatIndex::~IvfflatIndex() noexcept
{
  close();
}

float IvfflatIndex::compute_l2_distance(const vector<float> &a, const vector<float> &b) const
{
  if (a.size() != b.size()) {
    return std::numeric_limits<float>::max();
  }

  float sum = 0.0f;
  for (size_t i = 0; i < a.size(); ++i) {
    float diff = a[i] - b[i];
    sum += diff * diff;
  }
  return std::sqrt(sum);
}

void IvfflatIndex::kmeans_clustering(const vector<vector<float>> &vectors, int k, int max_iter)
{
  if (vectors.empty() || k <= 0) {
    LOG_WARN("Invalid parameters for kmeans: vectors.size=%zu, k=%d", vectors.size(), k);
    return;
  }

  const size_t n = vectors.size();
  const size_t dim = vectors[0].size();

  // 如果向量数量少于聚类数，调整k
  if (n < static_cast<size_t>(k)) {
    k = static_cast<int>(n);
    LOG_INFO("Adjusted k to %d due to insufficient vectors", k);
  }

  // 初始化聚类中心：使用K-Means++算法
  centroids_.resize(k);
  for (int i = 0; i < k; ++i) {
    centroids_[i].resize(dim);
  }

  // 随机选择第一个中心
  std::random_device rd;
  std::mt19937 gen(rd());
  std::uniform_int_distribution<size_t> dis(0, n - 1);

  size_t first_idx = dis(gen);
  centroids_[0] = vectors[first_idx];

  // K-Means++: 选择剩余中心
  vector<float> min_distances(n, std::numeric_limits<float>::max());

  for (int i = 1; i < k; ++i) {
    // 更新每个点到最近中心的距离
    for (size_t j = 0; j < n; ++j) {
      float dist = compute_l2_distance(vectors[j], centroids_[i-1]);
      if (dist < min_distances[j]) {
        min_distances[j] = dist;
      }
    }

    // 按距离平方加权随机选择下一个中心
    float sum = 0.0f;
    for (float d : min_distances) {
      sum += d * d;
    }

    std::uniform_real_distribution<float> prob_dis(0.0f, sum);
    float target = prob_dis(gen);

    float cumsum = 0.0f;
    size_t next_idx = 0;
    for (size_t j = 0; j < n; ++j) {
      cumsum += min_distances[j] * min_distances[j];
      if (cumsum >= target) {
        next_idx = j;
        break;
      }
    }

    centroids_[i] = vectors[next_idx];
  }

  // K-Means迭代
  vector<int> assignments(n);

  for (int iter = 0; iter < max_iter; ++iter) {
    bool changed = false;

    // 分配步骤：将每个向量分配到最近的中心
    for (size_t i = 0; i < n; ++i) {
      float min_dist = std::numeric_limits<float>::max();
      int best_cluster = 0;

      for (int j = 0; j < k; ++j) {
        float dist = compute_l2_distance(vectors[i], centroids_[j]);
        if (dist < min_dist) {
          min_dist = dist;
          best_cluster = j;
        }
      }

      if (assignments[i] != best_cluster) {
        assignments[i] = best_cluster;
        changed = true;
      }
    }

    if (!changed) {
      LOG_INFO("K-Means converged at iteration %d", iter);
      break;
    }

    // 更新步骤：重新计算每个簇的中心
    vector<vector<float>> new_centroids(k, vector<float>(dim, 0.0f));
    vector<int> counts(k, 0);

    for (size_t i = 0; i < n; ++i) {
      int cluster = assignments[i];
      counts[cluster]++;
      for (size_t d = 0; d < dim; ++d) {
        new_centroids[cluster][d] += vectors[i][d];
      }
    }

    for (int j = 0; j < k; ++j) {
      if (counts[j] > 0) {
        for (size_t d = 0; d < dim; ++d) {
          new_centroids[j][d] /= counts[j];
        }
        centroids_[j] = new_centroids[j];
      }
    }
  }

  LOG_INFO("K-Means clustering completed with k=%d, iterations=%d", k, max_iter);
}

RC IvfflatIndex::extract_vector_from_record(const char *record, vector<float> &vec) const
{
  if (!vector_field_meta_) {
    return RC::INTERNAL;
  }

  const int schema_dim = vector_field_meta_->vector_length();
  const bool vector_lob =
      (schema_dim > 1000) ||
      (schema_dim <= 0 && vector_field_meta_->len() == static_cast<int>(sizeof(LobRef)));

  const char      *payload = nullptr;
  int              payload_len = 0;
  std::vector<char> tmp_buffer;

  if (vector_lob) {
    LobRef ref;
    memcpy(&ref, record + vector_field_meta_->offset(), sizeof(LobRef));
    if (ref.length <= 0 || (ref.length % static_cast<int>(sizeof(float)) != 0)) {
      LOG_WARN("Invalid LobRef length for vector column. table=%s length=%d",
               table_ != nullptr ? table_->name() : "<unknown>", ref.length);
      return RC::INTERNAL;
    }
    if (table_ == nullptr || table_->lob_handler() == nullptr) {
      LOG_WARN("Table or LOB handler unavailable when extracting vector");
      return RC::INTERNAL;
    }
    tmp_buffer.resize(static_cast<size_t>(ref.length));
    RC rc = table_->lob_handler()->get_data(ref.offset, ref.length, tmp_buffer.data());
    if (rc != RC::SUCCESS) {
      LOG_WARN("Failed to load vector LOB payload. rc=%s", strrc(rc));
      return rc;
    }
    payload     = tmp_buffer.data();
    payload_len = ref.length;
  } else {
    payload     = record + vector_field_meta_->offset();
    payload_len = vector_field_meta_->len();
  }

  if (payload_len <= 0 || (payload_len % static_cast<int>(sizeof(float)) != 0)) {
    LOG_WARN("Vector payload length is invalid: %d", payload_len);
    return RC::INTERNAL;
  }

  const int actual_dim = payload_len / static_cast<int>(sizeof(float));
  if (dimension_ > 0 && actual_dim != dimension_) {
    LOG_WARN("Vector dimension mismatch: expected=%d, got=%d", dimension_, actual_dim);
    return RC::INTERNAL;
  }

  const float *float_data = reinterpret_cast<const float *>(payload);
  vec.assign(float_data, float_data + actual_dim);

  return RC::SUCCESS;
}

void IvfflatIndex::apply_meta_config(const IndexMeta &index_meta)
{
  index_type_    = index_meta.index_type();
  distance_type_ = index_meta.distance_type();

  string index_type_upper = index_type_;
  if (!index_type_upper.empty()) {
    common::str_to_upper(index_type_upper);
  }
  if (index_type_upper.empty() || index_type_upper == "IVFFLAT") {
    index_type_ = "IVFFLAT";
  } else {
    LOG_WARN("Unsupported vector index type %s, fallback to IVFFLAT", index_type_.c_str());
    index_type_ = "IVFFLAT";
  }

  string distance_type_upper = distance_type_;
  if (!distance_type_upper.empty()) {
    common::str_to_upper(distance_type_upper);
  }
  if (distance_type_upper.empty() || distance_type_upper == "L2_DISTANCE" ||
      distance_type_upper == "L2" || distance_type_upper == "EUCLIDEAN") {
    distance_type_ = "L2_DISTANCE";
  } else if (distance_type_upper == "COSINE_DISTANCE" || distance_type_upper == "COSINE") {
    LOG_WARN("COSINE distance is not yet supported by IVF-Flat search, fallback to L2");
    distance_type_ = "L2_DISTANCE";
  } else if (distance_type_upper == "INNER_PRODUCT" || distance_type_upper == "DOT") {
    LOG_WARN("INNER_PRODUCT distance is not yet supported by IVF-Flat search, fallback to L2");
    distance_type_ = "L2_DISTANCE";
  } else {
    LOG_WARN("Unsupported distance type %s, fallback to L2_DISTANCE", distance_type_.c_str());
    distance_type_ = "L2_DISTANCE";
  }

  int configured_lists = index_meta.lists();
  if (configured_lists <= 0) {
    configured_lists = 245;
  }
  lists_ = std::max(1, configured_lists);

  int configured_probes = index_meta.probes();
  if (configured_probes <= 0) {
    configured_probes = 5;
  }
  if (configured_probes > lists_) {
    LOG_INFO("Adjust probes from %d to %d to match lists", configured_probes, lists_);
    configured_probes = lists_;
  }
  probes_ = std::max(1, configured_probes);

  LOG_INFO("IVF-Flat meta config: index_type=%s distance=%s lists=%d probes=%d",
           index_type_.c_str(), distance_type_.c_str(), lists_, probes_);
}

RC IvfflatIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas)
{
  if (inited_) {
    LOG_WARN("IvfflatIndex already initialized");
    return RC::RECORD_OPENNED;
  }

  if (field_metas.size() != 1) {
    LOG_WARN("IvfflatIndex only supports single field");
    return RC::INVALID_ARGUMENT;
  }

  Index::init(index_meta, field_metas);

  table_             = table;
  file_name_         = file_name;
  vector_field_meta_ = &field_metas[0];

  apply_meta_config(index_meta);
  int requested_lists = lists_;

  // 计算向量维度
  dimension_ = vector_field_meta_->vector_length();
  if (dimension_ <= 0) {
    const int physical_len = vector_field_meta_->len();
    if (physical_len != static_cast<int>(sizeof(LobRef)) &&
        physical_len % static_cast<int>(sizeof(float)) == 0) {
      dimension_ = physical_len / static_cast<int>(sizeof(float));
    }
  }
  if (dimension_ > 0) {
    LOG_INFO("Creating IVF-Flat index with dimension=%d", dimension_);
  } else {
    LOG_INFO("Creating IVF-Flat index with unknown dimension, will infer from data");
  }

  // 扫描表，收集所有向量数据
  vector<vector<float>> all_vectors;
  vector<RID> all_rids;

  RecordScanner *scanner = nullptr;
  RC rc = table_->get_record_scanner(scanner, nullptr, ReadWriteMode::READ_ONLY);
  if (rc != RC::SUCCESS || !scanner) {
    LOG_WARN("Failed to create record scanner");
    return RC::INTERNAL;
  }

  Record record;
  while (RC::SUCCESS == scanner->next(record)) {
    vector<float> vec;
    RC extract_rc = extract_vector_from_record(record.data(), vec);
    if (RC::SUCCESS == extract_rc) {
      all_vectors.push_back(vec);
      all_rids.push_back(record.rid());
    }
  }
  scanner->close_scan();
  delete scanner;

  LOG_INFO("Collected %zu vectors from table", all_vectors.size());

  if (all_vectors.empty()) {
    LOG_INFO("No vectors found in table, initialize empty IVF-Flat index");

    if (dimension_ <= 0) {
      const int physical_len = vector_field_meta_->len();
      if (physical_len > 0 && physical_len % static_cast<int>(sizeof(float)) == 0) {
        dimension_ = physical_len / static_cast<int>(sizeof(float));
      }
    }
    if (dimension_ <= 0) {
      LOG_WARN("Cannot infer positive dimension for empty IVF-Flat index");
      return RC::INVALID_ARGUMENT;
    }

    lists_  = std::max(1, lists_);
    probes_ = std::max(1, std::min(probes_, lists_));

    centroids_.assign(lists_, std::vector<float>(dimension_, 0.0f));
    inverted_lists_.assign(lists_, std::vector<IvfEntry>());

    RC save_rc = save_to_file();
    if (save_rc != RC::SUCCESS) {
      LOG_WARN("Failed to save empty IVF-Flat index to file");
      return save_rc;
    }

    inited_ = true;
    LOG_INFO("Empty IVF-Flat index initialized with dimension=%d lists=%d probes=%d",
             dimension_, lists_, probes_);
    return RC::SUCCESS;
  }

  if (dimension_ <= 0) {
    dimension_ = static_cast<int>(all_vectors.front().size());
    LOG_INFO("Infer IVF-Flat dimension from data: %d", dimension_);
    if (dimension_ <= 0) {
      LOG_WARN("Failed to infer positive vector dimension for IVF-Flat index");
      return RC::INVALID_ARGUMENT;
    }
  }

  // 执行K-Means聚类
  int actual_lists = std::min(lists_, static_cast<int>(all_vectors.size()));
  if (actual_lists <= 0) {
    actual_lists = std::min(static_cast<int>(all_vectors.size()), 1);
  }
  probes_ = std::max(1, std::min(probes_, actual_lists));
  lists_  = actual_lists;
  LOG_INFO("Finalize IVF-Flat index config: requested_lists=%d actual_lists=%d probes=%d",
           requested_lists, lists_, probes_);
  kmeans_clustering(all_vectors, actual_lists, 100);

  // 初始化倒排列表
  inverted_lists_.resize(actual_lists);

  // 将向量分配到对应的倒排列表
  for (size_t i = 0; i < all_vectors.size(); ++i) {
    float min_dist = std::numeric_limits<float>::max();
    int best_cluster = 0;

    for (int j = 0; j < actual_lists; ++j) {
      float dist = compute_l2_distance(all_vectors[i], centroids_[j]);
      if (dist < min_dist) {
        min_dist = dist;
        best_cluster = j;
      }
    }

    inverted_lists_[best_cluster].emplace_back(all_rids[i], all_vectors[i]);
  }

  // 保存索引到文件
  RC save_rc = save_to_file();
  if (save_rc != RC::SUCCESS) {
    LOG_WARN("Failed to save index to file");
    return save_rc;
  }

  inited_ = true;
  LOG_INFO("Successfully created IVF-Flat index, file=%s", file_name_.c_str());
  return RC::SUCCESS;
}

RC IvfflatIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas)
{
  if (inited_) {
    LOG_WARN("IvfflatIndex already initialized");
    return RC::RECORD_OPENNED;
  }

  if (field_metas.size() != 1) {
    LOG_WARN("IvfflatIndex only supports single field");
    return RC::INVALID_ARGUMENT;
  }

  Index::init(index_meta, field_metas);

  table_ = table;
  file_name_ = file_name;
  vector_field_meta_ = &field_metas[0];
  apply_meta_config(index_meta);

  // 从文件加载索引
  RC rc = load_from_file();
  if (rc != RC::SUCCESS) {
    LOG_WARN("Failed to load index from file: %s", file_name_.c_str());
    return rc;
  }

  probes_ = std::max(1, std::min(probes_, std::max(1, lists_)));
  LOG_INFO("Open IVF-Flat index success: lists=%d probes=%d", lists_, probes_);

  inited_ = true;
  LOG_INFO("Successfully opened IVF-Flat index, file=%s", file_name_.c_str());
  return RC::SUCCESS;
}

RC IvfflatIndex::close()
{
  if (inited_) {
    LOG_INFO("Closing IVF-Flat index");
    centroids_.clear();
    inverted_lists_.clear();
    inited_ = false;
  }
  return RC::SUCCESS;
}

RC IvfflatIndex::insert_entry(const char *record, const RID *rid)
{
  if (!inited_) {
    return RC::INTERNAL;
  }

  // 提取向量
  vector<float> vec;
  RC rc = extract_vector_from_record(record, vec);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  // 找到最近的聚类中心
  if (centroids_.empty()) {
    LOG_WARN("Centroids missing before insert, rebuild default clusters");

    if (dimension_ <= 0) {
      LOG_WARN("Invalid dimension while rebuilding centroids");
      return RC::INTERNAL;
    }

    lists_  = std::max(1, lists_);
    probes_ = std::max(1, std::min(probes_, lists_));

    centroids_.assign(lists_, std::vector<float>(dimension_, 0.0f));
    inverted_lists_.assign(lists_, std::vector<IvfEntry>());
  }

  float min_dist = std::numeric_limits<float>::max();
  int best_cluster = 0;

  for (size_t i = 0; i < centroids_.size(); ++i) {
    float dist = compute_l2_distance(vec, centroids_[i]);
    if (dist < min_dist) {
      min_dist = dist;
      best_cluster = static_cast<int>(i);
    }
  }

  // 插入到倒排列表
  inverted_lists_[best_cluster].emplace_back(*rid, vec);

  return RC::SUCCESS;
}

RC IvfflatIndex::delete_entry(const char *record, const RID *rid)
{
  // 暂不实现删除功能
  return RC::UNIMPLEMENTED;
}

vector<int> IvfflatIndex::find_nearest_clusters(const vector<float> &query_vector, int n) const
{
  if (centroids_.empty()) {
    return {};
  }

  // 计算到所有聚类中心的距离
  vector<pair<float, int>> distances;
  distances.reserve(centroids_.size());

  for (size_t i = 0; i < centroids_.size(); ++i) {
    float dist = compute_l2_distance(query_vector, centroids_[i]);
    distances.emplace_back(dist, static_cast<int>(i));
  }

  // 排序并返回最近的n个
  std::sort(distances.begin(), distances.end());

  int actual_n = std::min(n, static_cast<int>(distances.size()));
  vector<int> result;
  result.reserve(actual_n);

  for (int i = 0; i < actual_n; ++i) {
    result.push_back(distances[i].second);
  }

  return result;
}

vector<RID> IvfflatIndex::ann_search(const vector<float> &query_vector, size_t limit)
{
  if (!inited_ || centroids_.empty()) {
    LOG_WARN("Index not initialized or no centroids");
    return {};
  }

  if (query_vector.size() != static_cast<size_t>(dimension_)) {
    LOG_WARN("Query vector dimension mismatch: expected=%d, got=%zu", dimension_, query_vector.size());
    return {};
  }

  // 找到最近的probes_个聚类中心
  vector<int> nearest_clusters = find_nearest_clusters(query_vector, probes_);

  // 使用优先队列维护Top-K结果 (最大堆)
  auto cmp = [](const pair<float, RID> &a, const pair<float, RID> &b) {
    return a.first < b.first;  // 最大堆：距离大的在堆顶
  };
  std::priority_queue<pair<float, RID>, vector<pair<float, RID>>, decltype(cmp)> top_k(cmp);

  // 在选中的簇中进行精确搜索
  for (int cluster_id : nearest_clusters) {
    if (cluster_id < 0 || cluster_id >= static_cast<int>(inverted_lists_.size())) {
      continue;
    }

    const auto &entries = inverted_lists_[cluster_id];
    for (const auto &entry : entries) {
      float dist = compute_l2_distance(query_vector, entry.vector_data);

      if (top_k.size() < limit) {
        top_k.emplace(dist, entry.rid);
      } else if (dist < top_k.top().first) {
        top_k.pop();
        top_k.emplace(dist, entry.rid);
      }
    }
  }

  // 提取结果（需要反转顺序，因为是最大堆）
  vector<RID> results;
  results.reserve(top_k.size());

  while (!top_k.empty()) {
    results.push_back(top_k.top().second);
    top_k.pop();
  }

  // 反转以得到距离从小到大的顺序
  std::reverse(results.begin(), results.end());

  return results;
}

bool IvfflatIndex::ready() const
{
  return inited_ && dimension_ > 0 && !centroids_.empty();
}

RC IvfflatIndex::save_to_file()
{
  std::ofstream ofs(file_name_, std::ios::binary);
  if (!ofs.is_open()) {
    LOG_WARN("Failed to open file for writing: %s", file_name_.c_str());
    return RC::IOERR_OPEN;
  }

  // 写入索引元数据
  ofs.write(reinterpret_cast<const char *>(&lists_), sizeof(lists_));
  ofs.write(reinterpret_cast<const char *>(&probes_), sizeof(probes_));
  ofs.write(reinterpret_cast<const char *>(&dimension_), sizeof(dimension_));

  // 写入聚类中心数量
  int num_centroids = static_cast<int>(centroids_.size());
  ofs.write(reinterpret_cast<const char *>(&num_centroids), sizeof(num_centroids));

  // 写入每个聚类中心
  for (const auto &centroid : centroids_) {
    ofs.write(reinterpret_cast<const char *>(centroid.data()), centroid.size() * sizeof(float));
  }

  // 写入倒排列表
  for (const auto &list : inverted_lists_) {
    int list_size = static_cast<int>(list.size());
    ofs.write(reinterpret_cast<const char *>(&list_size), sizeof(list_size));

    for (const auto &entry : list) {
      // 写入RID
      ofs.write(reinterpret_cast<const char *>(&entry.rid), sizeof(RID));
      // 写入向量数据
      ofs.write(reinterpret_cast<const char *>(entry.vector_data.data()),
                entry.vector_data.size() * sizeof(float));
    }
  }

  ofs.close();

  if (!ofs) {
    LOG_WARN("Error occurred while writing index file");
    return RC::IOERR_WRITE;
  }

  LOG_INFO("Index saved to file: %s", file_name_.c_str());
  return RC::SUCCESS;
}

RC IvfflatIndex::load_from_file()
{
  std::ifstream ifs(file_name_, std::ios::binary);
  if (!ifs.is_open()) {
    LOG_WARN("Failed to open file for reading: %s", file_name_.c_str());
    return RC::IOERR_OPEN;
  }

  // 读取索引元数据
  ifs.read(reinterpret_cast<char *>(&lists_), sizeof(lists_));
  ifs.read(reinterpret_cast<char *>(&probes_), sizeof(probes_));
  ifs.read(reinterpret_cast<char *>(&dimension_), sizeof(dimension_));

  // 读取聚类中心数量
  int num_centroids = 0;
  ifs.read(reinterpret_cast<char *>(&num_centroids), sizeof(num_centroids));

  // 读取每个聚类中心
  centroids_.resize(num_centroids);
  for (int i = 0; i < num_centroids; ++i) {
    centroids_[i].resize(dimension_);
    ifs.read(reinterpret_cast<char *>(centroids_[i].data()), dimension_ * sizeof(float));
  }

  // 读取倒排列表
  inverted_lists_.resize(num_centroids);
  for (int i = 0; i < num_centroids; ++i) {
    int list_size = 0;
    ifs.read(reinterpret_cast<char *>(&list_size), sizeof(list_size));

    inverted_lists_[i].reserve(list_size);
    for (int j = 0; j < list_size; ++j) {
      RID rid;
      ifs.read(reinterpret_cast<char *>(&rid), sizeof(RID));

      vector<float> vec(dimension_);
      ifs.read(reinterpret_cast<char *>(vec.data()), dimension_ * sizeof(float));

      inverted_lists_[i].emplace_back(rid, vec);
    }
  }

  ifs.close();

  if (!ifs) {
    LOG_WARN("Error occurred while reading index file");
    return RC::IOERR_READ;
  }

  LOG_INFO("Index loaded from file: %s, centroids=%d, dimension=%d",
           file_name_.c_str(), num_centroids, dimension_);
  return RC::SUCCESS;
}

RC IvfflatIndex::sync()
{
  if (!inited_) {
    return RC::SUCCESS;
  }

  return save_to_file();
}

IndexScanner *IvfflatIndex::create_scanner(const char *left_key, int left_len, bool left_inclusive,
                                            const char *right_key, int right_len, bool right_inclusive)
{
  // 向量索引不支持范围扫描
  LOG_WARN("IvfflatIndex does not support range scanning");
  return nullptr;
}
