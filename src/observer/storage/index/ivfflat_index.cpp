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
#include <cmath>
#include <limits>
#include <atomic>
#include <thread>
#include <mutex>
#include <iterator>
#include <numeric>
#include <cstring>

namespace {

inline float l2_squared_unrolled(const float *a, const float *b, size_t dim)
{
  size_t i     = 0;
  size_t bound = dim & ~static_cast<size_t>(3);

  float sum0 = 0.0f;
  float sum1 = 0.0f;
  float sum2 = 0.0f;
  float sum3 = 0.0f;

  for (; i < bound; i += 4) {
    float diff0 = a[i] - b[i];
    float diff1 = a[i + 1] - b[i + 1];
    float diff2 = a[i + 2] - b[i + 2];
    float diff3 = a[i + 3] - b[i + 3];

    sum0 += diff0 * diff0;
    sum1 += diff1 * diff1;
    sum2 += diff2 * diff2;
    sum3 += diff3 * diff3;
  }

  float total = (sum0 + sum1) + (sum2 + sum3);

  for (; i < dim; ++i) {
    float diff = a[i] - b[i];
    total += diff * diff;
  }

  return total;
}

inline float l2_squared_with_cap(const float *a, const float *b, size_t dim, float cap)
{
  if (cap <= 0.0f) {
    return 0.0f;
  }

  const bool check_cap = cap < std::numeric_limits<float>::max();
  float       total     = 0.0f;
  size_t      i         = 0;
  size_t      bound     = dim & ~static_cast<size_t>(3);

  for (; i < bound; i += 4) {
    float diff0 = a[i] - b[i];
    total += diff0 * diff0;
    if (check_cap && total >= cap) {
      return total;
    }

    float diff1 = a[i + 1] - b[i + 1];
    total += diff1 * diff1;
    if (check_cap && total >= cap) {
      return total;
    }

    float diff2 = a[i + 2] - b[i + 2];
    total += diff2 * diff2;
    if (check_cap && total >= cap) {
      return total;
    }

    float diff3 = a[i + 3] - b[i + 3];
    total += diff3 * diff3;
    if (check_cap && total >= cap) {
      return total;
    }
  }

  for (; i < dim; ++i) {
    float diff = a[i] - b[i];
    total += diff * diff;
    if (check_cap && total >= cap) {
      return total;
    }
  }

  return total;
}

inline float dot_product_unrolled(const float *a, const float *b, size_t dim)
{
  size_t i     = 0;
  size_t bound = dim & ~static_cast<size_t>(7);

  float sum0 = 0.0f;
  float sum1 = 0.0f;
  float sum2 = 0.0f;
  float sum3 = 0.0f;
  float sum4 = 0.0f;
  float sum5 = 0.0f;
  float sum6 = 0.0f;
  float sum7 = 0.0f;

  for (; i < bound; i += 8) {
    sum0 += a[i] * b[i];
    sum1 += a[i + 1] * b[i + 1];
    sum2 += a[i + 2] * b[i + 2];
    sum3 += a[i + 3] * b[i + 3];
    sum4 += a[i + 4] * b[i + 4];
    sum5 += a[i + 5] * b[i + 5];
    sum6 += a[i + 6] * b[i + 6];
    sum7 += a[i + 7] * b[i + 7];
  }

  float total = ((sum0 + sum1) + (sum2 + sum3)) + ((sum4 + sum5) + (sum6 + sum7));

  for (; i < dim; ++i) {
    total += a[i] * b[i];
  }

  return total;
}

}  // namespace

namespace {

thread_local std::vector<char> g_lob_tmp_buffer;

}

IvfflatIndex::~IvfflatIndex() noexcept
{
  close();
}

float IvfflatIndex::compute_l2_distance(const vector<float> &a, const vector<float> &b) const
{
  const size_t dim = a.size();
  if (dim != b.size()) {
    return std::numeric_limits<float>::max();
  }

  float dist_sq = l2_squared_unrolled(a.data(), b.data(), dim);
  return std::sqrt(dist_sq);
}

float IvfflatIndex::compute_l2_squared(const vector<float> &a, const vector<float> &b) const
{
  const size_t dim = a.size();
  if (dim != b.size()) {
    return std::numeric_limits<float>::max();
  }

  return l2_squared_unrolled(a.data(), b.data(), dim);
}

