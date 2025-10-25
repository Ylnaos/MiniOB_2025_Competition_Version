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
// Created by Wangyunlai on 2024/01/10.
//

#include "net/sql_task_handler.h"
#include "common/lang/string.h"
#include "net/communicator.h"
#include "event/session_event.h"
#include "event/sql_event.h"
#include "event/sql_debug.h"
#include "session/session.h"
#include <vector>

using namespace std;
using namespace common;

namespace {
vector<string> split_sql_statements(const string &sql)
{
  vector<string> results;
  string         current;
  current.reserve(sql.size());
  bool in_single        = false;
  bool in_double        = false;
  bool in_line_comment  = false;
  bool in_block_comment = false;

  auto emit = [&](string &buffer) {
    if (buffer.empty()) {
      return;
    }
    string trimmed = buffer;
    strip(trimmed);
    if (!trimmed.empty()) {
      results.emplace_back(std::move(trimmed));
    }
    buffer.clear();
  };

  const size_t len = sql.size();
  for (size_t i = 0; i < len; ++i) {
    char c    = sql[i];
    char next = (i + 1 < len) ? sql[i + 1] : '\0';

    if (in_line_comment) {
      current.push_back(c);
      if (c == '\n') {
        in_line_comment = false;
      }
      continue;
    }

    if (in_block_comment) {
      current.push_back(c);
      if (c == '*' && next == '/') {
        current.push_back(next);
        ++i;
        in_block_comment = false;
      }
      continue;
    }

    if (!in_single && !in_double) {
      if (c == '-' && next == '-') {
        current.push_back(c);
        current.push_back(next);
        ++i;
        in_line_comment = true;
        continue;
      }
      if (c == '/' && next == '*') {
        current.push_back(c);
        current.push_back(next);
        ++i;
        in_block_comment = true;
        continue;
      }
    }

    if (c == '\'' && !in_double) {
      current.push_back(c);
      if (in_single && next == '\'') {
        current.push_back(next);
        ++i;
      } else {
        in_single = !in_single;
      }
      continue;
    }

    if (c == '"' && !in_single) {
      current.push_back(c);
      if (in_double && next == '"') {
        current.push_back(next);
        ++i;
      } else {
        in_double = !in_double;
      }
      continue;
    }

    if (c == ';' && !in_single && !in_double) {
      emit(current);
      continue;
    }

    current.push_back(c);
  }

  if (!current.empty()) {
    emit(current);
  }

  return results;
}
}  // namespace

RC SqlTaskHandler::handle_event(Communicator *communicator)
{
  SessionEvent *event = nullptr;
  RC            rc    = communicator->read_event(event);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (nullptr == event) {
    return RC::SUCCESS;
  }

  vector<string> statements = split_sql_statements(event->query());
  if (statements.empty()) {
    statements.emplace_back(event->query());
  }

  RC  last_rc         = RC::SUCCESS;
  bool need_disconnect = false;

  for (size_t idx = 0; idx < statements.size(); ++idx) {
    const string &sql_text = statements[idx];
    if (is_blank(sql_text.c_str())) {
      continue;
    }

    event->set_query(sql_text);
    Session::set_current_session(event->session());
    event->session()->set_current_request(event);

    SQLStageEvent sql_event(event, sql_text);
    RC           exec_rc = handle_sql(&sql_event);
    if (OB_FAIL(exec_rc)) {
      LOG_TRACE("failed to handle sql. rc=%s", strrc(exec_rc));
      event->sql_result()->set_return_code(exec_rc);
    }

    bool write_disconnect = false;
    RC   write_rc         = communicator->write_result(event, write_disconnect);
    LOG_INFO("write result return %s", strrc(write_rc));

    if (OB_FAIL(write_rc) && OB_SUCC(last_rc)) {
      last_rc = write_rc;
    }
    if (write_disconnect) {
      need_disconnect = true;
    }

    event->session()->set_current_request(nullptr);
    Session::set_current_session(nullptr);

    if (need_disconnect) {
      break;
    }

    if (idx + 1 < statements.size()) {
      event->sql_result()->reset();
      event->sql_debug().clear_debug_info();
    }
  }

  delete event;

  if (need_disconnect) {
    return RC::INTERNAL;
  }
  return last_rc;
}

RC SqlTaskHandler::handle_sql(SQLStageEvent *sql_event)
{
  RC rc = query_cache_stage_.handle_request(sql_event);
  if (OB_FAIL(rc)) {
    LOG_TRACE("failed to do query cache. rc=%s", strrc(rc));
    return rc;
  }

  rc = parse_stage_.handle_request(sql_event);
  if (OB_FAIL(rc)) {
    LOG_TRACE("failed to do parse. rc=%s", strrc(rc));
    return rc;
  }

  rc = resolve_stage_.handle_request(sql_event);
  if (OB_FAIL(rc)) {
    LOG_TRACE("failed to do resolve. rc=%s", strrc(rc));
    return rc;
  }

  rc = optimize_stage_.handle_request(sql_event);
  if (rc != RC::UNIMPLEMENTED && rc != RC::SUCCESS) {
    LOG_TRACE("failed to do optimize. rc=%s", strrc(rc));
    return rc;
  }

  rc = execute_stage_.handle_request(sql_event);
  if (OB_FAIL(rc)) {
    LOG_TRACE("failed to do execute. rc=%s", strrc(rc));
    return rc;
  }

  return rc;
}
