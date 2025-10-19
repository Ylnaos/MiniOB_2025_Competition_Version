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
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Annoy库头文件
#include "annoylib.h"
#include "kissrandom.h"

// Annoy索引模板类型定义
template <typename T>
using AnnoyIndexType = Annoy::AnnoyIndex<int, float, T, Annoy::Kiss32Random,
                                          Annoy::AnnoyIndexSingleThreadedBuildPolicy>;

IvfflatIndex::~IvfflatIndex() noexcept
{
  close();
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
  thread_local std::vector<char> lob_buffer;

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
    lob_buffer.resize(static_cast<size_t>(ref.length));
    RC rc = table_->lob_handler()->get_data(ref.offset, ref.length, lob_buffer.data());
    if (rc != RC::SUCCESS) {
      LOG_WARN("Failed to load vector LOB payload. rc=%s", strrc(rc));
      return rc;
    }
    payload     = lob_buffer.data();
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
    LOG_WARN("COSINE distance is not yet supported, fallback to L2");
    distance_type_ = "L2_DISTANCE";
  } else if (distance_type_upper == "INNER_PRODUCT" || distance_type_upper == "DOT") {
    LOG_WARN("INNER_PRODUCT distance is not yet supported, fallback to L2");
    distance_type_ = "L2_DISTANCE";
  } else {
    LOG_WARN("Unsupported distance type %s, fallback to L2_DISTANCE", distance_type_.c_str());
    distance_type_ = "L2_DISTANCE";
  }

  int configured_lists = index_meta.lists();
  if (configured_lists <= 0) {
    configured_lists = 100;  // Annoy: n_trees=100，平衡质量和性能
  }
  lists_ = std::max(1, configured_lists);

  int configured_probes = index_meta.probes();
  if (configured_probes <= 0) {
    configured_probes = -1;  // Annoy: search_k=-1自动优化
  }
  probes_ = configured_probes;

  LOG_INFO("IVF-Flat (Annoy) meta config: index_type=%s distance=%s n_trees=%d probes=%d (NOTE: probes ignored, using search_k=-1 auto mode)",
           index_type_.c_str(), distance_type_.c_str(), lists_, probes_);
}

RC IvfflatIndex::save_rid_mapping()
{
  if (rid_map_.empty()) {
    LOG_WARN("No RID mapping to save");
    return RC::SUCCESS;
  }

  std::string aux_filename = file_name_ + ".aux";
  std::ofstream ofs(aux_filename, std::ios::binary);
  if (!ofs.is_open()) {
    LOG_WARN("Failed to open aux file for writing: %s", aux_filename.c_str());
    return RC::IOERR_OPEN;
  }

  ofs.write(reinterpret_cast<const char *>(rid_map_.data()), rid_map_.size() * sizeof(RID));
  ofs.close();

  if (!ofs) {
    LOG_WARN("Error occurred while writing aux file");
    return RC::IOERR_WRITE;
  }

  LOG_INFO("Saved %zu RID mappings to %s", rid_map_.size(), aux_filename.c_str());
  return RC::SUCCESS;
}

RC IvfflatIndex::load_rid_mapping()
{
  std::string aux_filename = file_name_ + ".aux";
  int fd = ::open(aux_filename.c_str(), O_RDONLY);
  if (fd < 0) {
    LOG_WARN("Failed to open aux file: %s, errno=%d", aux_filename.c_str(), errno);
    return RC::IOERR_OPEN;
  }

  // 获取文件大小
  off_t file_size = lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);

  if (file_size <= 0 || file_size % sizeof(RID) != 0) {
    LOG_WARN("Invalid aux file size: %ld", file_size);
    ::close(fd);
    return RC::IOERR_READ;
  }

  item_count_ = file_size / sizeof(RID);

  // mmap映射文件
  void *map_start = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);

  if (map_start == MAP_FAILED) {
    LOG_WARN("Failed to mmap aux file: %s, errno=%d", aux_filename.c_str(), errno);
    return RC::IOERR_ACCESS;
  }

  rid_map_mmap_ = static_cast<RID *>(map_start);
  LOG_INFO("Loaded %d RID mappings from %s using mmap", item_count_, aux_filename.c_str());
  return RC::SUCCESS;
}

