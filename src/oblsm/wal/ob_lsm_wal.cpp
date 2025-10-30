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
#include "oblsm/util/ob_file_reader.h"

namespace oceanbase {
RC WAL::open(const std::string &filename)
{
  if (file_writer_ != nullptr) {
    file_writer_.reset();
  }

  unique_ptr<ObFileWriter> writer = ObFileWriter::create_file_writer(filename, true);
  if (writer == nullptr) {
    LOG_WARN("failed to create wal file writer. filename=%s", filename.c_str());
    return RC::IOERR_OPEN;
  }

  filename_ = filename;
  file_writer_ = std::move(writer);
  return RC::SUCCESS;
}

RC WAL::recover(const std::string &wal_file, std::vector<WalRecord> &wal_records)
{
  wal_records.clear();

  if (wal_file.empty()) {
    LOG_WARN("wal file name is empty");
    return RC::WAL_INVALID_FILENAME;
  }

  ifstream file_check(wal_file, ios::binary);
  if (!file_check.good()) {
    LOG_WARN("wal file does not exist. filename=%s", wal_file.c_str());
    return RC::FILE_NOT_EXIST;
  }
  file_check.close();

  auto reader = ObFileReader::create_file_reader(wal_file);
  if (reader == nullptr) {
    LOG_WARN("failed to create wal file reader. filename=%s", wal_file.c_str());
    return RC::IOERR_OPEN;
  }

  reader->close_file();
  RC rc = reader->open_file();
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open wal file. filename=%s, rc=%s", wal_file.c_str(), strrc(rc));
    return rc == RC::INTERNAL ? RC::IOERR_OPEN : rc;
  }

  const uint32_t file_size = reader->file_size();
  uint32_t       pos       = 0;

  while (pos < file_size) {
    if (file_size - pos < sizeof(uint64_t)) {
      LOG_WARN("incomplete wal record for seq. filename=%s, pos=%u", wal_file.c_str(), pos);
      reader->close_file();
      return RC::IOERR_READ;
    }
    string seq_buf = reader->read_pos(pos, sizeof(uint64_t));
    if (seq_buf.size() != sizeof(uint64_t)) {
      LOG_WARN("failed to read seq from wal file. filename=%s, pos=%u", wal_file.c_str(), pos);
      reader->close_file();
      return RC::IOERR_READ;
    }
    uint64_t seq = 0;
    memcpy(&seq, seq_buf.data(), sizeof(uint64_t));
    pos += sizeof(uint64_t);

    if (file_size - pos < sizeof(size_t)) {
      LOG_WARN("incomplete wal record for key length. filename=%s, pos=%u", wal_file.c_str(), pos);
      reader->close_file();
      return RC::IOERR_READ;
    }
    string key_len_buf = reader->read_pos(pos, sizeof(size_t));
    if (key_len_buf.size() != sizeof(size_t)) {
      LOG_WARN("failed to read key length from wal file. filename=%s, pos=%u", wal_file.c_str(), pos);
      reader->close_file();
      return RC::IOERR_READ;
    }
    size_t key_len = 0;
    memcpy(&key_len, key_len_buf.data(), sizeof(size_t));
    pos += sizeof(size_t);

    if (file_size - pos < key_len) {
      LOG_WARN("invalid key length in wal file. filename=%s, key_len=%zu, pos=%u", wal_file.c_str(), key_len, pos);
      reader->close_file();
      return RC::IOERR_READ;
    }
    string key = reader->read_pos(pos, static_cast<uint32_t>(key_len));
    if (key.size() != key_len) {
      LOG_WARN("failed to read key from wal file. filename=%s, pos=%u, key_len=%zu", wal_file.c_str(), pos, key_len);
      reader->close_file();
      return RC::IOERR_READ;
    }
    pos += static_cast<uint32_t>(key_len);

    if (file_size - pos < sizeof(size_t)) {
      LOG_WARN("incomplete wal record for value length. filename=%s, pos=%u", wal_file.c_str(), pos);
      reader->close_file();
      return RC::IOERR_READ;
    }
    string val_len_buf = reader->read_pos(pos, sizeof(size_t));
    if (val_len_buf.size() != sizeof(size_t)) {
      LOG_WARN("failed to read value length from wal file. filename=%s, pos=%u", wal_file.c_str(), pos);
      reader->close_file();
      return RC::IOERR_READ;
    }
    size_t val_len = 0;
    memcpy(&val_len, val_len_buf.data(), sizeof(size_t));
    pos += sizeof(size_t);

    if (file_size - pos < val_len) {
      LOG_WARN("invalid value length in wal file. filename=%s, val_len=%zu, pos=%u", wal_file.c_str(), val_len, pos);
      reader->close_file();
      return RC::IOERR_READ;
    }
    string val = reader->read_pos(pos, static_cast<uint32_t>(val_len));
    if (val.size() != val_len) {
      LOG_WARN("failed to read value from wal file. filename=%s, pos=%u, val_len=%zu", wal_file.c_str(), pos, val_len);
      reader->close_file();
      return RC::IOERR_READ;
    }
    pos += static_cast<uint32_t>(val_len);

    wal_records.emplace_back(seq, std::move(key), std::move(val));
  }

  reader->close_file();
  return RC::SUCCESS;
}

RC WAL::put(uint64_t seq, string_view key, string_view val)
{
  if (file_writer_ == nullptr || !file_writer_->is_open()) {
    LOG_WARN("wal file not open for write");
    return RC::FILE_NOT_OPENED;
  }

  size_t key_len = key.size();
  size_t val_len = val.size();
  const size_t total_size = sizeof(seq) + sizeof(key_len) + key_len + sizeof(val_len) + val_len;

  string buffer;
  buffer.reserve(total_size);
  buffer.append(reinterpret_cast<const char *>(&seq), sizeof(seq));
  buffer.append(reinterpret_cast<const char *>(&key_len), sizeof(key_len));
  buffer.append(key.data(), key_len);
  buffer.append(reinterpret_cast<const char *>(&val_len), sizeof(val_len));
  buffer.append(val.data(), val_len);

  RC rc = file_writer_->write(string_view(buffer.data(), buffer.size()));
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to write wal record. filename=%s, rc=%d", filename_.c_str(), rc);
  }
  return rc;
}

RC WAL::sync()
{
  if (file_writer_ == nullptr || !file_writer_->is_open()) {
    LOG_WARN("wal file not open for sync");
    return RC::FILE_NOT_OPENED;
  }

  RC rc = file_writer_->flush();
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to flush wal file. filename=%s, rc=%d", filename_.c_str(), rc);
  }
  return rc;
}
}  // namespace oceanbase
