#include "storage/index/ivfflat_index.h"
#include "deps/3rd/annoy/src/annoylib.h"
#include "deps/3rd/annoy/src/kissrandom.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "common/lang/string.h"
#include "common/log/log.h"
#include "storage/record/record_manager.h"
#include "storage/record/record_scanner.h"
#include "storage/record/lob_ref.h"
#include "storage/table/table.h"
#include <cerrno>
#include <cstring>
#include <vector>

template <typename T>
using MiniObAnnoyIndex =
    Annoy::AnnoyIndex<int, float, T, Annoy::Kiss32Random, Annoy::AnnoyIndexSingleThreadedBuildPolicy>;

IvfflatIndex::~IvfflatIndex() noexcept
{
  if (index_ != nullptr) {
    switch (distance_type_cached_) {
      case DistanceType::L2_DISTANCE:
        delete static_cast<MiniObAnnoyIndex<Annoy::Euclidean> *>(index_);
        break;
      case DistanceType::COSINE_DISTANCE:
        delete static_cast<MiniObAnnoyIndex<Annoy::Angular> *>(index_);
        break;
      case DistanceType::INNER_PRODUCT:
        delete static_cast<MiniObAnnoyIndex<Annoy::DotProduct> *>(index_);
        break;
      default:
        ASSERT(false, "not implemented");
    }
    index_ = nullptr;
  }
  if (rid_map_start_ != nullptr && item_count_ > 0) {
    munmap(rid_map_start_, sizeof(RID) * item_count_);
    rid_map_start_ = nullptr;
  }
  if (aux_file_.is_open()) {
    aux_file_.close();
  }
}

void IvfflatIndex::apply_meta_config(const IndexMeta &index_meta)
{
  index_type_    = index_meta.index_type();
  distance_type_ = index_meta.distance_type();
  std::string type_upper = index_type_;
  if (!type_upper.empty()) {
    common::str_to_upper(type_upper);
  }
  if (!(type_upper.empty() || type_upper == "IVFFLAT" || type_upper == "ANNOY")) {
    LOG_WARN("Unsupported vector index type %s, fallback to Annoy", index_type_.c_str());
  }
  index_type_ = "ANNOY";
  std::string upper = distance_type_;
  if (!upper.empty()) {
    common::str_to_upper(upper);
  }
  if (upper == "COSINE_DISTANCE" || upper == "COSINE" || upper == "ANGULAR") {
    distance_type_ = "COSINE_DISTANCE";
    distance_type_cached_ = DistanceType::COSINE_DISTANCE;
  } else if (upper == "INNER_PRODUCT" || upper == "DOT" || upper == "DOT_PRODUCT") {
    distance_type_ = "INNER_PRODUCT";
    distance_type_cached_ = DistanceType::INNER_PRODUCT;
  } else {
    distance_type_ = "L2_DISTANCE";
    distance_type_cached_ = DistanceType::L2_DISTANCE;
  }
  lists_ = index_meta.lists();
  if (lists_ <= 0) {
    lists_ = 100;
  }
  probes_ = index_meta.probes();
  if (probes_ <= 0) {
    probes_ = 10;
  }
}

