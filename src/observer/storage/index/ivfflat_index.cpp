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
#include <atomic>
#include <thread>
#include <numeric>
#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace {

#if defined(__AVX2__)
inline float horizontal_sum_ps(__m256 value)
{
  alignas(32) float buffer[8];
  _mm256_storeu_ps(buffer, value);
  return buffer[0] + buffer[1] + buffer[2] + buffer[3] + buffer[4] + buffer[5] + buffer[6] + buffer[7];
}
#endif

inline float l2_squared_unrolled(const float *a, const float *b, size_t dim)
{
#if defined(__AVX2__)
  size_t i         = 0;
  const size_t blk = (dim / 16) * 16;
  __m256       acc0 = _mm256_setzero_ps();
  __m256       acc1 = _mm256_setzero_ps();

  for (; i < blk; i += 16) {
    __m256 va0   = _mm256_loadu_ps(a + i);
    __m256 vb0   = _mm256_loadu_ps(b + i);
    __m256 diff0 = _mm256_sub_ps(va0, vb0);
    acc0         = _mm256_fmadd_ps(diff0, diff0, acc0);

    __m256 va1   = _mm256_loadu_ps(a + i + 8);
    __m256 vb1   = _mm256_loadu_ps(b + i + 8);
    __m256 diff1 = _mm256_sub_ps(va1, vb1);
    acc1         = _mm256_fmadd_ps(diff1, diff1, acc1);
  }

  if (i + 8 <= dim) {
    __m256 va0   = _mm256_loadu_ps(a + i);
    __m256 vb0   = _mm256_loadu_ps(b + i);
    __m256 diff0 = _mm256_sub_ps(va0, vb0);
    acc0         = _mm256_fmadd_ps(diff0, diff0, acc0);
    i += 8;
  }

  float total = horizontal_sum_ps(acc0) + horizontal_sum_ps(acc1);

  for (; i + 4 <= dim; i += 4) {
    float diff0 = a[i] - b[i];
    float diff1 = a[i + 1] - b[i + 1];
    float diff2 = a[i + 2] - b[i + 2];
    float diff3 = a[i + 3] - b[i + 3];
    total += diff0 * diff0 + diff1 * diff1 + diff2 * diff2 + diff3 * diff3;
  }

  for (; i < dim; ++i) {
    float diff = a[i] - b[i];
    total += diff * diff;
  }

  return total;
#else
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
#endif
}

