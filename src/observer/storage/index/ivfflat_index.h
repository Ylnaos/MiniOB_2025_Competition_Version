#pragma once

#include "storage/index/index.h"
#include "storage/index/vector_index_meta.h"  // 包含 DistanceType 定义
#include <fstream>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief ivfflat vector index
 * @ingroup Index
 * @details Annoy-based approximate nearest neighbour index kept under the ivfflat name for compatibility
 */
class IvfflatIndex : public Index
{
public:
  IvfflatIndex() = default;
  virtual ~IvfflatIndex() noexcept;

  /**
   * @brief Build a new Annoy index by scanning table records
   */
  RC create(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas) override;

  /**
   * @brief Open an existing Annoy index and its auxiliary data
   */
  RC open(Table *table, const char *file_name, const IndexMeta &index_meta, span<const FieldMeta> field_metas) override;

  /**
   * @brief Perform ANN search and return the nearest rid list
   */
  vector<RID> ann_search(const vector<float> &query_vector, size_t limit);

  /**
   * @brief Close index resources
   */
  RC close();

  /**
   * @brief Insert vector entry into the index
   */
  RC insert_entry(const char *record, const RID *rid) override;

  /**
   * @brief Remove vector entry from the index (not implemented yet)
   */
  RC delete_entry(const char *record, const RID *rid) override;

  /**
   * @brief Flush index state to disk
   */
  RC sync() override;

  /**
   * @brief Vector index does not support range scanners
   */
  IndexScanner *create_scanner(const char *left_key, int left_len, bool left_inclusive,
                                 const char *right_key, int right_len, bool right_inclusive) override;

  /**
   * @brief Identify this as a vector index
   */
  bool is_vector_index() override { return true; }

  /**
   * @brief Check if the index is initialized and ready for querying
   */
  bool ready() const;

  /**
   * @brief Get configured vector dimension, returns 0 when not ready
   */
  int dimension() const { return dimension_; }

private:
  /**
   * @brief Extract vector data from record buffer
   */
  RC extract_vector_from_record(const char *record, vector<float> &vec) const;

  void apply_meta_config(const IndexMeta &index_meta);

private:
  bool   inited_ = false;
  Table *table_  = nullptr;
  string file_name_;

  // index configuration
  int    lists_      = 100;  // Annoy tree count (n_trees)
  int    probes_     = 10;   // Search depth parameter (search_k)
  int    dimension_  = 0;    // Vector dimension
  string distance_type_;
  string index_type_;
  DistanceType distance_type_cached_ = DistanceType::L2_DISTANCE;  // 缓存解析后的距离类型
  void  *index_       = nullptr;  // Annoy index instance (type erased)
  int    item_count_  = 0;        // Number of vectors stored in the index

  // vector field information
  string       vector_field_name_;  // Field name stored for later lookup
  std::ofstream aux_file_;          // Auxiliary file storing RID mapping
  RID          *rid_map_start_ = nullptr;  // Memory-mapped RID array start

  /**
   * @brief Safely fetch FieldMeta from table
   */
  const FieldMeta *get_vector_field_meta() const;
};
