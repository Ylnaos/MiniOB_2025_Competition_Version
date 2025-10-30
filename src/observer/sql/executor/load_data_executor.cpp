/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2023/7/12.
//

#include "sql/executor/load_data_executor.h"
#include "common/lang/string.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "sql/executor/sql_result.h"
#include "sql/stmt/load_data_stmt.h"
#include "storage/common/chunk.h"

using namespace common;

/**
 * RFC 4180 兼容的 CSV 解析器
 * 支持：
 * - ENCLOSED BY（字段引号）
 * - TERMINATED BY（字段分隔符）
 * - 引号内的引号（双引号转义）
 * - 引号内的换行符和分隔符
 *
 * @param input 输入流
 * @param row 输出：解析的一行数据
 * @param terminated 字段分隔符
 * @param enclosed 字段引号字符
 * @return true 如果成功读取一行，false 如果到达文件末尾
 */
bool parse_csv_line(std::istream &input, std::vector<std::string> &row, char terminated, char enclosed)
{
  row.clear();

  if (input.eof()) {
    return false;
  }

  std::string field;
  field.reserve(256);  // 预分配空间优化性能

  enum class State {
    NORMAL,            // 正常状态（未引号内）
    IN_QUOTED,         // 在引号内
    QUOTE_IN_QUOTED    // 在引号内遇到引号（可能是转义或结束）
  };

  State state = State::NORMAL;
  char ch;
  bool has_content = false;

  while (input.get(ch)) {
    has_content = true;

    switch (state) {
      case State::NORMAL:
        if (ch == enclosed && field.empty()) {
          // 字段开始处的引号，进入引号模式
          state = State::IN_QUOTED;
        } else if (ch == terminated) {
          // 遇到分隔符，保存当前字段
          row.push_back(field);
          field.clear();
        } else if (ch == '\n') {
          // 遇到换行符，行结束
          row.push_back(field);
          return true;
        } else if (ch == '\r') {
          // 忽略 \r（处理 Windows 换行符 \r\n）
          // 继续读取，期待下一个字符是 \n
        } else {
          field += ch;
        }
        break;

      case State::IN_QUOTED:
        if (ch == enclosed) {
          // 在引号内遇到引号，可能是转义或字段结束
          state = State::QUOTE_IN_QUOTED;
        } else {
          // 引号内的普通字符（包括换行符和分隔符）
          field += ch;
        }
        break;

      case State::QUOTE_IN_QUOTED:
        if (ch == enclosed) {
          // 两个连续引号，转义为一个引号
          field += enclosed;
          state = State::IN_QUOTED;
        } else if (ch == terminated) {
          // 引号后紧跟分隔符，字段结束
          row.push_back(field);
          field.clear();
          state = State::NORMAL;
        } else if (ch == '\n') {
          // 引号后紧跟换行符，行结束
          row.push_back(field);
          return true;
        } else if (ch == '\r') {
          // 忽略 \r，等待 \n
          state = State::QUOTE_IN_QUOTED;
        } else {
          // 引号后跟其他字符（RFC 4180 不推荐，但我们容错处理）
          field += ch;
          state = State::NORMAL;
        }
        break;
    }
  }

  // 文件结束，保存最后一个字段
  if (has_content) {
    row.push_back(field);
    return true;
  }

  return false;
}

RC LoadDataExecutor::execute(SQLStageEvent *sql_event)
{
  RC            rc         = RC::SUCCESS;
  SqlResult    *sql_result = sql_event->session_event()->sql_result();
  LoadDataStmt *stmt       = static_cast<LoadDataStmt *>(sql_event->stmt());
  Table        *table      = stmt->table();
  const char   *file_name  = stmt->filename();
  load_data(table, file_name, stmt->terminated(), stmt->enclosed(), sql_result);
  return rc;
}

/**
 * 从文件中导入数据时使用。尝试向表中插入解析后的一行数据。
 * @param table  要导入的表
 * @param file_values 从文件中读取到的一行数据，使用分隔符拆分后的几个字段值
 * @param record_values Table::insert_record使用的参数，为了防止频繁的申请内存
 * @param errmsg 如果出现错误，通过这个参数返回错误信息
 * @return 成功返回RC::SUCCESS
 */
RC insert_record_from_file(
    Table *table, vector<string> &file_values, vector<Value> &record_values, stringstream &errmsg)
{

  const int field_num     = record_values.size();
  const int sys_field_num = table->table_meta().sys_field_num();

  if (file_values.size() < record_values.size()) {
    return RC::SCHEMA_FIELD_MISSING;
  }

  RC rc = RC::SUCCESS;

  stringstream deserialize_stream;
  for (int i = 0; i < field_num && RC::SUCCESS == rc; i++) {
    const FieldMeta *field = table->table_meta().field(i + sys_field_num);

    string &file_value = file_values[i];
    if (!is_string_type(field->type())) {
      common::strip(file_value);
    }
    rc = DataType::type_instance(field->type())->set_value_from_str(record_values[i], file_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("Failed to deserialize value from string: %s, type=%d", file_value.c_str(), field->type());
      return rc;
    }
  }

  if (RC::SUCCESS == rc) {
    Record record;
    rc = table->make_record(field_num, record_values.data(), record);
    if (rc != RC::SUCCESS) {
      errmsg << "insert failed.";
    } else if (RC::SUCCESS != (rc = table->insert_record(record))) {
      errmsg << "insert failed.";
    }
  }
  return rc;
}


