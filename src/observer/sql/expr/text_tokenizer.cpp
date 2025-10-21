/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/expr/text_tokenizer.h"

#include "common/lang/string.h"
#include "common/log/log.h"
#include "common/os/process_param.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using namespace std;

#if defined(__has_include)
#  if __has_include("cppjieba/Jieba.hpp")
#    define MINIOB_WITH_JIEBA 1
#    include "cppjieba/Jieba.hpp"
#  else
#    define MINIOB_WITH_JIEBA 0
#  endif
#else
#  define MINIOB_WITH_JIEBA 0
#endif

namespace {

static size_t utf8_char_length(unsigned char ch)
{
  if ((ch & 0x80) == 0) {
    return 1;
  }
  if ((ch & 0xE0) == 0xC0) {
    return 2;
  }
  if ((ch & 0xF0) == 0xE0) {
    return 3;
  }
  if ((ch & 0xF8) == 0xF0) {
    return 4;
  }
  return 1;
}

static bool is_cjk_punctuation(string_view glyph)
{
  struct Item {
    const char *data;
    size_t len;
  };
  static const Item puncts[] = {
      {"\xEF\xBC\x8C", 3}, {"\xE3\x80\x82", 3}, {"\xEF\xBC\x81", 3}, {"\xEF\xBC\x9F", 3},
      {"\xE3\x80\x81", 3}, {"\xEF\xBC\x9B", 3}, {"\xEF\xBC\x9A", 3}, {"\xEF\xBC\x88", 3},
      {"\xEF\xBC\x89", 3}, {"\xE3\x80\x90", 3}, {"\xE3\x80\x91", 3}, {"\xE3\x80\x8A", 3},
      {"\xE3\x80\x8B", 3}, {"\xE3\x80\x88", 3}, {"\xE3\x80\x89", 3}, {"\xE2\x80\x9C", 3},
      {"\xE2\x80\x9D", 3}, {"\xE2\x80\x98", 3}, {"\xE2\x80\x99", 3}, {"\xE2\x80\x94", 3},
      {"\xEF\xBC\x8D", 3}, {"\xE2\x80\xA6", 3}, {"\xC2\xB7", 2}, {"\xEF\xBC\x8F", 3},
      {"\xEF\xBD\x9C", 3}};
  for (const auto &item : puncts) {
    if (glyph.size() == item.len && std::memcmp(glyph.data(), item.data, item.len) == 0) {
      return true;
    }
  }
  return false;
}

static const unordered_set<string> &builtin_stop_words()
{
  static const unordered_set<string> words = {
      "\xE5\x9C\xA8", // 在
      "\xE4\xB8\xBA", // 为
      "\xE6\x98\xAF", // 是
      "\xE5\x92\x8C", // 和
      "\xE5\x8F\x8A", // 及
      "\xE4\xB8\x8E", // 与
      "\xE6\x88\x96", // 或
      "\xE5\x93\xAA\xE4\xBA\x9B", // 哪些
      "\xE9\x82\xA3\xE4\xBA\x9B", // 那些
      "\xE8\xBF\x99\xE4\xBA\x9B", // 这些
      "\xE4\xBB\x80\xE4\xB9\x88", // 什么
      "\xE5\x88\x86\xE5\x88\xAB", // 分别
      "\xE5\x9B\xA0\xE4\xB8\xBA", // 因为
      "\xE4\xBA\x8E", // 于
      "\xE5\x91\x80", // 呀
      "\xE5\x90\x97"  // 吗
  };
  return words;
}

static const unordered_set<string> &builtin_cjk_dictionary()
{
  static const unordered_set<string> dict = {
      "\xE8\xA7\x86\xE5\x9B\xBE", // 视图
      "\xE5\xAD\x97\xE6\xAE\xB5", // 字段
      "\xE4\xB8\xAD",             // 中
      "\xE4\xB8\x8D\xE8\x83\xBD", // 不能
      "\xE4\xBB\xA3\xE8\xA1\xA8", // 代表
      "\xE4\xBF\xA1\xE6\x81\xAF"  // 信息
  };
  return dict;
}

static void split_cjk_sequence(const string &seq, vector<string> &out)
{
  if (seq.empty()) {
    return;
  }
  vector<string> chars;
  chars.reserve(seq.size());
  for (size_t i = 0; i < seq.size();) {
    unsigned char ch = static_cast<unsigned char>(seq[i]);
    size_t len       = utf8_char_length(ch);
    if (len == 0) {
      len = 1;
    }
    if (i + len > seq.size()) {
      len = seq.size() - i;
    }
    chars.emplace_back(seq.substr(i, len));
    i += len;
  }

  const auto &dict = builtin_cjk_dictionary();
  size_t      idx  = 0;
  while (idx < chars.size()) {
    string candidate;
    string best_match;
    size_t best_len = 0;
    for (size_t k = idx; k < chars.size(); ++k) {
      candidate.append(chars[k]);
      if (dict.find(candidate) != dict.end()) {
        best_match = candidate;
        best_len   = k - idx + 1;
      }
    }
    if (best_len > 0) {
      out.emplace_back(std::move(best_match));
      idx += best_len;
    } else {
      out.emplace_back(chars[idx]);
      idx += 1;
    }
  }
}

static void split_token_into_units(const string &token, vector<string> &out)
{
  enum class Group { None, Latin, Chinese };
  Group current_group = Group::None;
  string buffer;

  auto flush = [&](Group group) {
    if (buffer.empty()) {
      return;
    }
    if (group == Group::Chinese) {
      split_cjk_sequence(buffer, out);
    } else {
      out.emplace_back(buffer);
    }
    buffer.clear();
  };

  for (size_t i = 0; i < token.size();) {
    unsigned char ch = static_cast<unsigned char>(token[i]);
    if (ch < 0x80) {
      if (std::isspace(ch) || ch == '_' || std::ispunct(ch)) {
        flush(current_group);
        current_group = Group::None;
        ++i;
        continue;
      }
      Group new_group = Group::Latin;
      if (new_group != current_group) {
        flush(current_group);
        current_group = new_group;
      }
      buffer.push_back(static_cast<char>(ch));
      ++i;
      continue;
    }

    size_t len = utf8_char_length(ch);
    if (len == 0) {
      ++i;
      continue;
    }
    if (i + len > token.size()) {
      len = token.size() - i;
    }
    string_view glyph(token.data() + i, len);
    if (is_cjk_punctuation(glyph)) {
      flush(current_group);
      current_group = Group::None;
      i += len;
      continue;
    }

    Group new_group = Group::Chinese;
    if (new_group != current_group) {
      flush(current_group);
      current_group = new_group;
    }
    buffer.append(token, i, len);
    i += len;
  }

  flush(current_group);
}

static void refine_tokens_from_raw(const vector<string> &raw,
                                   const unordered_set<string> *stop_words,
                                   vector<string> &tokens)
{
  tokens.clear();
  vector<string> pieces;
  pieces.reserve(16);
  const auto &fallback_stop = builtin_stop_words();

  for (const string &word : raw) {
    if (word.empty()) {
      continue;
    }
    split_token_into_units(word, pieces);
    for (string &piece : pieces) {
      if (piece.empty()) {
        continue;
      }
      if (common::is_blank(piece.c_str())) {
        continue;
      }
      bool is_stop = false;
      if (stop_words != nullptr && stop_words->find(piece) != stop_words->end()) {
        is_stop = true;
      }
      if (!is_stop && fallback_stop.find(piece) != fallback_stop.end()) {
        is_stop = true;
      }
      if (!is_stop) {
        tokens.emplace_back(std::move(piece));
      }
    }
    pieces.clear();
  }
}

#if MINIOB_WITH_JIEBA
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
  fs::path absolute_path = fs::absolute(normalized, ec);
  if (ec) {
    absolute_path = fs::current_path() / normalized;
  }
  return absolute_path.lexically_normal();
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
  std::array<char, 4096> buffer {};
  ssize_t captured = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (captured <= 0) {
    return {};
  }
  buffer[static_cast<size_t>(captured)] = '\0';
  fs::path exec_path(buffer.data());
  return make_absolute_safely(exec_path).parent_path();
}

