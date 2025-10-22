/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/expr/jieba_tokenizer.h"

#include "common/lang/string.h"
#include "common/log/log.h"
#include "common/os/process_param.h"
#include "cppjieba/Jieba.hpp"
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace std;

namespace {

namespace fs = std::filesystem;

static fs::path make_absolute_safely(const fs::path &path)
{
  if (path.empty()) {
    return {};
  }
  fs::path normalized = path.lexically_normal();
  if (normalized.is_absolute()) {
    return normalized;
  }
  std::error_code ec;
  fs::path cwd = fs::current_path(ec);
  if (ec) {
    LOG_WARN("make_absolute_safely: failed to get current path, keeping relative path '%s'", normalized.string().c_str());
    return normalized;
  }
  return (cwd / normalized).lexically_normal();
}

static bool ensure_file_exists(const fs::path &candidate)
{
  struct stat file_stat {};
  std::string file_path = candidate.lexically_normal().string();
  return ::stat(file_path.c_str(), &file_stat) == 0;
}

static bool has_jieba_resource(const fs::path &dir)
{
  if (dir.empty()) {
    return false;
  }

  const fs::path normalized = dir.lexically_normal();
  const std::string dir_str = normalized.string();

  struct stat dir_stat {};
  if (::stat(dir_str.c_str(), &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) {
    return false;
  }

  bool has_dict = ensure_file_exists(normalized / "jieba.dict.utf8");
  bool has_hmm  = ensure_file_exists(normalized / "hmm_model.utf8");
  bool has_user = ensure_file_exists(normalized / "user.dict.utf8");
  bool has_idf  = ensure_file_exists(normalized / "idf.utf8");
  bool has_stop = ensure_file_exists(normalized / "stop_words.utf8");

  if (!has_dict) LOG_WARN("Missing jieba.dict.utf8 in %s", normalized.string().c_str());
  if (!has_hmm) LOG_WARN("Missing hmm_model.utf8 in %s", normalized.string().c_str());
  if (!has_user) LOG_WARN("Missing user.dict.utf8 in %s", normalized.string().c_str());
  if (!has_idf) LOG_WARN("Missing idf.utf8 in %s", normalized.string().c_str());
  if (!has_stop) LOG_WARN("Missing stop_words.utf8 in %s", normalized.string().c_str());

  return has_dict && has_hmm && has_user && has_idf && has_stop;
}

static void push_unique_path(vector<fs::path> &paths, unordered_set<string> &seen, const fs::path &candidate)
{
  fs::path normalized = make_absolute_safely(candidate);
  if (normalized.empty()) {
    return;
  }
  string key = normalized.string();
  if (seen.insert(key).second) {
    paths.emplace_back(std::move(normalized));
  }
}

static fs::path locate_executable_dir()
{
#ifdef _WIN32
  std::array<char, 4096> buffer {};
  DWORD result = GetModuleFileNameA(NULL, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (result == 0 || result >= buffer.size()) {
    return {};
  }
  buffer[static_cast<size_t>(result)] = '\0';
  fs::path exec_path(buffer.data());
  return make_absolute_safely(exec_path).parent_path();
#else
  std::array<char, 4096> buffer {};
  ssize_t captured = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (captured <= 0) {
    return {};
  }
  buffer[static_cast<size_t>(captured)] = '\0';
  fs::path exec_path(buffer.data());
  return make_absolute_safely(exec_path).parent_path();
#endif
}

static fs::path detect_jieba_dict_dir()
{
  vector<fs::path> candidates;
  unordered_set<string> candidate_seen;
  auto add_candidate = [&](const fs::path &path) { push_unique_path(candidates, candidate_seen, path); };

  if (const char *env_dir = std::getenv("MINIOB_JIEBA_DICT_DIR"); env_dir != nullptr && env_dir[0] != '\0') {
    add_candidate(fs::path(env_dir));
  }

  vector<fs::path> base_dirs;
  unordered_set<string> base_seen;
  auto add_base = [&](const fs::path &path) { push_unique_path(base_dirs, base_seen, path); };

  if (const char *miniob_home = std::getenv("MINIOB_HOME"); miniob_home != nullptr && miniob_home[0] != '\0') {
    add_base(fs::path(miniob_home));
  }

  if (auto *proc = common::the_process_param(); proc != nullptr) {
    const string &conf = proc->get_conf();
    if (!conf.empty()) {
      fs::path conf_path = make_absolute_safely(fs::path(conf));
      if (!conf_path.empty()) {
        fs::path dir = conf_path.parent_path();
        while (!dir.empty()) {
          add_base(dir);
          fs::path parent = dir.parent_path();
          if (parent == dir) {
            break;
          }
          dir = parent;
        }
      }
    }
  }

  if (fs::path exec_dir = locate_executable_dir(); !exec_dir.empty()) {
    fs::path dir = exec_dir;
    while (!dir.empty()) {
      add_base(dir);
      fs::path parent = dir.parent_path();
      if (parent == dir) {
        break;
      }
      dir = parent;
    }
  }

  fs::path source_dir = make_absolute_safely(fs::path(__FILE__)).parent_path();
  for (int i = 0; i < 6 && !source_dir.empty(); ++i) {
    add_base(source_dir);
    fs::path parent = source_dir.parent_path();
    if (parent == source_dir) {
      break;
    }
    source_dir = parent;
  }

  for (fs::path current = fs::current_path(); !current.empty();) {
    add_base(current);
    fs::path parent = current.parent_path();
    if (parent == current) {
      break;
    }
    current = parent;
  }

  for (const auto &base : base_dirs) {
    add_candidate(base / "deps/3rd/cppjieba/dict");
  }

  static const char *const relative_dirs[] = {
      "deps/3rd/cppjieba/dict",
      "../deps/3rd/cppjieba/dict",
      "../../deps/3rd/cppjieba/dict",
      "../../../deps/3rd/cppjieba/dict",
      "../../../../deps/3rd/cppjieba/dict"};
  for (const char *rel : relative_dirs) {
    add_candidate(fs::path(rel));
  }

  for (const auto &dir : candidates) {
    LOG_INFO("Checking jieba dict directory: %s", dir.string().c_str());
    if (has_jieba_resource(dir)) {
      LOG_INFO("Found valid jieba dict directory: %s", dir.string().c_str());
      return dir;
    }
  }

  LOG_WARN("Failed to locate jieba dictionary directory. Checked %zu candidates.", candidates.size());
  return {};
}

class TokenizeContext
{
public:
  std::once_flag init_once;
  RC init_rc = RC::SUCCESS;
  std::unique_ptr<cppjieba::Jieba> jieba;
  std::unordered_set<std::string> stop_words;
  fs::path dict_dir;

  RC initialize()
  {
    dict_dir = detect_jieba_dict_dir();
    if (dict_dir.empty()) {
      LOG_WARN("Failed to locate jieba dictionary directory");
      return RC::NOTFOUND;
    }

    const string dict_path = (dict_dir / "jieba.dict.utf8").string();
    const string hmm_path  = (dict_dir / "hmm_model.utf8").string();
    const string user_path = (dict_dir / "user.dict.utf8").string();
    const string idf_path  = (dict_dir / "idf.utf8").string();
    const string stop_path = (dict_dir / "stop_words.utf8").string();

    try {
      jieba = make_unique<cppjieba::Jieba>(dict_path, hmm_path, user_path, idf_path, stop_path);
    } catch (const std::exception &e) {
      LOG_WARN("Failed to initialize jieba tokenizer: %s", e.what());
      return RC::INTERNAL;
    }

    ifstream input(stop_path);
    if (!input.is_open()) {
      LOG_WARN("Failed to open stop_words file: %s", stop_path.c_str());
      return RC::IOERR_OPEN;
    }
    string line;
    while (getline(input, line)) {
      if (!line.empty() && static_cast<unsigned char>(line[0]) == 0xEF && line.size() >= 3 &&
          static_cast<unsigned char>(line[1]) == 0xBB && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
      }
      if (line.empty() || common::is_blank(line.c_str())) {
        continue;
      }
      stop_words.insert(line);
    }
    return RC::SUCCESS;
  }
};

static TokenizeContext &context()
{
  static TokenizeContext ctx;
  return ctx;
}

}  // namespace

RC JiebaTokenizer::tokenize(const string &text, const string &parser, vector<string> &tokens)
{
  LOG_INFO("JiebaTokenizer::tokenize called with text='%s', parser='%s'", text.c_str(), parser.c_str());

  string parser_name = parser;
  if (!parser_name.empty()) {
    common::str_to_lower(parser_name);
  }
  if (!parser_name.empty() && parser_name != "jieba") {
    LOG_WARN("Unsupported full-text parser: %s", parser.c_str());
    return RC::UNIMPLEMENTED;
  }

  LOG_INFO("JiebaTokenizer::tokenize getting context");
  auto &ctx = context();
  std::call_once(ctx.init_once, [&ctx]() {
    LOG_INFO("JiebaTokenizer::tokenize initializing jieba");
    ctx.init_rc = ctx.initialize();
    LOG_INFO("JiebaTokenizer::tokenize initialization completed with rc=%d", static_cast<int>(ctx.init_rc));
  });
  if (ctx.init_rc != RC::SUCCESS) {
    LOG_ERROR("JiebaTokenizer::tokenize initialization failed with rc=%d", static_cast<int>(ctx.init_rc));
    return ctx.init_rc;
  }

  vector<string> raw;
  ctx.jieba->Cut(text, raw, true);

  tokens.clear();
  tokens.reserve(raw.size());
  for (auto &word : raw) {
    if (word.empty()) {
      continue;
    }
    if (common::is_blank(word.c_str())) {
      continue;
    }
    if (ctx.stop_words.find(word) != ctx.stop_words.end()) {
      continue;
    }
    tokens.emplace_back(word);
  }
  return RC::SUCCESS;
}