RC IvfflatIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta,
                         span<const FieldMeta> field_metas)
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

  // 计算向量维度
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
    LOG_INFO("Creating Annoy index with dimension=%d", dimension_);
  } else {
    LOG_INFO("Creating Annoy index with unknown dimension, will infer from data");
  }

  // 扫描表，收集所有向量数据
  vector<vector<float>> all_vectors;
  all_vectors.reserve(1000);
  rid_map_.reserve(1000);

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
      rid_map_.push_back(record.rid());
    }
  }
  scanner->close_scan();
  delete scanner;

  LOG_INFO("Collected %zu vectors from table", all_vectors.size());

  if (all_vectors.empty()) {
    LOG_INFO("No vectors found in table, initialize empty Annoy index");

    if (dimension_ <= 0) {
      const int physical_len = vector_field_meta->len();
      if (physical_len > 0 && physical_len % static_cast<int>(sizeof(float)) == 0) {
        dimension_ = physical_len / static_cast<int>(sizeof(float));
      }
    }
    if (dimension_ <= 0) {
      LOG_WARN("Cannot infer positive dimension for empty Annoy index");
      return RC::INVALID_ARGUMENT;
    }

    // 创建空Annoy索引
    annoy_index_ = new AnnoyIndexType<Annoy::Euclidean>(dimension_);

    inited_ = true;
    return RC::SUCCESS;
  }

  if (dimension_ <= 0) {
    dimension_ = static_cast<int>(all_vectors.front().size());
    LOG_INFO("Infer Annoy dimension from data: %d", dimension_);
    if (dimension_ <= 0) {
      LOG_WARN("Failed to infer positive vector dimension for Annoy index");
      return RC::INVALID_ARGUMENT;
    }
  }

  // 创建Annoy索引
  auto *annoy = new AnnoyIndexType<Annoy::Euclidean>(dimension_);
  annoy_index_ = annoy;

  // 添加所有向量到Annoy
  for (size_t i = 0; i < all_vectors.size(); ++i) {
    annoy->add_item(i, all_vectors[i].data());
  }
  item_count_ = all_vectors.size();

  LOG_INFO("Building Annoy index with %d vectors, n_trees=%d...", item_count_, lists_);

  // 构建索引（lists_作为n_trees参数）
  annoy->build(lists_);

  LOG_INFO("Annoy index built successfully, saving to file...");

  // 保存Annoy索引
  if (!annoy->save(file_name_.c_str())) {
    LOG_WARN("Failed to save Annoy index to file: %s", file_name_.c_str());
    return RC::IOERR_WRITE;
  }

  // 保存RID映射
  RC save_rc = save_rid_mapping();
  if (save_rc != RC::SUCCESS) {
    LOG_WARN("Failed to save RID mapping");
    return save_rc;
  }

  // 清空临时的rid_map_，避免内存占用
  rid_map_.clear();
  rid_map_.shrink_to_fit();

  // 加载RID映射到内存（mmap），使索引立即可用于查询
  RC load_rc = load_rid_mapping();
  if (load_rc != RC::SUCCESS) {
    LOG_WARN("Failed to load RID mapping after creation");
    return load_rc;
  }

  inited_ = true;
  LOG_INFO("Successfully created Annoy index, file=%s", file_name_.c_str());
  return RC::SUCCESS;
}

RC IvfflatIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta,
                       span<const FieldMeta> field_metas)
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

  // 计算维度
  const FieldMeta *vector_field_meta = &field_metas[0];
  dimension_ = vector_field_meta->vector_length();
  if (dimension_ <= 0) {
    const int physical_len = vector_field_meta->len();
    if (physical_len != static_cast<int>(sizeof(LobRef)) &&
        physical_len % static_cast<int>(sizeof(float)) == 0) {
      dimension_ = physical_len / static_cast<int>(sizeof(float));
    }
  }

  if (dimension_ <= 0) {
    LOG_WARN("Cannot determine vector dimension when opening index");
    return RC::INVALID_ARGUMENT;
  }

  // 检查索引文件是否存在
  struct stat st;
  if (stat(file_name_.c_str(), &st) != 0) {
    LOG_WARN("Index file does not exist: %s. Index will be marked as not ready.", file_name_.c_str());
    inited_ = false;  // 标记为未就绪
    return RC::SUCCESS;  // 不返回错误，允许表打开
  }

  // 创建Annoy索引对象并加载
  auto *annoy = new AnnoyIndexType<Annoy::Euclidean>(dimension_);
  annoy_index_ = annoy;

  if (!annoy->load(file_name_.c_str())) {
    LOG_WARN("Failed to load Annoy index from file: %s. Index will be marked as not ready.", file_name_.c_str());
    delete annoy;
    annoy_index_ = nullptr;
    inited_ = false;  // 标记为未就绪
    return RC::SUCCESS;  // 不返回错误，允许表打开
  }

  // 加载RID映射
  RC rc = load_rid_mapping();
  if (rc != RC::SUCCESS) {
    LOG_WARN("Failed to load RID mapping. Index will be marked as not ready.");
    delete static_cast<AnnoyIndexType<Annoy::Euclidean> *>(annoy_index_);
    annoy_index_ = nullptr;
    inited_ = false;
    return RC::SUCCESS;  // 不返回错误，允许表打开
  }

  inited_ = true;
  LOG_INFO("Successfully opened Annoy index, file=%s, dimension=%d, items=%d",
           file_name_.c_str(), dimension_, item_count_);
  return RC::SUCCESS;
}

