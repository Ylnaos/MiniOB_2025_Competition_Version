/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include "storage/table/table_engine.h"
#include "storage/index/index.h"
#include "storage/record/record_manager.h"
#include "storage/db/db.h"
#include "oblsm/include/ob_lsm.h"
using namespace oceanbase;

/**
 * @brief lsm table engine
 */
class LsmTableEngine : public TableEngine
{
public:
  LsmTableEngine(TableMeta *table_meta, Db *db, Table *table)
      : TableEngine(table_meta), db_(db), table_(table), lsm_(db->lsm())
  {}
  ~LsmTableEngine() override = default;

  RC insert_record(Record &record) override;
  RC insert_chunk(const Chunk &chunk) override { return RC::UNIMPLEMENTED; }
  RC delete_record(const Record &record) override { return RC::UNIMPLEMENTED; }
  RC insert_record_with_trx(Record &record, Trx *trx) override { return RC::UNIMPLEMENTED; }
  RC delete_record_with_trx(const Record &record, Trx *trx) override { return RC::UNIMPLEMENTED; }
  RC update_record_with_trx(const Record &old_record, const Record &new_record, Trx *trx) override;
  RC get_record(const RID &rid, Record &record) override { return RC::UNIMPLEMENTED; }
  RC create_index(Trx *trx, span<const FieldMeta> field_metas, const char *index_name, bool unique) override;
  RC get_record_scanner(RecordScanner *&scanner, Trx *trx, ReadWriteMode mode) override;
  RC get_chunk_scanner(ChunkFileScanner &scanner, Trx *trx, ReadWriteMode mode) override { return RC::UNIMPLEMENTED; }
  RC visit_record(const RID &rid, function<bool(Record &)> visitor) override { return RC::UNIMPLEMENTED; }
  // TODO:
  RC     sync() override;
  Index *find_index(const char *index_name) const override;
  Index *find_index_by_field(const char *field_name) const override;
  RC     open() override;
  RC     init() override { return RC::UNIMPLEMENTED; }

private:
  // 将 LSM 的自增行键(64-bit) 映射为一个可用于 B+ 树二级索引的 RID。
  static RID rid_from_rowid(uint64_t rowid)
  {
    return RID(static_cast<PageNum>(rowid >> 32), static_cast<SlotNum>(rowid & 0xFFFFFFFFULL));
  }

  // 从 LSM 的内部 key 解码出 64-bit rowid: t/<table_id>/r/<rowid>
  static RC decode_rowid_from_lsm_key(const string &key, uint64_t &rowid);

  RC insert_entry_of_indexes(const char *record, const RID &rid);
  RC delete_entry_of_indexes(const char *record, const RID &rid, bool error_on_not_exists);

  Db              *db_;
  Table           *table_;
  ObLsm           *lsm_;
  atomic<uint64_t> inc_id_{0};

  vector<Index *> indexes_;
};
