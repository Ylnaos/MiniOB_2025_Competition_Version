/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/record/lob_handler.h"

#include <sys/stat.h>

RC LobFileHandler::create_file(const char *file_name)
{
  RC rc = file_.create_file(file_name);
  if (OB_SUCC(rc)) {
    append_offset_ = 0;
    append_buffer_.clear();
    append_buffer_.reserve(APPEND_BUFFER_LIMIT);
  }
  return rc;
}

RC LobFileHandler::open_file(const char *file_name)
{
  std::ifstream file(file_name);
  if (file.good()) {
    RC rc = file_.open_file(file_name);
    if (OB_SUCC(rc)) {
      struct stat st;
      if (stat(file_name, &st) != 0) {
        return RC::IOERR_SEEK;
      }
      append_offset_ = st.st_size;
      append_buffer_.clear();
      append_buffer_.reserve(APPEND_BUFFER_LIMIT);
    }
    return rc;
  } else {
    return RC::FILE_NOT_EXIST;
  }
  return RC::INTERNAL;
}

RC LobFileHandler::close_file()
{
  RC rc = flush();
  if (OB_FAIL(rc)) {
    return rc;
  }
  return file_.close_file();
}

RC LobFileHandler::insert_data(int64_t &offset, int64_t length, const char *data)
{
  if (length < 0 || (length > 0 && data == nullptr)) {
    return RC::INVALID_ARGUMENT;
  }

  offset = append_offset_ + static_cast<int64_t>(append_buffer_.size());
  if (length == 0) {
    return RC::SUCCESS;
  }

  if (length > APPEND_BUFFER_LIMIT) {
    RC rc = flush();
    if (OB_FAIL(rc)) {
      return rc;
    }

    int64_t out_size = 0;
    rc = file_.write_at(append_offset_, static_cast<int>(length), data, &out_size);
    if (OB_FAIL(rc)) {
      return rc;
    }
    if (out_size != length) {
      return RC::IOERR_WRITE;
    }
    append_offset_ += length;
    return RC::SUCCESS;
  }

  if (static_cast<int64_t>(append_buffer_.size()) + length > APPEND_BUFFER_LIMIT) {
    RC rc = flush();
    if (OB_FAIL(rc)) {
      return rc;
    }
    offset = append_offset_;
  }

  append_buffer_.append(data, static_cast<size_t>(length));
  return RC::SUCCESS;
}

RC LobFileHandler::get_data(int64_t offset, int64_t length, char *data)
{
  RC rc = flush();
  if (OB_FAIL(rc)) {
    return rc;
  }
  return file_.read_at(offset, length, data);
}

RC LobFileHandler::flush()
{
  if (append_buffer_.empty()) {
    return RC::SUCCESS;
  }

  int64_t out_size = 0;
  RC rc = file_.write_at(append_offset_, static_cast<int>(append_buffer_.size()), append_buffer_.data(), &out_size);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (out_size != static_cast<int64_t>(append_buffer_.size())) {
    return RC::IOERR_WRITE;
  }

  append_offset_ += out_size;
  append_buffer_.clear();
  return RC::SUCCESS;
}