void IvfflatIndex::kmeans_clustering(const vector<vector<float>> &vectors, int k, int max_iter)
{
  if (vectors.empty() || k <= 0) {
    LOG_WARN("Invalid parameters for kmeans: vectors.size=%zu, k=%d", vectors.size(), k);
    return;
  }

  const size_t n   = vectors.size();
  const size_t dim = vectors[0].size();
  if (dim == 0) {
    LOG_WARN("Vector dimension is zero when running kmeans");
    return;
  }

  const size_t MINI_BATCH_THRESHOLD = 15000;
  bool         use_mini_batch       = (n > MINI_BATCH_THRESHOLD);

  if (n < static_cast<size_t>(k)) {
    k = static_cast<int>(n);
    LOG_INFO("Adjusted k to %d due to insufficient vectors", k);
  }

  centroids_.assign(k, vector<float>(dim, 0.0f));

  std::random_device rd;
  std::mt19937        gen(rd());
  std::uniform_int_distribution<size_t> dis(0, n - 1);

  size_t first_idx = dis(gen);
  centroids_[0]     = vectors[first_idx];

  vector<float> min_distances(n, std::numeric_limits<float>::max());

  const size_t max_threads       = static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency()));
  const size_t init_worker_count = std::max<size_t>(1, std::min(max_threads, n));
  const size_t init_block_size   = (n + init_worker_count - 1) / init_worker_count;

  for (int i = 1; i < k; ++i) {
    const float *prev_centroid = centroids_[i - 1].data();
    auto         update_distances = [&](size_t start_idx, size_t end_idx) {
      for (size_t j = start_idx; j < end_idx; ++j) {
        float dist_sq = l2_squared_unrolled(vectors[j].data(), prev_centroid, dim);
        if (dist_sq < min_distances[j]) {
          min_distances[j] = dist_sq;
        }
      }
    };

    if (init_worker_count == 1) {
      update_distances(0, n);
    } else {
      std::vector<std::thread> workers;
      workers.reserve(init_worker_count);
      for (size_t t = 0; t < init_worker_count; ++t) {
        size_t start_idx = t * init_block_size;
        if (start_idx >= n) {
          break;
        }
        size_t end_idx = std::min(start_idx + init_block_size, n);
        workers.emplace_back(update_distances, start_idx, end_idx);
      }
      for (auto &worker : workers) {
        if (worker.joinable()) {
          worker.join();
        }
      }
    }

    float sum = 0.0f;
    for (float d_sq : min_distances) {
      sum += d_sq;
    }

    if (sum < 1e-9f) {
      std::uniform_int_distribution<size_t> fallback_dis(0, n - 1);
      centroids_[i] = vectors[fallback_dis(gen)];
      continue;
    }

    std::uniform_real_distribution<float> prob_dis(0.0f, sum);
    float target = prob_dis(gen);

    float  cumsum   = 0.0f;
    size_t next_idx = 0;
    for (size_t j = 0; j < n; ++j) {
      cumsum += min_distances[j];
      if (cumsum >= target) {
        next_idx = j;
        break;
      }
    }

    centroids_[i] = vectors[next_idx];
  }

  size_t batch_size = n;
  if (use_mini_batch) {
    size_t desired_lists = static_cast<size_t>(std::max(1, lists_));
    size_t desired = std::max<size_t>(static_cast<size_t>(k) * 6, desired_lists * 6);  // 平衡采样：6倍
    desired        = std::max<size_t>(desired, 1000);  // 适中的最小值
    batch_size     = std::min(desired, n);
    LOG_INFO("Using Mini-Batch K-Means with batch_size=%zu for n=%zu vectors", batch_size, n);
  }

  vector<int> assignments(n, -1);
  const size_t worker_count = std::max<size_t>(1, std::min(max_threads, n));
  const float  convergence_threshold = 5e-4f * static_cast<float>(dim);  // 平衡收敛条件
  int          completed_iterations  = 0;

  std::vector<float> recent_shifts;
  recent_shifts.reserve(3);  // 恢复3次连续检查

  int effective_max_iter = std::max(1, max_iter);
  if (use_mini_batch) {
    effective_max_iter = std::min(effective_max_iter, 10);  // 平衡性能和召回率：10次迭代
  }

  for (int iter = 0; iter < effective_max_iter; ++iter) {
    completed_iterations = iter + 1;
    std::atomic<bool> changed_flag(false);

    std::vector<size_t> sample_indices;
    if (use_mini_batch) {
      sample_indices.reserve(batch_size);
      std::uniform_int_distribution<size_t> sample_dis(0, n - 1);
      for (size_t idx = 0; idx < batch_size; ++idx) {
        sample_indices.push_back(sample_dis(gen));
      }
    } else {
      sample_indices.resize(n);
      std::iota(sample_indices.begin(), sample_indices.end(), 0);
    }

    const size_t sample_size       = sample_indices.size();
    const size_t sample_block_size = (sample_size + worker_count - 1) / worker_count;

    auto assign_worker = [this, &vectors, &assignments, &changed_flag, &sample_indices, k, dim](size_t start_idx, size_t end_idx) {
      for (size_t idx = start_idx; idx < end_idx; ++idx) {
        size_t       data_index = sample_indices[idx];
        const float *vec_ptr    = vectors[data_index].data();

        float min_dist_sq = std::numeric_limits<float>::max();
        int   best_cluster = 0;

        for (int j = 0; j < k; ++j) {
          float dist_sq = l2_squared_unrolled(vec_ptr, this->centroids_[j].data(), dim);
          if (dist_sq < min_dist_sq) {
            min_dist_sq = dist_sq;
            best_cluster = j;
          }
        }

        if (assignments[data_index] != best_cluster) {
          assignments[data_index] = best_cluster;
          changed_flag.store(true, std::memory_order_relaxed);
        }
      }
    };

    if (worker_count == 1) {
      assign_worker(0, sample_size);
    } else {
      std::vector<std::thread> workers;
      workers.reserve(worker_count);
      for (size_t t = 0; t < worker_count; ++t) {
        size_t start_idx = t * sample_block_size;
        if (start_idx >= sample_size) {
          break;
        }
        size_t end_idx = std::min(start_idx + sample_block_size, sample_size);
        workers.emplace_back(assign_worker, start_idx, end_idx);
      }
      for (auto &worker : workers) {
        if (worker.joinable()) {
          worker.join();
        }
      }
    }

    if (!changed_flag.load(std::memory_order_relaxed)) {
      LOG_INFO("K-Means converged (assignments stable) at iteration %d", iter);
      break;
    }

    vector<vector<float>> new_centroids(k, vector<float>(dim, 0.0f));
    vector<int>           counts(k, 0);

    auto accumulate_point = [&](size_t data_index) {
      if (data_index >= n) {
        return;
      }
      int cluster = assignments[data_index];
      if (cluster < 0 || cluster >= k) {
        return;
      }
      counts[cluster]++;
      const float *vec_ptr  = vectors[data_index].data();
      float       *dest_ptr = new_centroids[cluster].data();
      for (size_t d = 0; d < dim; ++d) {
        dest_ptr[d] += vec_ptr[d];
      }
    };

    if (use_mini_batch) {
      for (size_t idx : sample_indices) {
        accumulate_point(idx);
      }
    } else {
      for (size_t idx = 0; idx < n; ++idx) {
        accumulate_point(idx);
      }
    }

    float max_shift_sq = 0.0f;
    for (int j = 0; j < k; ++j) {
      if (counts[j] > 0) {
        float inv_count   = 1.0f / static_cast<float>(counts[j]);
        float *centroid_p = new_centroids[j].data();
        for (size_t d = 0; d < dim; ++d) {
          centroid_p[d] *= inv_count;
        }
        float shift_sq = l2_squared_unrolled(centroids_[j].data(), centroid_p, dim);
        if (shift_sq > max_shift_sq) {
          max_shift_sq = shift_sq;
        }
        centroids_[j] = std::move(new_centroids[j]);
      }
    }

    float max_shift = std::sqrt(max_shift_sq);
    recent_shifts.push_back(max_shift);
    if (recent_shifts.size() > 3) {
      recent_shifts.erase(recent_shifts.begin());
    }

    if (recent_shifts.size() == 3) {
      bool all_small = true;
      for (float shift : recent_shifts) {
        if (shift >= convergence_threshold) {
          all_small = false;
          break;
        }
      }
      if (all_small) {
        LOG_INFO("K-Means converged with 3 consecutive small shifts at iteration %d", iter);
        break;
      }
    }

    if (max_shift < convergence_threshold * 0.5f) {
      LOG_INFO("K-Means converged with max_shift=%.6f at iteration %d", max_shift, iter);
      break;
    }
  }

  refresh_centroid_norms();
  LOG_INFO("K-Means clustering completed with k=%d, iterations=%d", k, completed_iterations);
}