inline float l2_squared_with_cap(const float *a, const float *b, size_t dim, float cap)
{
  if (cap <= 0.0f) {
    return 0.0f;
  }

  const bool check_cap = cap < std::numeric_limits<float>::max();
  float       total     = 0.0f;
  size_t      i         = 0;
#if defined(__AVX2__)
  if (!check_cap) {
    return l2_squared_unrolled(a, b, dim);
  }

  const size_t blk16 = (dim / 16) * 16;
  for (; i < blk16; i += 16) {
    __m256 va0   = _mm256_loadu_ps(a + i);
    __m256 vb0   = _mm256_loadu_ps(b + i);
    __m256 diff0 = _mm256_sub_ps(va0, vb0);
    __m256 sq0   = _mm256_mul_ps(diff0, diff0);

    __m256 va1   = _mm256_loadu_ps(a + i + 8);
    __m256 vb1   = _mm256_loadu_ps(b + i + 8);
    __m256 diff1 = _mm256_sub_ps(va1, vb1);
    __m256 sq1   = _mm256_mul_ps(diff1, diff1);

    float block_sum = horizontal_sum_ps(_mm256_add_ps(sq0, sq1));
    total += block_sum;
    if (total >= cap) {
      return total;
    }
  }

  const size_t blk8 = (dim / 8) * 8;
  for (; i < blk8; i += 8) {
    __m256 va0   = _mm256_loadu_ps(a + i);
    __m256 vb0   = _mm256_loadu_ps(b + i);
    __m256 diff0 = _mm256_sub_ps(va0, vb0);
    __m256 sq0   = _mm256_mul_ps(diff0, diff0);

    float block_sum = horizontal_sum_ps(sq0);
    total += block_sum;
    if (total >= cap) {
      return total;
    }
  }
#else
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
#endif

  for (; i < dim; ++i) {
    float diff = a[i] - b[i];
    total += diff * diff;
    if (check_cap && total >= cap) {
      return total;
    }
  }

  return total;
}

}  // namespace

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
    size_t desired = std::max<size_t>(static_cast<size_t>(k) * 8, desired_lists * 8);
    desired        = std::max<size_t>(desired, 1024);
    batch_size     = std::min(desired, n);
    LOG_INFO("Using Mini-Batch K-Means with batch_size=%zu for n=%zu vectors", batch_size, n);
  }

  vector<int> assignments(n, -1);
  const size_t worker_count = std::max<size_t>(1, std::min(max_threads, n));
  const float  convergence_threshold = 1e-4f * static_cast<float>(dim);
  int          completed_iterations  = 0;

  std::vector<float> recent_shifts;
  recent_shifts.reserve(3);

  int effective_max_iter = std::max(1, max_iter);
  if (use_mini_batch) {
    effective_max_iter = std::min(effective_max_iter, 40);
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
  std::vector<char> tmp_buffer;

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
  kmeans_clustering(all_vectors, actual_lists, 100);

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

      local_lists[best_cluster].emplace_back(all_rids[i], std::move(all_vectors[i]));
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

  // 鎻愬彇鍚戦噺
  vector<float> vec;
  RC rc = extract_vector_from_record(record, vec);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  // 鎵惧埌鏈€杩戠殑鑱氱被涓績
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

  float        min_dist_sq = std::numeric_limits<float>::max();
  int          best_cluster = 0;
  const float *vec_ptr      = vec.data();
  const size_t dim          = vec.size();

  for (size_t i = 0; i < centroids_.size(); ++i) {
    float dist_sq = l2_squared_unrolled(vec_ptr, centroids_[i].data(), dim);
    if (dist_sq < min_dist_sq) {
      min_dist_sq = dist_sq;
      best_cluster = static_cast<int>(i);
    }
  }

  // 鎻掑叆鍒板€掓帓鍒楄〃
  inverted_lists_[best_cluster].emplace_back(*rid, vec);

  // 妫€鏌ユ槸鍚﹂渶瑕侀噸寤虹储寮曪紙濡傛灉centroids_鏄叏闆朵笖宸茬疮绉冻澶熸暟鎹級
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
    // 缁熻褰撳墠宸叉彃鍏ョ殑鍚戦噺鎬绘暟
    size_t total_vectors = 0;
    for (const auto &list : inverted_lists_) {
      total_vectors += list.size();
    }

    // 濡傛灉宸叉湁瓒冲鐨勫悜閲忥紙鑷冲皯鏄痩ists_鐨?鍊嶏級锛岃Е鍙戦噸寤?
    if (total_vectors >= static_cast<size_t>(lists_ * 2)) {
      LOG_INFO("Rebuilding index with %zu vectors after zero-centroid detection", total_vectors);

      // 鏀堕泦鎵€鏈夊悜閲?
      vector<vector<float>> all_vectors;
      all_vectors.reserve(total_vectors);
      for (const auto &list : inverted_lists_) {
        for (const auto &entry : list) {
          all_vectors.push_back(entry.vector_data);
        }
      }

      // 閲嶆柊鑱氱被
      int actual_lists = std::min(lists_, static_cast<int>(all_vectors.size()));
      if (actual_lists > 0) {
        kmeans_clustering(all_vectors, actual_lists, 50);
        // 閲嶆柊鍒嗛厤鍚戦噺鍒版柊鐨勮仛绫讳腑蹇?
        vector<vector<IvfEntry>> new_inverted_lists(actual_lists);
        for (const auto &list : inverted_lists_) {
          for (const auto &entry : list) {
            float        min_d_sq = std::numeric_limits<float>::max();
            int          best_c   = 0;
            const float *vec_ptr  = entry.vector_data.data();
            const size_t dim_ptr  = entry.vector_data.size();
            for (int j = 0; j < actual_lists; ++j) {
              float d_sq = l2_squared_unrolled(vec_ptr, centroids_[j].data(), dim_ptr);
              if (d_sq < min_d_sq) {
                min_d_sq = d_sq;
                best_c = j;
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

  return RC::SUCCESS;
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
  if (limit == 0) {
    return {};
  }

  if (query_vector.size() != static_cast<size_t>(dimension_)) {
    LOG_WARN("Query vector dimension mismatch: expected=%d, got=%zu", dimension_, query_vector.size());
    return {};
  }

  // 鎵惧埌鏈€杩戠殑probes_涓仛绫讳腑蹇?
  vector<int> nearest_clusters = find_nearest_clusters(query_vector, probes_);

  // 浣跨敤浼樺厛闃熷垪缁存姢Top-K缁撴灉 (鏈€澶у爢)
  auto cmp = [](const pair<float, RID> &a, const pair<float, RID> &b) {
    return a.first < b.first;  // 鏈€澶у爢锛氳窛绂诲ぇ鐨勫湪鍫嗛《
  };
  std::priority_queue<pair<float, RID>, vector<pair<float, RID>>, decltype(cmp)> top_k(cmp);
  // 鍦ㄩ€変腑鐨勭皣涓繘琛岀簿纭悳绱?
  const float *query_data = query_vector.data();
  const size_t dim = static_cast<size_t>(dimension_);
  if (dim == 0) {
    return {};
  }
  for (int cluster_id : nearest_clusters) {
    if (cluster_id < 0 || cluster_id >= static_cast<int>(inverted_lists_.size())) {
      continue;
    }

    const auto &entries = inverted_lists_[cluster_id];
    for (const auto &entry : entries) {
      const float *vec_data = entry.vector_data.data();
      if (vec_data == nullptr) {
        continue;
      }
      float cutoff = (top_k.size() == limit && limit > 0) ? top_k.top().first : std::numeric_limits<float>::max();
      float dist_sq = l2_squared_with_cap(query_data, vec_data, dim, cutoff);

      if (top_k.size() == limit && limit > 0 && dist_sq >= cutoff) {
        continue;
      }

      if (top_k.size() < limit) {
        top_k.emplace(dist_sq, entry.rid);
      } else if (dist_sq < top_k.top().first) {
        top_k.pop();
        top_k.emplace(dist_sq, entry.rid);
      }
    }
  }

  // 鎻愬彇缁撴灉锛堥渶瑕佸弽杞『搴忥紝鍥犱负鏄渶澶у爢锛?
  vector<RID> results;

  results.reserve(top_k.size());

  while (!top_k.empty()) {
    results.push_back(top_k.top().second);
    top_k.pop();
  }

  // 鍙嶈浆浠ュ緱鍒拌窛绂讳粠灏忓埌澶х殑椤哄簭
  std::reverse(results.begin(), results.end());

  return results;
}

bool IvfflatIndex::ready() const
{
  if (!inited_ || dimension_ <= 0 || centroids_.empty()) {
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
  // 鍚戦噺绱㈠紩涓嶆敮鎸佽寖鍥存壂鎻?  LOG_WARN("IvfflatIndex does not support range scanning");
  return nullptr;
}
