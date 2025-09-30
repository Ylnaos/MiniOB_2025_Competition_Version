#pragma once

#include "storage/index/index.h"

#include <string>
#include <vector>

class Table;

class IvfflatIndex : public Index
{
public:
  IvfflatIndex() = default;
  ~IvfflatIndex() override = default;

  RC create(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas) override;
  RC open(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas) override;
  RC close();

  bool is_vector_index() override { return true; }

  std::vector<RID> ann_search(const std::vector<float> &base_vector, size_t limit);

  RC insert_entry(const char *record, const RID *rid) override;
  RC delete_entry(const char *record, const RID *rid) override;
  RC sync() override;
  IndexScanner *create_scanner(const char *left_key, int left_len, bool left_inclusive, const char *right_key,
      int right_len, bool right_inclusive) override;

private:
  enum class DistanceKind {
    L2,
    COSINE,
    INNER_PRODUCT
  };

  float compute_distance(const float *lhs, const float *rhs) const;
  void  extract_vector(const char *record, std::vector<float> &output) const;
  void  assign_vector(const std::vector<float> &vec, const RID &rid);
  RC    build_from_table(Table *table);
  RC    persist(const char *file_name) const;
  RC    load(const char *file_name);

private:
  int          dimension_      = 0;
  int          lists_          = 1;
  int          probes_         = 1;
  DistanceKind distance_kind_  = DistanceKind::L2;
  Table       *table_          = nullptr;
  std::string  index_file_;
  bool         dirty_          = false;

  std::vector<std::vector<float>> centroids_;
  struct Entry {
    RID              rid;
    std::vector<float> values;
  };
  std::vector<std::vector<Entry>> inverted_lists_;
};