static bool has_jieba_resource(const fs::path &dir)
{
  if (dir.empty()) {
    return false;
  }

  const fs::path     normalized = dir.lexically_normal();
  const std::string  dir_str    = normalized.string();
  struct stat        dir_stat {};
  if (::stat(dir_str.c_str(), &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) {
    return false;
  }

  auto ensure_file_exists = [](const fs::path &candidate) -> bool {
    struct stat file_stat {};
    std::string file_path = candidate.lexically_normal().string();
    return ::stat(file_path.c_str(), &file_stat) == 0;
  };

  return ensure_file_exists(normalized / "jieba.dict.utf8") &&
         ensure_file_exists(normalized / "hmm_model.utf8") &&
         ensure_file_exists(normalized / "user.dict.utf8") &&
         ensure_file_exists(normalized / "idf.utf8") &&
         ensure_file_exists(normalized / "stop_words.utf8");
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
    if (has_jieba_resource(dir)) {
      return dir;
    }
  }

  return {};
}

class JiebaTokenizer
{
public:
  static RC tokenize(const string &text, const string &parser, vector<string> &tokens)
  {
    string parser_name = parser;
    if (!parser_name.empty()) {
      common::str_to_lower(parser_name);
    }
    if (!parser_name.empty() && parser_name != "jieba") {
      LOG_WARN("Unsupported full-text parser: %s", parser.c_str());
      return RC::UNIMPLEMENTED;
    }

    auto &ctx = context();
    std::call_once(ctx.init_once, [&ctx]() {
      ctx.init_rc = ctx.initialize();
    });
    if (ctx.init_rc != RC::SUCCESS) {
      return ctx.init_rc;
    }

    vector<string> raw;
    ctx.jieba->Cut(text, raw, true);
    refine_tokens_from_raw(raw, &ctx.stop_words, tokens);
    return RC::SUCCESS;
  }

private:
  struct Context
  {
    std::once_flag                      init_once;
    RC                                  init_rc = RC::SUCCESS;
    unique_ptr<cppjieba::Jieba>         jieba;
    unordered_set<string>               stop_words;
    fs::path                            dict_dir;

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

  static Context &context()
  {
    static Context ctx;
    return ctx;
  }
};

#else  // MINIOB_WITH_JIEBA

class JiebaTokenizer
{
public:
  static RC tokenize(const string &text, const string &parser, vector<string> &tokens)
  {
    (void)parser;
    vector<string> raw{ text };
    refine_tokens_from_raw(raw, nullptr, tokens);
    return RC::SUCCESS;
  }
};

#endif  // MINIOB_WITH_JIEBA

}  // namespace

RC TextTokenizer::tokenize(const string &text, const string &parser, vector<string> &tokens)
{
  return JiebaTokenizer::tokenize(text, parser, tokens);
}