RC IvfflatIndex::create(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas)
{
  if (inited_) return RC::RECORD_OPENNED;
  if (field_metas.size() != 1) return RC::INVALID_ARGUMENT;
  Index::init(index_meta, field_metas);
  table_ = table;
  file_name_ = file_name;
  vector_field_name_ = field_metas[0].name();
  apply_meta_config(index_meta);
  item_count_ = 0;
  rid_map_start_ = nullptr;
  dimension_ = field_metas[0].vector_length();
  if (dimension_ <= 0) {
    const int physical_len = field_metas[0].len();
    if (physical_len != static_cast<int>(sizeof(LobRef)) && physical_len % static_cast<int>(sizeof(float)) == 0) dimension_ = physical_len / static_cast<int>(sizeof(float));
  }
  switch (distance_type_cached_) {
    case DistanceType::L2_DISTANCE: index_ = new MiniObAnnoyIndex<Annoy::Euclidean>(dimension_); break;
    case DistanceType::COSINE_DISTANCE: index_ = new MiniObAnnoyIndex<Annoy::Angular>(dimension_); break;
    case DistanceType::INNER_PRODUCT: index_ = new MiniObAnnoyIndex<Annoy::DotProduct>(dimension_); break;
    default: ASSERT(false, "not implemented");
  }
  aux_file_.open(std::string(file_name_) + ".aux", std::ios::binary | std::ios::trunc);
  if (!aux_file_.is_open()) {
    return RC::IOERR_OPEN;
  }
  RecordScanner *scanner = nullptr;
  RC rc = table_->get_record_scanner(scanner, nullptr, ReadWriteMode::READ_ONLY);
  if (rc != RC::SUCCESS || scanner == nullptr) {
    aux_file_.close();
    return rc != RC::SUCCESS ? rc : RC::INTERNAL;
  }
  Record record;
  while (RC::SUCCESS == scanner->next(record)) {
    std::vector<float> vec;
    if (extract_vector_from_record(record.data(), vec) == RC::SUCCESS) {
      switch (distance_type_cached_) {
        case DistanceType::L2_DISTANCE: static_cast<MiniObAnnoyIndex<Annoy::Euclidean> *>(index_)->add_item(item_count_++, vec.data()); break;
        case DistanceType::COSINE_DISTANCE: static_cast<MiniObAnnoyIndex<Annoy::Angular> *>(index_)->add_item(item_count_++, vec.data()); break;
        case DistanceType::INNER_PRODUCT: static_cast<MiniObAnnoyIndex<Annoy::DotProduct> *>(index_)->add_item(item_count_++, vec.data()); break;
        default: ASSERT(false, "not implemented");
      }
      aux_file_.write(reinterpret_cast<const char *>(&record.rid()), sizeof(RID));
    }
  }
  scanner->close_scan();
  delete scanner;
  if (item_count_ == 0) {
    aux_file_.close();
    inited_ = true;
    return RC::SUCCESS;
  }
  aux_file_.close();
  switch (distance_type_cached_) {
    case DistanceType::L2_DISTANCE: { auto *idx = static_cast<MiniObAnnoyIndex<Annoy::Euclidean> *>(index_); idx->build(lists_); idx->save(file_name_.c_str()); break; }
    case DistanceType::COSINE_DISTANCE: { auto *idx = static_cast<MiniObAnnoyIndex<Annoy::Angular> *>(index_); idx->build(lists_); idx->save(file_name_.c_str()); break; }
    case DistanceType::INNER_PRODUCT: { auto *idx = static_cast<MiniObAnnoyIndex<Annoy::DotProduct> *>(index_); idx->build(lists_); idx->save(file_name_.c_str()); break; }
    default: ASSERT(false, "not implemented");
  }
  const std::string aux_path = std::string(file_name_) + ".aux";
  int fd = ::open(aux_path.c_str(), O_RDONLY);
  if (fd < 0) {
    LOG_WARN("failed to open %s: %s", aux_path.c_str(), strerror(errno));
    return RC::IOERR_OPEN;
  }
  void *map_start = mmap(nullptr, sizeof(RID) * item_count_, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map_start == MAP_FAILED) {
    LOG_WARN("failed to mmap %s: %s", aux_path.c_str(), strerror(errno));
    ::close(fd);
    return RC::IOERR_ACCESS;
  }
  ::close(fd);
  rid_map_start_ = static_cast<RID *>(map_start);
  inited_ = true;
  return RC::SUCCESS;
}