// TODO: pax format and row format
void LoadDataExecutor::load_data(Table *table, const char *file_name, char terminated, char enclosed, SqlResult *sql_result)
{
  stringstream result_string;

  fstream fs;
  fs.open(file_name, ios_base::in | ios_base::binary);
  if (!fs.is_open()) {
    sql_result->set_return_code(RC::FILE_NOT_EXIST);
    sql_result->set_state_string(result_string.str());
    return;
  }

  struct timespec begin_time;
  clock_gettime(CLOCK_MONOTONIC, &begin_time);
  const int field_num     = table->table_meta().visible_field_num();
  const int sys_field_num = table->table_meta().sys_field_num();

  vector<string>      file_values;
  int                 line_num        = 0;
  int                 insertion_count = 0;
  RC                  rc              = RC::SUCCESS;

  if (table->table_meta().storage_format() == StorageFormat::PAX_FORMAT) {
    // PAX 格式：批量插入优化
    const int BATCH_SIZE = 1000;  // 每批处理的行数
    Chunk chunk;

    // 初始化 Chunk 的列
    for (int i = 0; i < field_num; ++i) {
      const FieldMeta *field = table->table_meta().field(i + sys_field_num);
      auto col = make_unique<Column>(field->type(), field->len(), BATCH_SIZE);
      chunk.add_column(std::move(col), field->field_id());
    }

    vector<Value> record_values(field_num);
    int batch_count = 0;
    int parse_failed_count = 0;  // 解析失败的行数
    int insert_failed_count = 0; // 插入失败的行数

    while (parse_csv_line(fs, file_values, terminated, enclosed)) {
      line_num++;

      if (file_values.size() < static_cast<size_t>(field_num)) {
        LOG_WARN("Line %d: insufficient fields, expected=%d, got=%zu",
                 line_num, field_num, file_values.size());
        parse_failed_count++;
        continue;
      }

      // 解析每个字段
      bool parse_success = true;
      int failed_field_idx = -1;
      RC failed_rc = RC::SUCCESS;
      for (int i = 0; i < field_num && parse_success; i++) {
        const FieldMeta *field = table->table_meta().field(i + sys_field_num);
        string &file_value = file_values[i];
        if (!is_string_type(field->type())) {
          common::strip(file_value);
        }
        rc = DataType::type_instance(field->type())->set_value_from_str(record_values[i], file_value);
        if (rc != RC::SUCCESS) {
          parse_success = false;
          failed_field_idx = i;
          failed_rc = rc;
        }
      }

      if (!parse_success) {
        parse_failed_count++;
        const FieldMeta *failed_field = table->table_meta().field(failed_field_idx + sys_field_num);
        // 每1000个错误记录一次日志，避免日志过多
        if (parse_failed_count <= 10 || parse_failed_count % 1000 == 0) {
          LOG_WARN("Line %d: failed to parse field '%s' (type=%d), value='%s', error=%s",
                   line_num, failed_field->name(), failed_field->type(),
                   file_values[failed_field_idx].c_str(), strrc(failed_rc));
        }
        continue;
      }

      // 将数据追加到 Chunk
      for (int i = 0; i < field_num; i++) {
        chunk.column(i).append_value(record_values[i]);
      }
      batch_count++;

      // 当达到批量大小或文件结束时，执行批量插入
      if (batch_count >= BATCH_SIZE) {
        rc = table->insert_chunk(chunk);
        if (rc != RC::SUCCESS) {
          LOG_ERROR("Failed to insert chunk at line %d, batch_count=%d, error=%s",
                    line_num, batch_count, strrc(rc));
          insert_failed_count += batch_count;
          break;
        }
        insertion_count += batch_count;
        batch_count = 0;
        chunk.reset_data();
      }
    }

    // 插入剩余的数据
    if (batch_count > 0 && rc == RC::SUCCESS) {
      rc = table->insert_chunk(chunk);
      if (rc == RC::SUCCESS) {
        insertion_count += batch_count;
      } else {
        LOG_ERROR("Failed to insert final chunk, batch_count=%d, error=%s",
                  batch_count, strrc(rc));
        insert_failed_count += batch_count;
      }
    }

    // 输出详细统计信息
    LOG_INFO("Load data statistics: total_lines=%d, inserted=%d, parse_failed=%d, insert_failed=%d",
             line_num, insertion_count, parse_failed_count, insert_failed_count);
  } else {
    // ROW 格式：逐行插入
    vector<Value> record_values(field_num);

    while (parse_csv_line(fs, file_values, terminated, enclosed)) {
      line_num++;

      stringstream errmsg;
      rc = insert_record_from_file(table, file_values, record_values, errmsg);
      if (rc != RC::SUCCESS) {
        break;
      }
      insertion_count++;
    }
  }

  fs.close();

  struct timespec end_time;
  clock_gettime(CLOCK_MONOTONIC, &end_time);
  double elapsed_time = (end_time.tv_sec - begin_time.tv_sec) +
                        (end_time.tv_nsec - begin_time.tv_nsec) / 1000000000.0;

  // 成功时不输出额外信息，只通过RC状态码返回
  LOG_INFO("load data done. row num: %d, result: %s, time: %.2fs", insertion_count, strrc(rc), elapsed_time);
  sql_result->set_return_code(rc);
  sql_result->set_state_string(result_string.str());
}