const FieldMeta *IvfflatIndex::get_vector_field_meta() const
{
  if (!table_) {
    return nullptr;
  }
  return table_->table_meta().field(vector_field_name_.c_str());
}

RC IvfflatIndex::extract_vector_from_record(const char *record, vector<float> &vec) const
{
  const FieldMeta *vector_field_meta = get_vector_field_meta();
  if (!vector_field_meta) {
    return RC::INTERNAL;
  }

  const int schema_dim = vector_field_meta->vector_length();
  const bool vector_lob =
      (schema_dim > 1000) ||
      (schema_dim <= 0 && vector_field_meta->len() == static_cast<int>(sizeof(LobRef)));

  const char      *payload = nullptr;
  int              payload_len = 0;

  if (vector_lob) {
    LobRef ref;
    memcpy(&ref, record + vector_field_meta->offset(), sizeof(LobRef));
    if (ref.length <= 0 || (ref.length % static_cast<int>(sizeof(float)) != 0)) {
      LOG_WARN("Invalid LobRef length for vector column. table=%s length=%d",
               table_ != nullptr ? table_->name() : "<unknown>", ref.length);
      return RC::INTERNAL;
    }
    if (table_ == nullptr || table_->lob_handler() == nullptr) {
      LOG_WARN("Table or LOB handler unavailable when extracting vector");
      return RC::INTERNAL;
    }
    auto &tmp_buffer = g_lob_tmp_buffer;
    tmp_buffer.resize(static_cast<size_t>(ref.length));
    RC rc = table_->lob_handler()->get_data(ref.offset, ref.length, tmp_buffer.data());
    if (rc != RC::SUCCESS) {
      LOG_WARN("Failed to load vector LOB payload. rc=%s", strrc(rc));
      return rc;
    }
    payload     = tmp_buffer.data();
    payload_len = ref.length;
  } else {
    payload     = record + vector_field_meta->offset();
    payload_len = vector_field_meta->len();
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
  vec.resize(actual_dim);
  std::memcpy(vec.data(), float_data, static_cast<size_t>(payload_len));

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
    configured_probes = 10;  // 提高默认probes：5→10，提升召回率
  }
  if (configured_probes > lists_) {
    LOG_INFO("Adjust probes from %d to %d to match lists", configured_probes, lists_);
    configured_probes = lists_;
  }
  probes_ = std::max(1, configured_probes);

  LOG_INFO("IVF-Flat meta config: index_type=%s distance=%s lists=%d probes=%d",
           index_type_.c_str(), distance_type_.c_str(), lists_, probes_);
}