RC IvfflatIndex::close()
{
  if (inited_) {
    LOG_INFO("Closing Annoy index");

    // 释放Annoy索引
    if (annoy_index_) {
      auto *annoy = static_cast<AnnoyIndexType<Annoy::Euclidean> *>(annoy_index_);
      delete annoy;
      annoy_index_ = nullptr;
    }

    // 释放mmap的RID映射
    if (rid_map_mmap_) {
      munmap(rid_map_mmap_, item_count_ * sizeof(RID));
      rid_map_mmap_ = nullptr;
    }

    rid_map_.clear();
    inited_ = false;
  }
  return RC::SUCCESS;
}

RC IvfflatIndex::insert_entry(const char *record, const RID *rid)
{
  // Annoy不支持动态插入，需要重建索引
  LOG_WARN("Annoy index does not support dynamic insertion, please rebuild the index");
  return RC::UNSUPPORTED;
}

RC IvfflatIndex::delete_entry(const char *record, const RID *rid)
{
  // 暂不实现删除功能
  return RC::UNIMPLEMENTED;
}

vector<RID> IvfflatIndex::ann_search(const vector<float> &query_vector, size_t limit)
{
  if (!inited_ || annoy_index_ == nullptr || rid_map_mmap_ == nullptr) {
    LOG_WARN("Index not ready: inited=%d, annoy_index=%p, rid_map_mmap=%p",
             inited_, annoy_index_, rid_map_mmap_);
    return {};
  }

  if (item_count_ == 0) {
    return {};
  }

  if (limit == 0) {
    return {};
  }

  // 限制查询数量不超过实际向量数
  if (limit > static_cast<size_t>(item_count_)) {
    limit = static_cast<size_t>(item_count_);
  }

  if (query_vector.size() != static_cast<size_t>(dimension_)) {
    LOG_WARN("Query vector dimension mismatch: expected=%d, got=%zu", dimension_, query_vector.size());
    return {};
  }

  auto *annoy = static_cast<AnnoyIndexType<Annoy::Euclidean> *>(annoy_index_);

  std::vector<int> result_indices;
  std::vector<float> distances;

  // 使用Annoy搜索
  // search_k: -1表示auto (n_trees * limit)，让Annoy自动计算最优值
  // 硬编码的 probes 值（如5）太小，会导致搜索质量极差，recall=0
  // 使用-1确保足够的搜索质量，达到 recall >0.9
  int search_k = -1;

  annoy->get_nns_by_vector(query_vector.data(), limit, search_k, &result_indices, &distances);

  // 将Annoy的内部索引转换为RID
  vector<RID> result;
  result.reserve(result_indices.size());

  for (int idx : result_indices) {
    if (idx >= 0 && idx < item_count_) {
      result.push_back(rid_map_mmap_[idx]);
    }
  }

  return result;
}

bool IvfflatIndex::ready() const
{
  return inited_ && annoy_index_ != nullptr && rid_map_mmap_ != nullptr &&
         dimension_ > 0 && item_count_ > 0;
}

RC IvfflatIndex::sync()
{
  // Annoy索引已经保存到文件，无需额外同步
  return RC::SUCCESS;
}

IndexScanner *IvfflatIndex::create_scanner(const char *left_key, int left_len, bool left_inclusive,
                                             const char *right_key, int right_len, bool right_inclusive)
{
  // 向量索引不支持范围扫描
  LOG_WARN("IvfflatIndex (Annoy) does not support range scanning");
  return nullptr;
}