RC IvfflatIndex::open(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas)
{
  if (inited_) return RC::RECORD_OPENNED;
  if (field_metas.size() != 1) return RC::INVALID_ARGUMENT;
  Index::init(index_meta, field_metas);
  table_ = table;
  file_name_ = file_name;
  vector_field_name_ = field_metas[0].name();
  apply_meta_config(index_meta);
  dimension_ = field_metas[0].vector_length();
  if (dimension_ <= 0) {
    const int physical_len = field_metas[0].len();
    if (physical_len != static_cast<int>(sizeof(LobRef)) && physical_len % static_cast<int>(sizeof(float)) == 0) dimension_ = physical_len / static_cast<int>(sizeof(float));
  }
  switch (distance_type_cached_) {
    case DistanceType::L2_DISTANCE: { auto *idx = new MiniObAnnoyIndex<Annoy::Euclidean>(dimension_); idx->load(file_name_.c_str()); item_count_ = idx->get_n_items(); index_ = idx; break; }
    case DistanceType::COSINE_DISTANCE: { auto *idx = new MiniObAnnoyIndex<Annoy::Angular>(dimension_); idx->load(file_name_.c_str()); item_count_ = idx->get_n_items(); index_ = idx; break; }
    case DistanceType::INNER_PRODUCT: { auto *idx = new MiniObAnnoyIndex<Annoy::DotProduct>(dimension_); idx->load(file_name_.c_str()); item_count_ = idx->get_n_items(); index_ = idx; break; }
    default: ASSERT(false, "not implemented");
  }
  if (item_count_ == 0) {
    inited_ = true;
    return RC::SUCCESS;
  }
  const std::string aux_path = std::string(file_name_) + ".aux";
  int fd = ::open(aux_path.c_str(), O_RDONLY);
  if (fd < 0) {
    LOG_WARN("failed to open %s: %s", aux_path.c_str(), strerror(errno));
    return RC::IOERR_OPEN;
  }
  void *map_start = mmap(nullptr, sizeof(RID) * item_count_, PROT_READ, MAP_PRIVATE, fd, 0);
  if (map_start == MAP_FAILED) {
    LOG_WARN("failed to mmap %s: %s", aux_path.c_str(), strerror(errno));
    ::close(fd);
    return RC::IOERR_ACCESS;
  }
  ::close(fd);
  rid_map_start_ = static_cast<RID *>(map_start);
  inited_ = true;
  return RC::SUCCESS;
}

vector<RID> IvfflatIndex::ann_search(const vector<float> &query_vector, size_t limit)
{
  if (!inited_ || item_count_ == 0) {
    return {};
  }
  if (query_vector.size() != static_cast<size_t>(dimension_)) {
    LOG_WARN("Query vector dimension mismatch: expected=%d, got=%zu", dimension_, query_vector.size());
    return {};
  }
  std::vector<int> result_internal;
  std::vector<float> distances;
  switch (distance_type_cached_) {
    case DistanceType::L2_DISTANCE:
      static_cast<MiniObAnnoyIndex<Annoy::Euclidean> *>(index_)->get_nns_by_vector(query_vector.data(), limit, probes_, &result_internal, &distances);
      break;
    case DistanceType::COSINE_DISTANCE:
      static_cast<MiniObAnnoyIndex<Annoy::Angular> *>(index_)->get_nns_by_vector(query_vector.data(), limit, probes_, &result_internal, &distances);
      break;
    case DistanceType::INNER_PRODUCT:
      static_cast<MiniObAnnoyIndex<Annoy::DotProduct> *>(index_)->get_nns_by_vector(query_vector.data(), limit, probes_, &result_internal, &distances);
      break;
    default:
      ASSERT(false, "not implemented");
  }
  vector<RID> results;
  results.reserve(result_internal.size());
  for (const int idx : result_internal) {
    results.push_back(rid_map_start_[idx]);
  }
  return results;
}

RC IvfflatIndex::close() { if (inited_) inited_ = false; return RC::SUCCESS; }

RC IvfflatIndex::insert_entry(const char *, const RID *) { LOG_WARN("Annoy vector index does not support online insertion"); return RC::UNIMPLEMENTED; }

RC IvfflatIndex::delete_entry(const char *, const RID *) { return RC::UNIMPLEMENTED; }

RC IvfflatIndex::sync() { return RC::SUCCESS; }

bool IvfflatIndex::ready() const { return inited_ && index_ != nullptr && item_count_ > 0; }

IndexScanner *IvfflatIndex::create_scanner(const char *, int, bool, const char *, int, bool) { LOG_WARN("IvfflatIndex does not support range scanning"); return nullptr; }

const FieldMeta *IvfflatIndex::get_vector_field_meta() const { return table_ ? table_->table_meta().field(vector_field_name_.c_str()) : nullptr; }

RC IvfflatIndex::extract_vector_from_record(const char *record, vector<float> &vec) const
{
  const FieldMeta *vector_field_meta = get_vector_field_meta();
  if (!vector_field_meta) {
    return RC::INTERNAL;
  }
  const int schema_dim = vector_field_meta->vector_length();
  const bool vector_lob =
      (schema_dim > 1000) || (schema_dim <= 0 && vector_field_meta->len() == static_cast<int>(sizeof(LobRef)));
  const char *payload = nullptr;
  int payload_len = 0;
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
    payload = tmp_buffer.data();
    payload_len = ref.length;
  } else {
    payload = record + vector_field_meta->offset();
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