RC IvfflatIndex::enqueue_pending_entry(IvfEntry &&entry, bool force_flush)
{
  std::vector<IvfEntry> batch;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_entries_.emplace_back(std::move(entry));
    if (!force_flush && pending_entries_.size() < pending_batch_limit_) {
      return RC::SUCCESS;
    }
    batch.swap(pending_entries_);
  }

  RC rc = process_batch(batch);
  if (rc != RC::SUCCESS) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_entries_.insert(
        pending_entries_.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
  }
  return rc;
}

RC IvfflatIndex::flush_pending_entries()
{
  if (!inited_) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_entries_.clear();
    return RC::SUCCESS;
  }

  std::vector<IvfEntry> batch;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_entries_.empty()) {
      return RC::SUCCESS;
    }
    batch.swap(pending_entries_);
  }

  RC rc = process_batch(batch);
  if (rc != RC::SUCCESS) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_entries_.insert(
        pending_entries_.end(), std::make_move_iterator(batch.begin()), std::make_move_iterator(batch.end()));
  }
  return rc;
}

RC IvfflatIndex::process_batch(std::vector<IvfEntry> &batch)
{
  if (batch.empty()) {
    return RC::SUCCESS;
  }

  if (centroids_.empty()) {
    LOG_WARN("Centroids missing before batch insert, rebuild default clusters");

    if (dimension_ <= 0) {
      LOG_WARN("Invalid dimension while rebuilding centroids");
      return RC::INTERNAL;
    }

    lists_  = std::max(1, lists_);
    probes_ = std::max(1, std::min(probes_, lists_));

    centroids_.assign(lists_, std::vector<float>(dimension_, 0.0f));
    inverted_lists_.assign(lists_, std::vector<IvfEntry>());
    centroid_norms_.assign(lists_, 0.0f);
  }

  const size_t lists = centroids_.size();
  if (lists == 0) {
    return RC::SUCCESS;
  }

  if (centroid_norms_.size() != lists) {
    refresh_centroid_norms();
  }

  std::vector<int> assignments(batch.size(), 0);
  const size_t     dim = (dimension_ > 0) ? static_cast<size_t>(dimension_)
                                          : (batch.empty() ? 0 : batch.front().vector_data.size());

  if (dim == 0) {
    LOG_WARN("Invalid dimension detected while processing batch insert");
    return RC::INTERNAL;
  }

  const size_t max_threads = std::max<size_t>(1, std::thread::hardware_concurrency());
  const size_t worker_count =
      std::max<size_t>(1, std::min({max_threads, batch.size(), static_cast<size_t>(lists)}));
  const size_t block_size = (batch.size() + worker_count - 1) / worker_count;

  std::vector<std::vector<size_t>> local_counts(worker_count, std::vector<size_t>(lists, 0));

  auto assign_worker = [&](size_t worker_id, size_t start, size_t end) {
    auto &counts_local = local_counts[worker_id];
    for (size_t i = start; i < end; ++i) {
      const float *vec_ptr = batch[i].vector_data.data();
      float        vec_norm_sq = batch[i].norm_sq;
      if (vec_norm_sq <= 0.0f) {
        vec_norm_sq = l2_squared_unrolled(vec_ptr, vec_ptr, dim);
        batch[i].norm_sq = vec_norm_sq;
      }

      float min_dist_sq = std::numeric_limits<float>::max();
      int   best_cluster = 0;

      for (size_t j = 0; j < lists; ++j) {
        float dot     = dot_product_unrolled(vec_ptr, centroids_[j].data(), dim);
        float dist_sq = centroid_norms_[j] + vec_norm_sq - 2.0f * dot;
        if (dist_sq < 0.0f) {
          dist_sq = 0.0f;
        }
        if (dist_sq < min_dist_sq) {
          min_dist_sq = dist_sq;
          best_cluster = static_cast<int>(j);
        }
      }

      assignments[i] = best_cluster;
      counts_local[best_cluster]++;
    }
  };

  if (worker_count == 1) {
    assign_worker(0, 0, batch.size());
  } else {
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t worker_id = 0; worker_id < worker_count; ++worker_id) {
      size_t start = worker_id * block_size;
      if (start >= batch.size()) {
        break;
      }
      size_t end = std::min(start + block_size, batch.size());
      workers.emplace_back(assign_worker, worker_id, start, end);
    }
    for (auto &worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  std::vector<size_t> counts(lists, 0);
  for (size_t worker_id = 0; worker_id < local_counts.size(); ++worker_id) {
    for (size_t j = 0; j < lists; ++j) {
      counts[j] += local_counts[worker_id][j];
    }
  }

  // 优化锁策略：先预留空间，再插入数据（两次短锁替代一次长锁）
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t cluster = 0; cluster < lists; ++cluster) {
      if (counts[cluster] > 0) {
        size_t current = inverted_lists_[cluster].size();
        size_t needed = current + counts[cluster];
        // 预留20%额外空间，减少后续realloc
        size_t reserved = static_cast<size_t>(needed * 1.2);
        if (reserved > inverted_lists_[cluster].capacity()) {
          inverted_lists_[cluster].reserve(reserved);
        }
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < batch.size(); ++i) {
      inverted_lists_[assignments[i]].emplace_back(std::move(batch[i]));
    }
  }

  // 优化零质心检测：只在第一次检测，避免重复O(lists×dim)开销
  if (!centroids_ready_) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!centroids_ready_) {  // 双重检查锁定
      bool has_zero_centroids = true;
      for (const auto &centroid : centroids_) {
        for (float val : centroid) {
          if (std::abs(val) > 1e-9f) {
            has_zero_centroids = false;
            break;
          }
        }
        if (!has_zero_centroids) {
          break;
        }
      }

      if (has_zero_centroids) {
    size_t total_vectors = 0;
    for (const auto &list : inverted_lists_) {
      total_vectors += list.size();
    }

    if (total_vectors >= static_cast<size_t>(lists_ * 2)) {
      LOG_INFO("Rebuilding index with %zu vectors after zero-centroid detection", total_vectors);

      vector<vector<float>> all_vectors;
      all_vectors.reserve(total_vectors);
      for (const auto &list : inverted_lists_) {
        for (const auto &entry : list) {
          all_vectors.push_back(entry.vector_data);
        }
      }

      int actual_lists = std::min(static_cast<int>(lists), static_cast<int>(all_vectors.size()));
      if (actual_lists > 0) {
        kmeans_clustering(all_vectors, actual_lists, 20);  // 零质心重建：20次迭代

        vector<vector<IvfEntry>> new_inverted_lists(actual_lists);
        for (const auto &list : inverted_lists_) {
          for (const auto &entry : list) {
            float        min_d_sq = std::numeric_limits<float>::max();
            int          best_c   = 0;
            const float *vec_ptr2 = entry.vector_data.data();
            for (int j = 0; j < actual_lists; ++j) {
              float d_sq = l2_squared_unrolled(vec_ptr2, centroids_[j].data(), dim);
              if (d_sq < min_d_sq) {
                min_d_sq = d_sq;
                best_c   = j;
              }
            }
            new_inverted_lists[best_c].push_back(entry);
          }
        }
        inverted_lists_ = std::move(new_inverted_lists);
        LOG_INFO("Index rebuilt successfully with %d clusters", actual_lists);
      }
    }
      }
      // 标记质心已就绪，后续batch无需重复检测
      centroids_ready_ = true;
    }
  }

  return RC::SUCCESS;
}

