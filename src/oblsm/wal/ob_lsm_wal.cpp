/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
   miniob is licensed under Mulan PSL v2.
   You can use this software according to the terms and conditions of the Mulan PSL v2.
   You may obtain a copy of Mulan PSL v2 at:
            http://license.coscl.org.cn/MulanPSL2
   THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
   EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
   MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
   See the Mulan PSL v2 for more details. */

#include "oblsm/wal/ob_lsm_wal.h"
#include "common/log/log.h"
#include "common/lang/filesystem.h"
#include "oblsm/util/ob_coding.h"
#include "oblsm/util/ob_file_reader.h"

namespace oceanbase {
RC WAL::open(const std::string &filename)
{
  lock_guard<mutex> lock(mutex_);
  filename_ = filename;
  writer_   = ObFileWriter::create_file_writer(filename_, true);
  if (writer_ == nullptr) {
    LOG_WARN("Failed to open wal file %s", filename_.c_str());
    return RC::IOERR_OPEN;
  }
  return RC::SUCCESS;
}

RC WAL::recover(const std::string &wal_file, std::vector<WalRecord> &wal_records)
{
  if (!filesystem::exists(wal_file)) {
    return RC::SUCCESS;
  }

  auto reader = ObFileReader::create_file_reader(wal_file);
  if (reader == nullptr) {
    return RC::IOERR_OPEN;
  }

  uint32_t pos       = 0;
  uint32_t file_size = reader->file_size();
  while (pos < file_size) {
    if (file_size - pos < sizeof(uint64_t) + sizeof(size_t)) {
      return RC::IOERR_READ;
    }

    string seq_data = reader->read_pos(pos, sizeof(uint64_t));
    if (seq_data.size() != sizeof(uint64_t)) {
      return RC::IOERR_READ;
    }
    uint64_t seq = get_numeric<uint64_t>(seq_data.data());
    pos += sizeof(uint64_t);

    string key_len_data = reader->read_pos(pos, sizeof(size_t));
    if (key_len_data.size() != sizeof(size_t)) {
      return RC::IOERR_READ;
    }
    size_t key_len = get_numeric<size_t>(key_len_data.data());
    pos += sizeof(size_t);
    if (key_len > file_size - pos) {
      return RC::IOERR_READ;
    }

    string key = reader->read_pos(pos, static_cast<uint32_t>(key_len));
    if (key.size() != key_len) {
      return RC::IOERR_READ;
    }
    pos += key_len;

    if (file_size - pos < sizeof(size_t)) {
      return RC::IOERR_READ;
    }
    string val_len_data = reader->read_pos(pos, sizeof(size_t));
    if (val_len_data.size() != sizeof(size_t)) {
      return RC::IOERR_READ;
    }
    size_t val_len = get_numeric<size_t>(val_len_data.data());
    pos += sizeof(size_t);
    if (val_len > file_size - pos) {
      return RC::IOERR_READ;
    }

    string val = reader->read_pos(pos, static_cast<uint32_t>(val_len));
    if (val.size() != val_len) {
      return RC::IOERR_READ;
    }
    pos += val_len;

    wal_records.emplace_back(seq, std::move(key), std::move(val));
  }

  return RC::SUCCESS;
}

RC WAL::put(uint64_t seq, string_view key, string_view val)
{
  string record;
  put_numeric<uint64_t>(&record, seq);
  put_numeric<size_t>(&record, key.size());
  record.append(key.data(), key.size());
  put_numeric<size_t>(&record, val.size());
  record.append(val.data(), val.size());

  lock_guard<mutex> lock(mutex_);
  if (writer_ == nullptr) {
    return RC::IOERR_WRITE;
  }
  return writer_->write(record);
}

RC WAL::sync()
{
  lock_guard<mutex> lock(mutex_);
  if (writer_ == nullptr) {
    return RC::IOERR_SYNC;
  }
  return writer_->flush();
}
}  // namespace oceanbase