void IvfflatIndex::refresh_centroid_norms()
{
  centroid_norms_.resize(centroids_.size());
  for (size_t i = 0; i < centroids_.size(); ++i) {
    const auto &centroid = centroids_[i];
    centroid_norms_[i] = centroid.empty() ? 0.0f
                                         : l2_squared_unrolled(centroid.data(), centroid.data(), centroid.size());
  }
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
  vector_field_name_ = field_metas[0].name();

  apply_meta_config(index_meta);
  int requested_lists = lists_;

  // 璁＄畻鍚戦噺缁村害
  const FieldMeta *vector_field_meta = &field_metas[0];
  dimension_ = vector_field_meta->vector_length();
  if (dimension_ <= 0) {
    const int physical_len = vector_field_meta->len();
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

  // 鎵弿琛紝鏀堕泦鎵€鏈夊悜閲忔暟鎹?
  vector<vector<float>> all_vectors;
  vector<RID> all_rids;
  all_vectors.reserve(1000);  // 鍐呭瓨浼樺寲锛氶鍒嗛厤绌洪棿
  all_rids.reserve(1000);

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
      const int physical_len = vector_field_meta->len();
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

  // 鎵цK-Means鑱氱被
  int actual_lists = std::min(lists_, static_cast<int>(all_vectors.size()));
  if (actual_lists <= 0) {
    actual_lists = std::min(static_cast<int>(all_vectors.size()), 1);
  }
  probes_ = std::max(1, std::min(probes_, actual_lists));
  lists_  = actual_lists;
  LOG_INFO("Finalize IVF-Flat index config: requested_lists=%d actual_lists=%d probes=%d",
           requested_lists, lists_, probes_);
  kmeans_clustering(all_vectors, actual_lists, 20);  // 平衡优化：20次迭代保证质量

  // 鍒濆鍖栧€掓帓鍒楄〃
  inverted_lists_.resize(actual_lists);

  // 灏嗗悜閲忓垎閰嶅埌瀵瑰簲鐨勫€掓帓鍒楄〃锛堝苟琛屽寲浼樺寲锛?
  const size_t total_vectors = all_vectors.size();
  const size_t max_threads = static_cast<size_t>(std::max(1u, std::thread::hardware_concurrency()));
  const size_t assign_worker_count = std::max<size_t>(1, std::min(max_threads, total_vectors));
  const size_t assign_block_size = (total_vectors + assign_worker_count - 1) / assign_worker_count;

  const size_t dim_assign = static_cast<size_t>(dimension_ > 0 ? dimension_ : (all_vectors.empty() ? 0 : all_vectors.front().size()));
  std::vector<std::vector<std::vector<IvfEntry>>> thread_local_lists(
      assign_worker_count, std::vector<std::vector<IvfEntry>>(actual_lists));

  auto assign_to_list = [this, &all_vectors, &all_rids, actual_lists, &thread_local_lists, dim_assign](size_t worker_id,
                                                                                                       size_t start,
                                                                                                       size_t end) {
    auto &local_lists = thread_local_lists[worker_id];
    for (size_t i = start; i < end; ++i) {
      const float *vec_ptr = all_vectors[i].data();
      float        min_dist_sq = std::numeric_limits<float>::max();
      int          best_cluster = 0;

      for (int j = 0; j < actual_lists; ++j) {
        float dist_sq = l2_squared_unrolled(vec_ptr, this->centroids_[j].data(), dim_assign);
        if (dist_sq < min_dist_sq) {
          min_dist_sq = dist_sq;
          best_cluster = j;
        }
      }

      auto &entry = local_lists[best_cluster].emplace_back(all_rids[i], std::move(all_vectors[i]));
      entry.norm_sq = entry.vector_data.empty() ? 0.0f
                      : l2_squared_unrolled(entry.vector_data.data(), entry.vector_data.data(), entry.vector_data.size());
    }
  };

  size_t used_worker_count = 0;

  if (assign_worker_count == 1 || total_vectors <= assign_block_size) {
    assign_to_list(0, 0, total_vectors);
    used_worker_count = 1;
  } else {
    std::vector<std::thread> workers;
    workers.reserve(assign_worker_count);
    for (size_t t = 0; t < assign_worker_count; ++t) {
      size_t start_idx = t * assign_block_size;
      if (start_idx >= total_vectors) {
        break;
      }
      size_t end_idx = std::min(start_idx + assign_block_size, total_vectors);
      workers.emplace_back(assign_to_list, t, start_idx, end_idx);
      used_worker_count = t + 1;
    }
    for (auto &worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  if (used_worker_count == 0) {
    used_worker_count = 1;
  }

  for (int cluster = 0; cluster < actual_lists; ++cluster) {
    size_t expected_size = inverted_lists_[cluster].size();
    for (size_t wid = 0; wid < used_worker_count; ++wid) {
      expected_size += thread_local_lists[wid][cluster].size();
    }
    if (expected_size > inverted_lists_[cluster].capacity()) {
      inverted_lists_[cluster].reserve(expected_size);
    }
    for (size_t wid = 0; wid < used_worker_count; ++wid) {
      auto &local_list = thread_local_lists[wid][cluster];
      for (auto &entry : local_list) {
        inverted_lists_[cluster].emplace_back(std::move(entry));
      }
    }
  }

  // 淇濆瓨绱㈠紩鍒版枃浠?
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
  vector_field_name_ = field_metas[0].name();
  apply_meta_config(index_meta);

  // 浠庢枃浠跺姞杞界储寮?
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
  flush_pending_entries();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_entries_.clear();
  }
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

  vector<float> vec;
  RC rc = extract_vector_from_record(record, vec);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  IvfEntry entry(*rid, std::move(vec));
  return enqueue_pending_entry(std::move(entry), false);
}


RC IvfflatIndex::delete_entry(const char *record, const RID *rid)
{
  // 鏆備笉瀹炵幇鍒犻櫎鍔熻兘
  return RC::UNIMPLEMENTED;
}

vector<int> IvfflatIndex::find_nearest_clusters(const vector<float> &query_vector, int n) const
{
  if (centroids_.empty()) {
    return {};
  }

  // 璁＄畻鍒版墍鏈夎仛绫讳腑蹇冪殑璺濈
  vector<pair<float, int>> distances;
  distances.reserve(centroids_.size());

  const float *query_ptr = query_vector.data();
  const size_t dim = static_cast<size_t>(dimension_ > 0 ? dimension_ : query_vector.size());

  for (size_t i = 0; i < centroids_.size(); ++i) {
    float dist_sq = l2_squared_unrolled(query_ptr, centroids_[i].data(), dim);
    distances.emplace_back(dist_sq, static_cast<int>(i));
  }

  // 鎺掑簭骞惰繑鍥炴渶杩戠殑n涓?
  int actual_n = std::min(n, static_cast<int>(distances.size()));
  if (actual_n <= 0) {
    return {};
  }

  if (actual_n < static_cast<int>(distances.size())) {
    std::partial_sort(distances.begin(), distances.begin() + actual_n, distances.end());
    distances.resize(actual_n);
  } else {
    std::sort(distances.begin(), distances.end());
  }

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

  // 优化flush策略：只在pending条目过多时才flush，避免每次查询阻塞
  size_t pending_size = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_size = pending_entries_.size();
  }

  if (pending_size > 8192) {  // 提高阈值：4096→8192，减少flush频率
    RC flush_rc = flush_pending_entries();
    if (flush_rc != RC::SUCCESS) {
      LOG_WARN("Failed to flush pending entries before ANN search. rc=%s", strrc(flush_rc));
    }
  }

  if (limit == 0) {
    return {};
  }

  if (query_vector.size() != static_cast<size_t>(dimension_)) {
    LOG_WARN("Query vector dimension mismatch: expected=%d, got=%zu", dimension_, query_vector.size());
    return {};
  }

  // 鎵惧埌鏈€杩戠殑probes_涓仛绫讳腑蹇?
  vector<int> nearest_clusters = find_nearest_clusters(query_vector, probes_);

  // 浣跨敤heap缁存姢Top-K缁撴灉锛孫(log k)鎻掑叆澶嶆潅搴?
  struct TopKHeap
  {
    explicit TopKHeap(size_t k) : limit_(k) { heap_.reserve(k); }

    float cutoff() const
    {
      return (heap_.size() < limit_) ? std::numeric_limits<float>::max() : heap_.front().first;
    }

    void consider(float dist_sq, const RID &rid)
    {
      if (heap_.size() < limit_) {
        heap_.emplace_back(dist_sq, rid);
        if (heap_.size() == limit_) {
          // 鑷畾涔夋瘮杈冨櫒锛氬彧姣旇緝璺濈锛屼笉姣旇緝RID
          auto cmp = [](const pair<float, RID> &a, const pair<float, RID> &b) {
            return a.first < b.first;
          };
          std::make_heap(heap_.begin(), heap_.end(), cmp);
        }
      } else if (dist_sq < heap_.front().first) {
        auto cmp = [](const pair<float, RID> &a, const pair<float, RID> &b) {
          return a.first < b.first;
        };
        std::pop_heap(heap_.begin(), heap_.end(), cmp);
        heap_.back() = {dist_sq, rid};
        std::push_heap(heap_.begin(), heap_.end(), cmp);
      }
    }

    vector<RID> materialize() const
    {
      auto sorted = heap_;
      auto cmp = [](const pair<float, RID> &a, const pair<float, RID> &b) {
        return a.first < b.first;
      };
      std::sort_heap(sorted.begin(), sorted.end(), cmp);
      vector<RID> result;
      result.reserve(sorted.size());
      for (const auto &p : sorted) {
        result.push_back(p.second);
      }
      return result;
    }

  private:
    size_t                   limit_;
    vector<pair<float, RID>> heap_;
  };

  TopKHeap top_k(limit);

  const float *query_data = query_vector.data();
  const size_t dim = static_cast<size_t>(dimension_);
  if (dim == 0) {
    return {};
  }

  float query_norm_sq = l2_squared_unrolled(query_data, query_data, dim);

  for (int cluster_id : nearest_clusters) {
    if (cluster_id < 0 || cluster_id >= static_cast<int>(inverted_lists_.size())) {
      continue;
    }

    auto &entries = inverted_lists_[cluster_id];
    for (auto &entry : entries) {
      const float *vec_data = entry.vector_data.data();
      if (vec_data == nullptr) {
        continue;
      }
      float entry_norm_sq = entry.norm_sq;
      if (entry_norm_sq <= 0.0f) {
        entry_norm_sq = l2_squared_unrolled(vec_data, vec_data, dim);
        entry.norm_sq = entry_norm_sq;
      }

      float dot     = dot_product_unrolled(query_data, vec_data, dim);
      float dist_sq = query_norm_sq + entry_norm_sq - 2.0f * dot;
      if (dist_sq < 0.0f) {
        dist_sq = 0.0f;
      }

      float current_cutoff = top_k.cutoff();
      if (dist_sq >= current_cutoff) {
        continue;
      }

      top_k.consider(dist_sq, entry.rid);
    }
  }

  return top_k.materialize();
}

bool IvfflatIndex::ready() const
{
  if (!inited_ || dimension_ <= 0 || centroids_.empty()) {
    return false;
  }

  // Fix: Check if inverted lists have data
  bool has_data = false;
  for (const auto& list : inverted_lists_) {
    if (!list.empty()) {
      has_data = true;
      break;
    }
  }
  if (!has_data) {
    return false;
  }

  // 妫€鏌ユ槸鍚︽湁鑷冲皯涓€涓潪闆剁殑鑱氱被涓績锛堥伩鍏嶅叏闆跺垵濮嬪寲鐨勬儏鍐碉級
  bool has_nonzero_centroid = false;
  for (const auto &centroid : centroids_) {
    for (float val : centroid) {
      if (std::abs(val) > 1e-9f) {
        has_nonzero_centroid = true;
        break;
      }
    }
    if (has_nonzero_centroid) {
      break;
    }
  }

  return has_nonzero_centroid;
}

RC IvfflatIndex::save_to_file()
{
  std::ofstream ofs(file_name_, std::ios::binary);
  if (!ofs.is_open()) {
    LOG_WARN("Failed to open file for writing: %s", file_name_.c_str());
    return RC::IOERR_OPEN;
  }

  std::lock_guard<std::mutex> lock(mutex_);

  // 鍐欏叆绱㈠紩鍏冩暟鎹?  ofs.write(reinterpret_cast<const char *>(&lists_), sizeof(lists_));
  ofs.write(reinterpret_cast<const char *>(&probes_), sizeof(probes_));
  ofs.write(reinterpret_cast<const char *>(&dimension_), sizeof(dimension_));

  // 鍐欏叆鑱氱被涓績鏁伴噺
  int num_centroids = static_cast<int>(centroids_.size());
  ofs.write(reinterpret_cast<const char *>(&num_centroids), sizeof(num_centroids));

  // 鍐欏叆姣忎釜鑱氱被涓績
  for (const auto &centroid : centroids_) {
    ofs.write(reinterpret_cast<const char *>(centroid.data()), centroid.size() * sizeof(float));
  }

  // 鍐欏叆鍊掓帓鍒楄〃
  for (const auto &list : inverted_lists_) {
    int list_size = static_cast<int>(list.size());
    ofs.write(reinterpret_cast<const char *>(&list_size), sizeof(list_size));

    for (const auto &entry : list) {
      // 鍐欏叆RID
      ofs.write(reinterpret_cast<const char *>(&entry.rid), sizeof(RID));
      // 鍐欏叆鍚戦噺鏁版嵁
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

  // 璇诲彇绱㈠紩鍏冩暟鎹?  ifs.read(reinterpret_cast<char *>(&lists_), sizeof(lists_));
  ifs.read(reinterpret_cast<char *>(&probes_), sizeof(probes_));
  ifs.read(reinterpret_cast<char *>(&dimension_), sizeof(dimension_));

  // 璇诲彇鑱氱被涓績鏁伴噺
  int num_centroids = 0;
  ifs.read(reinterpret_cast<char *>(&num_centroids), sizeof(num_centroids));

  // 璇诲彇姣忎釜鑱氱被涓績
  centroids_.resize(num_centroids);
  for (int i = 0; i < num_centroids; ++i) {
    centroids_[i].resize(dimension_);
    ifs.read(reinterpret_cast<char *>(centroids_[i].data()), dimension_ * sizeof(float));
  }

  // 璇诲彇鍊掓帓鍒楄〃
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

      auto &entry = inverted_lists_[i].emplace_back(rid, vec);
      entry.norm_sq = entry.vector_data.empty() ? 0.0f
                      : l2_squared_unrolled(entry.vector_data.data(), entry.vector_data.data(), entry.vector_data.size());
    }
  }

  ifs.close();

  if (!ifs) {
    LOG_WARN("Error occurred while reading index file");
    return RC::IOERR_READ;
  }

  // 重新计算centroid norms（关键修复：load后必须初始化centroid_norms_）
  refresh_centroid_norms();

  LOG_INFO("Index loaded from file: %s, centroids=%d, dimension=%d",
           file_name_.c_str(), num_centroids, dimension_);
  return RC::SUCCESS;
}

RC IvfflatIndex::sync()
{
  RC flush_rc = flush_pending_entries();
  if (flush_rc != RC::SUCCESS) {
    LOG_WARN("Failed to flush pending entries before sync. rc=%s", strrc(flush_rc));
    return flush_rc;
  }

  if (!inited_) {
    return RC::SUCCESS;
  }

  return save_to_file();
}

IndexScanner *IvfflatIndex::create_scanner(const char *left_key, int left_len, bool left_inclusive,
                                            const char *right_key, int right_len, bool right_inclusive)
{
  // 鍚戦噺绱㈠紩涓嶆敮鎸佽寖鍥存壂鎻?  LOG_WARN("IvfflatIndex does not support range scanning");
  return nullptr;
}
