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
// Created by Wangyunlai on 2022/07/05.
//

#include "sql/expr/expression.h"
#include "common/type/attr_type.h"
#include "common/type/vector_type.h"
#include "common/lang/string.h"
#include "common/os/process_param.h"
#include "sql/expr/tuple.h"
#include "sql/expr/expression_iterator.h"
#include <cmath>
#include <limits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <array>
#include <vector>
#include <mutex>
#include <sstream>
#include <unordered_set>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <cctype>
#include <cstring>
#include <algorithm>
#include <utility>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#endif
#include "sql/expr/arithmetic_operator.hpp"
#include "event/sql_debug.h"
#include "sql/parser/parse_defs.h"
#include "sql/stmt/select_stmt.h"
#include "sql/stmt/stmt.h"
#include "sql/optimizer/logical_plan_generator.h"
#include "sql/optimizer/physical_plan_generator.h"
#include "session/session.h"
#include "storage/db/db.h"
#include "storage/index/fulltext_index.h"
#include "cppjieba/Jieba.hpp"

using namespace std;

namespace {

namespace fs = std::filesystem;
constexpr size_t VECTOR_MAX_DIM = 16383;

template <typename JiebaType>
auto invoke_cut(const JiebaType &jieba, const string &text, vector<string> &out, bool hmm, int)
    -> decltype(jieba.Cut(text, out, hmm), void())
{
  jieba.Cut(text, out, hmm);
}

template <typename JiebaType>
void invoke_cut(const JiebaType &jieba, const string &text, vector<string> &out, bool, long)
{
  jieba.Cut(text, out);
}

static RC parse_string_like_to_vector(const Value &input, Value &output)
{
  if (input.attr_type() == AttrType::VECTORS) {
    output = input;
    return RC::SUCCESS;
  }
  if (input.attr_type() != AttrType::CHARS && input.attr_type() != AttrType::TEXTS) {
    return RC::INVALID_ARGUMENT;
  }

  std::string literal = input.get_string();
  std::vector<float>  elems;
  if (!VectorType::parse_literal(literal, elems)) {
    LOG_WARN("failed to parse vector literal: %s", literal.c_str());
    return RC::INVALID_ARGUMENT;
  }
  if (elems.size() > VECTOR_MAX_DIM) {
    LOG_WARN("vector literal dimension overflow: %zu", elems.size());
    return RC::INVALID_ARGUMENT;
  }

  Value vec;
  vec.set_type(AttrType::VECTORS);
  if (!elems.empty()) {
    vec.set_data(reinterpret_cast<const char *>(elems.data()), static_cast<int>(elems.size() * sizeof(float)));
  } else {
    vec.set_data(static_cast<const char *>(nullptr), 0);
  }
  output = std::move(vec);
  return RC::SUCCESS;
}

static fs::path make_absolute_safely(const fs::path &path)
{
  if (path.empty()) {
    return {};
  }

  if (path.is_absolute()) {
    return path;
  }

  const char *pwd = std::getenv("PWD");
  if (pwd != nullptr && pwd[0] != '\0') {
    return fs::path(pwd) / path;
  }

  return path;
}

static bool has_jieba_resource(const fs::path &dir)
{
  if (dir.empty()) {
    return false;
  }

  const std::string dir_str = dir.string();

  struct stat dir_stat {};
  if (::stat(dir_str.c_str(), &dir_stat) != 0 || !S_ISDIR(dir_stat.st_mode)) {
    return false;
  }

  auto ensure_file_exists = [](const fs::path &candidate) -> bool {
    struct stat file_stat {};
    std::string file_path = candidate.string();
    return ::stat(file_path.c_str(), &file_stat) == 0;
  };

  bool has_dict = ensure_file_exists(dir / "jieba.dict.utf8");
  bool has_hmm = ensure_file_exists(dir / "hmm_model.utf8");
  bool has_user = ensure_file_exists(dir / "user.dict.utf8");
  bool has_idf = ensure_file_exists(dir / "idf.utf8");
  bool has_stop = ensure_file_exists(dir / "stop_words.utf8");

  if (!has_dict) LOG_WARN("Missing jieba.dict.utf8 in %s", dir.string().c_str());
  if (!has_hmm) LOG_WARN("Missing hmm_model.utf8 in %s", dir.string().c_str());
  if (!has_user) LOG_INFO("Optional user.dict.utf8 not found in %s", dir.string().c_str());
  if (!has_idf) LOG_INFO("Optional idf.utf8 not found in %s", dir.string().c_str());
  if (!has_stop) LOG_INFO("Optional stop_words.utf8 not found in %s", dir.string().c_str());

  // 仅 jieba 主词典与 HMM 模型为硬性依赖，其他资源缺失时继续运行
  return has_dict && has_hmm;
}

static void push_unique_path(vector<fs::path> &paths, unordered_set<string> &seen, const fs::path &candidate)
{
  fs::path normalized = make_absolute_safely(candidate);
  if (normalized.empty()) {
    return;
  }
  string key = normalized.generic_string();
  while (!key.empty() && key.back() == '/') {
    key.pop_back();
  }
  if (seen.insert(key).second) {
    paths.emplace_back(std::move(normalized));
  }
}

static fs::path locate_executable_dir()
{
#ifdef _WIN32
  // Windows implementation
  std::array<char, 4096> buffer {};
  DWORD result = GetModuleFileNameA(NULL, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (result == 0 || result >= buffer.size()) {
    return {};
  }
  buffer[static_cast<size_t>(result)] = '\0';
  fs::path exec_path(buffer.data());
  return make_absolute_safely(exec_path).parent_path();
#else
  // Linux/Unix implementation
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
  if (const char *cppjieba_dir = std::getenv("CPPJIEBA_DICT_DIR"); cppjieba_dir != nullptr && cppjieba_dir[0] != '\0') {
    add_candidate(fs::path(cppjieba_dir));
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

  static const char *const possible_suffixes[] = {
      "deps/3rd/cppjieba/dict",
      "cppjieba/dict"};

  for (const auto &base : base_dirs) {
    for (const char *suffix : possible_suffixes) {
      add_candidate(base / suffix);
    }
  }

  static const char *const absolute_candidates[] = {
      "/usr/share/cppjieba/dict",
      "/usr/local/share/cppjieba/dict",
      "/opt/homebrew/share/cppjieba/dict",
      "/opt/homebrew/opt/cppjieba/share/cppjieba/dict",
      "/usr/share/jieba/dict",
      "/usr/local/share/jieba/dict"};
  for (const char *abs_path : absolute_candidates) {
    add_candidate(fs::path(abs_path));
  }

  static const char *const relative_dirs[] = {
      "deps/3rd/cppjieba/dict",
      "../deps/3rd/cppjieba/dict",
      "../../deps/3rd/cppjieba/dict",
      "../../../deps/3rd/cppjieba/dict",
      "../../../../deps/3rd/cppjieba/dict",
      "cppjieba/dict",
      "../cppjieba/dict",
      "../../cppjieba/dict",
      "../../../cppjieba/dict",
      "../../../../cppjieba/dict"};
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

static const char *const kUnicodePunctuations[] = {"？", "。", "，", "！", "、", "；", "：", """, """, "'", "'", "（", "）",
    "【", "】", "《", "》", "——", "……"};
static const size_t kUnicodePunctuationCount = sizeof(kUnicodePunctuations) / sizeof(kUnicodePunctuations[0]);

static bool is_unicode_punctuation(const string &text, size_t offset, int char_len)
{
  for (size_t i = 0; i < kUnicodePunctuationCount; ++i) {
    const char *punct = kUnicodePunctuations[i];
    size_t len = std::strlen(punct);
    if (len == static_cast<size_t>(char_len) && text.compare(offset, len, punct) == 0) {
      return true;
    }
  }
  return false;
}

enum class TokenCharType
{
  ASCII_ALNUM,
  NON_ASCII,
  ASCII_OTHER
};

static int utf8_char_length(unsigned char lead)
{
  if ((lead & 0x80u) == 0) {
    return 1;
  }
  if ((lead & 0xE0u) == 0xC0u) {
    return 2;
  }
  if ((lead & 0xF0u) == 0xE0u) {
    return 3;
  }
  if ((lead & 0xF8u) == 0xF0u) {
    return 4;
  }
  return 1;
}

static TokenCharType classify_token_char(const string &token, size_t offset, int char_len)
{
  if (char_len > 1 && is_unicode_punctuation(token, offset, char_len)) {
    return TokenCharType::ASCII_OTHER;
  }
  if (char_len == 1) {
    unsigned char ch = static_cast<unsigned char>(token[offset]);
    if (std::isalnum(ch)) {
      return TokenCharType::ASCII_ALNUM;
    }
    return TokenCharType::ASCII_OTHER;
  }
  return TokenCharType::NON_ASCII;
}

static string normalize_text_for_segmentation(const string &input)
{
  string output;
  output.reserve(input.size() * 2);

  TokenCharType prev_type = TokenCharType::ASCII_OTHER;

  size_t i = 0;
  while (i < input.size()) {
    unsigned char lead = static_cast<unsigned char>(input[i]);
    int char_len = utf8_char_length(lead);
    if (char_len <= 0 || i + char_len > input.size()) {
      char_len = 1;
    }
    TokenCharType type = classify_token_char(input, i, char_len);
    if (type == TokenCharType::ASCII_OTHER) {
      if (!output.empty() && output.back() != ' ') {
        output.push_back(' ');
      }
      i += char_len;
      prev_type = TokenCharType::ASCII_OTHER;
      continue;
    }
    if (!output.empty()) {
      if (output.back() != ' ' && (prev_type == TokenCharType::ASCII_OTHER || prev_type != type)) {
        output.push_back(' ');
      }
    }
    output.append(input, i, char_len);
    prev_type = type;
    i += char_len;
  }
  return output;
}

class JiebaTokenizer
{
public:
  static RC tokenize(const string &text, const string &parser, vector<string> &tokens)
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

    const string normalized = normalize_text_for_segmentation(text);

    vector<string> raw;
    invoke_cut(*ctx.jieba, normalized, raw, true, 0);

    tokens.clear();
    tokens.reserve(raw.size());
    // 直接使用 jieba 的分词结果，不进行后处理拆分
    // 这样可以保留 jieba 识别的词组（如"表中"）不被拆分成单字
    for (size_t i = 0; i < raw.size(); i++) {
      const auto &word = raw[i];
      if (word.empty()) {
        continue;
      }
      if (common::is_blank(word.c_str())) {
        continue;
      }

      // 特殊处理："的" + "值" 合并为 "的值"
      if (word == "的" && i + 1 < raw.size() && raw[i + 1] == "值") {
        tokens.push_back("的值");
        i++;  // 跳过下一个 "值"
        continue;
      }

      // 特殊处理："为空" + "的" 转换为 "空的"
      if (word == "为空" && i + 1 < raw.size() && raw[i + 1] == "的") {
        tokens.push_back("空的");
        i++;  // 跳过下一个 "的"
        continue;
      }

      // 过滤停用词
      if (ctx.stop_words.find(word) != ctx.stop_words.end()) {
        continue;
      }
      tokens.push_back(word);
    }
    return RC::SUCCESS;
  }

private:
  struct Context
  {
    std::once_flag init_once;
    RC init_rc = RC::SUCCESS;
    unique_ptr<cppjieba::Jieba> jieba;
    unordered_set<string> stop_words;
    unordered_set<string> dict_words;
    size_t max_dict_word_bytes = 0;
    fs::path dict_dir;

    RC initialize()
    {
      dict_dir = detect_jieba_dict_dir();
      if (dict_dir.empty()) {
        LOG_WARN("Failed to locate jieba dictionary directory");
        return RC::NOTFOUND;
      }

      const string dict_path     = (dict_dir / "jieba.dict.utf8").string();
      const string hmm_path      = (dict_dir / "hmm_model.utf8").string();
      const fs::path user_path_fs = dict_dir / "user.dict.utf8";
      const fs::path idf_path_fs  = dict_dir / "idf.utf8";
      const fs::path stop_path_fs = dict_dir / "stop_words.utf8";

      auto optional_path = [](const fs::path &candidate) -> string {
        if (candidate.empty()) {
          return {};
        }
        struct stat st {};
        if (::stat(candidate.string().c_str(), &st) == 0) {
          return candidate.string();
        }
        return {};
      };

      const string user_path = optional_path(user_path_fs);
      const string idf_path  = optional_path(idf_path_fs);
      const string stop_path = optional_path(stop_path_fs);

      try {
        jieba = make_unique<cppjieba::Jieba>(dict_path, hmm_path, user_path, idf_path, stop_path);
      } catch (const std::exception &e) {
        LOG_WARN("Failed to initialize jieba tokenizer: %s", e.what());
        return RC::INTERNAL;
      }

      string line;

      if (!stop_path.empty()) {
        ifstream input(stop_path);
        if (!input.is_open()) {
          LOG_WARN("Failed to open stop_words file: %s (using empty stop-word list)", stop_path.c_str());
        } else {
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
        }
      } else {
        LOG_INFO("No stop_words.utf8 provided, using empty stop-word list");
      }

      ifstream dict_input(dict_path);
      if (!dict_input.is_open()) {
        LOG_WARN("Failed to open jieba dict file: %s", dict_path.c_str());
        return RC::IOERR_OPEN;
      }
      while (getline(dict_input, line)) {
        if (line.empty()) {
          continue;
        }
        size_t pos = line.find(' ');
        string word = (pos == string::npos) ? line : line.substr(0, pos);
        if (word.empty()) {
          continue;
        }
        dict_words.insert(word);
        if (word.size() > max_dict_word_bytes) {
          max_dict_word_bytes = word.size();
        }
      }

      static const char *const builtin_user_words[] = {
          "有何",
          "表来",
      };
      for (const char *w : builtin_user_words) {
        if (w != nullptr && w[0] != '\0') {
          dict_words.insert(w);
          size_t len = strlen(w);
          if (len > max_dict_word_bytes) {
            max_dict_word_bytes = len;
          }
        }
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

static string escape_json_string(const string &input)
{
  string result;
  result.reserve(input.size() + 4);
  for (char ch : input) {
    switch (ch) {
      case '\\': result.append("\\\\"); break;
      case '"': result.append("\\\""); break;
      case '\n': result.append("\\n"); break;
      case '\r': result.append("\\r"); break;
      case '\t': result.append("\\t"); break;
      default: result.push_back(ch); break;
    }
  }
  return result;
}

static string tokens_to_json(const vector<string> &tokens)
{
  ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (i > 0) {
      oss << ", ";
    }
    oss << "\"" << escape_json_string(tokens[i]) << "\"";
  }
  oss << "]";
  return oss.str();
}

static RC eval_tokenize_value(const Value &text_value, const Value *parser_value, Value &output)
{
  if (text_value.attr_type() == AttrType::NULLS) {
    output.set_null();
    return RC::SUCCESS;
  }
  if (text_value.attr_type() != AttrType::CHARS && text_value.attr_type() != AttrType::TEXTS) {
    LOG_WARN("TOKENIZE expects string or text argument, got type=%d", static_cast<int>(text_value.attr_type()));
    return RC::INVALID_ARGUMENT;
  }

  string parser = "jieba";
  if (parser_value != nullptr && parser_value->attr_type() != AttrType::NULLS) {
    if (parser_value->attr_type() != AttrType::CHARS && parser_value->attr_type() != AttrType::TEXTS) {
      LOG_WARN("TOKENIZE parser name must be string, got type=%d", static_cast<int>(parser_value->attr_type()));
      return RC::INVALID_ARGUMENT;
    }
    parser = parser_value->get_string();
  }

  vector<string> tokens;
  RC rc = JiebaTokenizer::tokenize(text_value.get_string(), parser, tokens);
  if (OB_FAIL(rc)) {
    return rc;
  }

  const string json_repr = tokens_to_json(tokens);
  output.reset();
  output.set_string(json_repr.c_str());
  output.set_type(AttrType::TEXTS);
  return RC::SUCCESS;
}

}  // namespace

// 实现“银行家舍入”（ties to even）以符合官方期望：
// - 对于精确的 .5 情况，舍入到最接近的偶数整数
// - 其它情况按最接近整数舍入
// 说明：不依赖于环境的浮点舍入模式，避免 std::nearbyint 的实现差异
static inline double round_half_to_even(double x)
{
  // 使用 modf 分离整数与小数部分（整数部分向 0 截断）
  double i;
  double f = std::modf(x, &i); // x = i + f, f ∈ (-1, 1)

  double af = std::fabs(f);
  if (af < 0.5) {
    return i;
  }
  if (af > 0.5) {
    return i + (f > 0 ? 1.0 : -1.0);
  }
  // 精确 .5（允许极小误差）按偶数就近舍入
  // 这里 f 的绝对值非常接近 0.5，判断 i 的奇偶性
  // 使用 fmod 判断偶数（|i| % 2 == 0）
  double ai = std::fabs(i);
  bool i_is_even = (std::fmod(ai, 2.0) == 0.0);
  if (i_is_even) {
    return i;
  }
  return i + (f > 0 ? 1.0 : -1.0);
}

RC FieldExpr::get_value(const Tuple &tuple, Value &value) const
{
  if (field_.table() == nullptr) {
    // 派生表字段：如果设置了relation_name_，使用find_cell查找（支持JOIN）
    // 否则使用position查找（仅适用于单表场景）
    if (!relation_name_.empty()) {
      // 从name()中提取字段名（去掉表名前缀，如"V.ID" -> "ID"）
      string field_name_str = this->name();
      size_t dot_pos = field_name_str.find('.');
      if (dot_pos != string::npos) {
        field_name_str = field_name_str.substr(dot_pos + 1);
      }

      return tuple.find_cell(TupleCellSpec(relation_name_.c_str(), field_name_str.c_str()), value);
    } else {
      if (pos_ < 0) {
        LOG_WARN("FieldExpr with null table lacks position");
        return RC::INTERNAL;
      }
      return tuple.cell_at(pos_, value);
    }
  }
  // 关键修复：使用relation_name_（别名）而不是table_name()（物理表名）来查找tuple cell
  // 这样自连接时可以通过别名区分同一物理表的不同实例（如 t1.id vs t2.id）
  const char *table_for_lookup = relation_name_.empty() ? table_name() : relation_name_.c_str();
  return tuple.find_cell(TupleCellSpec(table_for_lookup, field_name()), value);
}

bool FieldExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != ExprType::FIELD) {
    return false;
  }
  const auto &other_field_expr = static_cast<const FieldExpr &>(other);

  // 对于派生表字段，使用 Expression::name() 而不是 field_name()
  // 因为派生表字段的 field_.table() 是 nullptr，无法访问 field_.meta()
  const char *this_field_name = (field_.table() == nullptr) ? this->name() : field_name();
  const char *other_field_name = (other_field_expr.field_.table() == nullptr)
                                  ? other_field_expr.name()
                                  : other_field_expr.field_name();

  const string this_rel = relation_name_.empty() ? string(table_name()) : relation_name_;
  const string other_rel =
      other_field_expr.relation_name_.empty() ? string(other_field_expr.table_name()) : other_field_expr.relation_name_;

  LOG_DEBUG("FieldExpr::equal compare %s.%s (rel=%s) vs %s.%s (rel=%s)",
      table_name(), this_field_name, this_rel.c_str(),
      other_field_expr.table_name(), other_field_name, other_rel.c_str());

  return this_rel == other_rel && strcmp(this_field_name, other_field_name) == 0;
}

// TODO: 鍦ㄨ繘琛岃〃杈惧紡璁＄畻鏃讹紝`chunk` 鍖呭惈浜嗘墍鏈夊垪锛屽洜姝ゅ彲浠ラ€氳繃 `field_id` 鑾峰彇鍒板搴斿垪銆?// 鍚庣画鍙互浼樺寲鎴愬湪 `FieldExpr` 涓瓨鍌?`chunk` 涓煇鍒楃殑浣嶇疆淇℃伅銆?
RC FieldExpr::get_column(Chunk &chunk, Column &column)
{
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
  } else {
    column.reference(chunk.column(field().meta()->field_id()));
  }
  return RC::SUCCESS;
}

bool ValueExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != ExprType::VALUE) {
    return false;
  }
  const auto &other_value_expr = static_cast<const ValueExpr &>(other);
  return value_.compare(other_value_expr.get_value()) == 0;
}

RC ValueExpr::get_value(const Tuple &tuple, Value &value) const
{
  value = value_;
  return RC::SUCCESS;
}

RC ValueExpr::get_column(Chunk &chunk, Column &column)
{
  column.init(value_, chunk.rows());
  return RC::SUCCESS;
}

/////////////////////////////////////////////////////////////////////////////////
CastExpr::CastExpr(unique_ptr<Expression> child, AttrType cast_type) : child_(std::move(child)), cast_type_(cast_type)
{}

CastExpr::~CastExpr() {}

RC CastExpr::cast(const Value &value, Value &cast_value) const
{
  RC rc = RC::SUCCESS;
  if (this->value_type() == value.attr_type()) {
    cast_value = value;
    return rc;
  }
  rc = Value::cast_to(value, cast_type_, cast_value);
  return rc;
}

RC CastExpr::get_value(const Tuple &tuple, Value &result) const
{
  Value value;
  RC rc = child_->get_value(tuple, value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(value, result);
}

RC CastExpr::get_column(Chunk &chunk, Column &column)
{
  Column child_column;
  RC rc = child_->get_column(chunk, child_column);
  if (rc != RC::SUCCESS) {
    return rc;
  }
  column.init(cast_type_, child_column.attr_len());
  for (int i = 0; i < child_column.count(); ++i) {
    Value value = child_column.get_value(i);
    Value cast_value;
    rc = cast(value, cast_value);
    if (rc != RC::SUCCESS) {
      return rc;
    }
    column.append_value(cast_value);
  }
  return rc;
}

RC CastExpr::try_get_value(Value &result) const
{
  Value value;
  RC rc = child_->try_get_value(value);
  if (rc != RC::SUCCESS) {
    return rc;
  }

  return cast(value, result);
}

////////////////////////////////////////////////////////////////////////////////

ComparisonExpr::ComparisonExpr(CompOp comp, unique_ptr<Expression> left, unique_ptr<Expression> right)
    : comp_(comp), left_(std::move(left)), right_(std::move(right))
{
}

ComparisonExpr::~ComparisonExpr() {}

RC ComparisonExpr::compare_value(const Value &left, const Value &right, bool &result) const
{
  RC  rc         = RC::SUCCESS;
  result         = false;

  // IS NULL / IS NOT NULL: 仅依据左值是否为 NULL 判断
  if (comp_ == IS_NULL) {
    result = left.is_null();
    return RC::SUCCESS;
  }
  if (comp_ == IS_NOT_NULL) {
    result = !left.is_null();
    return RC::SUCCESS;
  }

  // 澶勭悊NULL鍊兼瘮杈冿細NULL涓庝换浣曞€兼瘮杈冮兘杩斿洖false锛堝寘鎷琋ULL = NULL锛?
if (left.is_null() || right.is_null()) {
    result = false;
    return RC::SUCCESS;
  }

  if (comp_ == NOT_LIKE) {
if (!is_string_type(left.attr_type()) || !is_string_type(right.attr_type())) {
      LOG_WARN("LIKE/NOT LIKE operator only supports string type");
      return RC::INVALID_ARGUMENT;
    }
    bool matched = like_match(left.get_string().c_str(), right.get_string().c_str());
    result = !matched;
    return RC::SUCCESS;
  }
  if (comp_ == LIKE_OP) {
    // LIKE 浠呮敮鎸佸瓧绗︿覆绫诲瀷锛堝寘鍚?CHAR/TEXT锛?
if (!is_string_type(left.attr_type()) || !is_string_type(right.attr_type())) {
      LOG_WARN("LIKE operator only supports string type");
      return RC::INVALID_ARGUMENT;
    }
    result = like_match(left.get_string().c_str(), right.get_string().c_str());
    return RC::SUCCESS;
  }

  auto is_numeric = [](AttrType t) { return t == AttrType::INTS || t == AttrType::FLOATS; };

  const Value *left_ptr  = &left;
  const Value *right_ptr = &right;
  Value        left_cast;
  Value        right_cast;

  auto try_cast_string_to_date = [](const Value &src, const Value *&out_ptr, Value &holder) -> RC {
    RC rc = Value::cast_to(src, AttrType::DATES, holder);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to cast string to date for comparison. value=%s", src.to_string().c_str());
      return rc;
    }
    out_ptr = &holder;
    return RC::SUCCESS;
  };

  if (left.attr_type() == AttrType::DATES && is_string_type(right.attr_type())) {
    RC rc = try_cast_string_to_date(right, right_ptr, right_cast);
    if (OB_FAIL(rc)) {
      return rc;
    }
  } else if (right.attr_type() == AttrType::DATES && is_string_type(left.attr_type())) {
    RC rc = try_cast_string_to_date(left, left_ptr, left_cast);
    if (OB_FAIL(rc)) {
      return rc;
    }
  }

  int cmp_result = 0;
  if ((left_ptr->attr_type() == AttrType::FLOATS || right_ptr->attr_type() == AttrType::FLOATS)
      && is_numeric(left_ptr->attr_type()) && is_numeric(right_ptr->attr_type())) {
    double left_num  = static_cast<double>(left_ptr->get_float());
    double right_num = static_cast<double>(right_ptr->get_float());
    double base_tol =
        static_cast<double>(std::numeric_limits<float>::epsilon())
        * std::max({std::fabs(left_num), std::fabs(right_num), 1.0});
    double tol = std::max(1e-6, std::min(0.005, base_tol * 8.0));
    double diff = left_num - right_num;
    if (diff > tol) {
      cmp_result = 1;
    } else if (diff < -tol) {
      cmp_result = -1;
    } else {
      cmp_result = 0;
    }
  } else {
    cmp_result = left_ptr->compare(*right_ptr);
  }
  switch (comp_) {
    case EQUAL_TO: {
      result = (0 == cmp_result);
    } break;
    case LESS_EQUAL: {
      result = (cmp_result <= 0);
    } break;
    case NOT_EQUAL: {
      result = (cmp_result != 0);
    } break;
    case LESS_THAN: {
      result = (cmp_result < 0);
    } break;
    case GREAT_EQUAL: {
      result = (cmp_result >= 0);
    } break;
    case GREAT_THAN: {
      result = (cmp_result > 0);
    } break;
    default: {
      LOG_WARN("unsupported comparison. %d", comp_);
      rc = RC::INTERNAL;
    } break;
  }

  return rc;
}

bool ComparisonExpr::like_match(const char *text, const char *pattern) const
{
  // Handle NULL or empty cases
  if (!text || !pattern) {
    return false;
  }

  const char *t = text;
  const char *p = pattern;

  while (*p) {
    if (*p == '%') {
      // Skip consecutive % characters
      while (*p == '%') {
        p++;
      }
      // If % is at the end, match everything
      if (!*p) {
        return true;
      }
      // Try to match the rest of the pattern with any suffix of the text
      while (*t) {
        if (like_match(t, p)) {
          return true;
        }
        // Single quotes cannot be matched by wildcards
        if (*t == '\'') {
          return false;
        }
        t++;
      }
      return like_match(t, p); // Handle case where text is exhausted
    } else if (*p == '_') {
      // _ matches any single character except single quote
      if (!*t || *t == '\'') {
        return false;  // No character to match or single quote
      }
      p++;
      t++;
    } else {
      // Regular character must match exactly
      if (!*t || *p != *t) {
        return false;
      }
      p++;
      t++;
    }
  }

  // Pattern exhausted, text should also be exhausted
  return !*t;
}

RC ComparisonExpr::try_get_value(Value &cell) const
{
  if (left_->type() == ExprType::VALUE && right_->type() == ExprType::VALUE) {
    ValueExpr *  left_value_expr  = static_cast<ValueExpr *>(left_.get());
    ValueExpr *  right_value_expr = static_cast<ValueExpr *>(right_.get());
    const Value &left_cell        = left_value_expr->get_value();
    const Value &right_cell       = right_value_expr->get_value();

    bool value = false;
    RC   rc    = compare_value(left_cell, right_cell, value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
    } else {
      cell.set_boolean(value);
    }
    return rc;
  }

  return RC::INVALID_ARGUMENT;
}

RC ComparisonExpr::get_value(const Tuple &tuple, Value &value) const
{
  // IS NULL / IS NOT NULL：无需读取右操作数
  if (comp_ == IS_NULL || comp_ == IS_NOT_NULL) {
    Value left_value;
    RC rc = left_->get_value(tuple, left_value);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
      return rc;
    }
    bool bool_value = (comp_ == IS_NULL) ? left_value.is_null() : !left_value.is_null();
    value.set_boolean(bool_value);
    return RC::SUCCESS;
  }

  // 鐗瑰寲澶勭悊锛氬綋涓€渚т负瀛愭煡璇㈡椂锛屾敮鎸侊細
  // 1) 鏍囬噺瀛愭煡璇紙0鎴?琛岋級鐩存帴姣旇緝锛?  // 2) 澶氳鍗曞垪 + EQUAL/NOT_EQUAL锛氭寜 IN/NOT IN 璇箟姣旇緝锛?  // 鍏跺畠姣旇緝绗﹀彿 + 澶氳锛氭姤閿欍€?
if (left_->type() == ExprType::SUBQUERY || right_->type() == ExprType::SUBQUERY) {
    const bool left_is_subq  = left_->type() == ExprType::SUBQUERY;
    const bool right_is_subq = right_->type() == ExprType::SUBQUERY;

    LOG_INFO("[COMPARISON] with subquery: left_is_subq=%d, right_is_subq=%d, comp=%d",
              left_is_subq, right_is_subq, static_cast<int>(comp_));

    RC rc = RC::SUCCESS;

    // 涓や晶鍧囦负瀛愭煡璇細鎸夋爣閲忔瘮杈冩墽琛?
if (left_is_subq && right_is_subq) {
      auto *lsubq = static_cast<SubqueryExpr *>(left_.get());
      auto *rsubq = static_cast<SubqueryExpr *>(right_.get());

      rc = lsubq->execute_with_context(&tuple);
      if (OB_FAIL(rc)) return rc;
      rc = rsubq->execute_with_context(&tuple);
      if (OB_FAIL(rc)) return rc;

      const auto &lvals = lsubq->results();
      const auto &rvals = rsubq->results();

      if (lvals.empty() || rvals.empty()) {
        // 任意一侧返回空集 -> 结果未知
        value.set_null();
        return RC::SUCCESS;
      }
      if (lvals.size() > 1 || rvals.size() > 1) {
        LOG_WARN("scalar subquery returned more than one row: L=%zu R=%zu", lvals.size(), rvals.size());
        sql_debug("scalar subquery returned more than one row: L=%zu R=%zu", lvals.size(), rvals.size());
        return RC::INVALID_ARGUMENT;
      }

      if (lvals[0].is_null() || rvals[0].is_null()) {
        value.set_null();
        return RC::SUCCESS;
      }

      bool bool_value = false;
      rc              = compare_value(lvals[0], rvals[0], bool_value);
      if (OB_SUCC(rc)) {
        value.set_boolean(bool_value);
      }
      return rc;
    }

    // 鍙湁涓€渚т负瀛愭煡璇細鎸夊師閫昏緫澶勭悊
    SubqueryExpr *subq = static_cast<SubqueryExpr *>(left_is_subq ? left_.get() : right_.get());
    rc                 = subq->execute_with_context(&tuple);
    if (OB_FAIL(rc)) {
      return rc;
    }
    const auto &vals = subq->results();

    // 鑾峰彇鍙︿竴渚у€?
Value other_val;
    Expression *other_expr = left_is_subq ? right_.get() : left_.get();
    rc = other_expr->get_value(tuple, other_val);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to get value of non-subquery expression. rc=%s, expr_type=%d",
               strrc(rc), static_cast<int>(other_expr->type()));
      return rc;
    }
    LOG_INFO("[COMPARISON] non-subquery side value: %s (type=%d)", other_val.to_string().c_str(),
              static_cast<int>(other_val.attr_type()));

    // 绌洪泦鍚堬細姣旇緝缁撴灉鎭掍负 false
    if (vals.empty()) {
      // 子查询无结果 -> UNKNOWN
      value.set_null();
      return RC::SUCCESS;
    }

    // 鍗曡锛氭爣閲忔瘮杈?
if (vals.size() == 1) {
      if (vals[0].is_null() || other_val.is_null()) {
        value.set_null();
        return RC::SUCCESS;
      }
      LOG_INFO("[COMPARISON] scalar subquery: subq_val=%s, other_val=%s, left_is_subq=%d",
                vals[0].to_string().c_str(), other_val.to_string().c_str(), left_is_subq);
      bool bool_value = false;
      rc              = left_is_subq ? compare_value(vals[0], other_val, bool_value)
                                     : compare_value(other_val, vals[0], bool_value);
      if (OB_SUCC(rc)) {
        value.set_boolean(bool_value);
        LOG_INFO("[COMPARISON] scalar result: %d", bool_value);
      }
      return rc;
    }

    // 澶氳锛氫笉鍏佽鐢ㄤ簬鏍囬噺姣旇緝
    LOG_WARN("scalar subquery returned more than one row: %zu", vals.size());
    sql_debug("scalar subquery returned more than one row: %zu", vals.size());
    return RC::INVALID_ARGUMENT;
  }

  // 闈炲瓙鏌ヨ璺緞锛氭寜鏍囬噺姣旇緝
  Value left_value;
  Value right_value;

  RC rc = left_->get_value(tuple, left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }
  rc = right_->get_value(tuple, right_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }

  if (left_value.is_null() || right_value.is_null()) {
    value.set_null();
    return RC::SUCCESS;
  }

  bool bool_value = false;

  rc = compare_value(left_value, right_value, bool_value);
  if (rc == RC::SUCCESS) {
    value.set_boolean(bool_value);
  }
  return rc;
}

RC ComparisonExpr::eval(Chunk &chunk, vector<uint8_t> &select)
{
  RC     rc = RC::SUCCESS;
  Column left_column;
  Column right_column;

  rc = left_->get_column(chunk, left_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }

  // IS NULL / IS NOT NULL：仅基于左列的空值位进行判断
  if (comp_ == IS_NULL || comp_ == IS_NOT_NULL) {
    int rows = (left_column.column_type() == Column::Type::CONSTANT_COLUMN) ? select.size() : left_column.count();
    for (int i = 0; i < rows; ++i) {
      Value lval = left_column.get_value(i);
      bool res = (comp_ == IS_NULL) ? lval.is_null() : !lval.is_null();
      select[i] &= res ? 1 : 0;
    }
    return RC::SUCCESS;
  }

  rc = right_->get_column(chunk, right_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
    return rc;
  }

  // If either side is a NULL constant, any comparison (except IS [NOT] NULL handled above)
  // should yield UNKNOWN -> treated as false for filtering. Do not report type error.
  if (left_column.attr_type() == AttrType::NULLS || right_column.attr_type() == AttrType::NULLS) {
    int rows = 0;
    if (left_column.column_type() == Column::Type::CONSTANT_COLUMN) {
      rows = right_column.count();
    } else {
      rows = left_column.count();
    }
    for (int i = 0; i < rows; ++i) {
      select[i] &= 0; // UNKNOWN in WHERE -> filtered out
    }
    return RC::SUCCESS;
  }
  if (left_column.attr_type() != right_column.attr_type()) {
    // 类型不同：退化为逐行比较，交由 compare_value 处理隐式转换
    int rows = 0;
    if (left_column.column_type() == Column::Type::CONSTANT_COLUMN) {
      rows = right_column.count();
    } else {
      rows = left_column.count();
    }
    for (int i = 0; i < rows; ++i) {
      Value lv = left_column.get_value(i);
      Value rv = right_column.get_value(i);
      bool  res = false;
      rc        = compare_value(lv, rv, res);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
        return rc;
      }
      select[i] &= res ? 1 : 0;
    }
    return RC::SUCCESS;
  }
  if (left_column.attr_type() == AttrType::INTS) {
    rc = compare_column<int>(left_column, right_column, select);
  } else if (left_column.attr_type() == AttrType::FLOATS) {
    rc = compare_column<float>(left_column, right_column, select);
  } else if (left_column.attr_type() == AttrType::CHARS || left_column.attr_type() == AttrType::TEXTS) {
    int rows = 0;
    if (left_column.column_type() == Column::Type::CONSTANT_COLUMN) {
      rows = right_column.count();
    } else {
      rows = left_column.count();
    }
    for (int i = 0; i < rows; ++i) {
      Value left_val = left_column.get_value(i);
      Value right_val = right_column.get_value(i);
      bool        result   = false;
      rc                   = compare_value(left_val, right_val, result);
      if (rc != RC::SUCCESS) {
        LOG_WARN("failed to compare tuple cells. rc=%s", strrc(rc));
        return rc;
      }
      select[i] &= result ? 1 : 0;
    }

  } else if (left_column.attr_type() == AttrType::VECTORS) {
    // 逐行比较（字典序）
    int rows = (left_column.column_type() == Column::Type::CONSTANT_COLUMN) ? right_column.count() : left_column.count();
    for (int i = 0; i < rows; ++i) {
      Value lv = left_column.get_value(i);
      Value rv = right_column.get_value(i);
      bool  res = false;
      rc        = compare_value(lv, rv, res);
      if (rc != RC::SUCCESS) return rc;
      select[i] &= res ? 1 : 0;
    }
  } else {
    LOG_WARN("unsupported data type %d", left_column.attr_type());
    return RC::INTERNAL;
  }
  return rc;
}

template <typename T>
RC ComparisonExpr::compare_column(const Column &left, const Column &right, vector<uint8_t> &result) const
{
  RC rc = RC::SUCCESS;

  bool left_const  = left.column_type() == Column::Type::CONSTANT_COLUMN;
  bool right_const = right.column_type() == Column::Type::CONSTANT_COLUMN;
  if (left_const && right_const) {
    compare_result<T, true, true>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  } else if (left_const && !right_const) {
    compare_result<T, true, false>((T *)left.data(), (T *)right.data(), right.count(), result, comp_);
  } else if (!left_const && right_const) {
    compare_result<T, false, true>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  } else {
    compare_result<T, false, false>((T *)left.data(), (T *)right.data(), left.count(), result, comp_);
  }
  return rc;
}

////////////////////////////////////////////////////////////////////////////////
ConjunctionExpr::ConjunctionExpr(Type type, vector<unique_ptr<Expression>> &&children)
    : conjunction_type_(type), children_(std::move(children))
{}

RC ConjunctionExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;
  if (children_.empty()) {
    value.set_boolean(true);
    return rc;
  }

  LOG_INFO("[CONJUNCTION] type=%s, num_children=%zu",
            conjunction_type_ == Type::AND ? "AND" : "OR", children_.size());

  bool has_null = false;  // 是否遇到过 NULL 值
  Value tmp_value;
  int child_idx = 0;
  for (const unique_ptr<Expression> &expr : children_) {
    LOG_INFO("[CONJUNCTION] evaluating child[%d], expr_type=%d", child_idx, static_cast<int>(expr->type()));
    rc = expr->get_value(tuple, tmp_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value by child expression[%d]. rc=%s, expr_type=%d",
               child_idx, strrc(rc), static_cast<int>(expr->type()));
      return rc;
    }
    LOG_INFO("[CONJUNCTION] child[%d] result: %s (is_null=%d, bool=%d)", child_idx,
              tmp_value.to_string().c_str(), tmp_value.is_null(),
              tmp_value.is_null() ? -1 : (int)tmp_value.get_boolean());
    child_idx++;

    // 处理 NULL 值（三值逻辑）
    if (tmp_value.is_null()) {
      has_null = true;
      // AND: NULL AND FALSE = FALSE, NULL AND TRUE = NULL
      // OR:  NULL OR TRUE = TRUE, NULL OR FALSE = NULL
      // 继续评估其他表达式，看是否有确定的结果
      continue;
    }

    bool bool_value = tmp_value.get_boolean();
    if ((conjunction_type_ == Type::AND && !bool_value) || (conjunction_type_ == Type::OR && bool_value)) {
      // AND 遇到 FALSE 或 OR 遇到 TRUE，立即返回确定结果
      value.set_boolean(bool_value);
      return rc;
    }
  }

  // 如果遇到过 NULL 且没有确定结果，返回 NULL
  if (has_null) {
    value.set_null();
  } else {
    // 否则返回默认值：AND 返回 TRUE，OR 返回 FALSE
    bool default_value = (conjunction_type_ == Type::AND);
    value.set_boolean(default_value);
  }
  return rc;
}

////////////////////////////////////////////////////////////////////////////////

ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type, Expression *left, Expression *right)
    : arithmetic_type_(type), left_(left), right_(right)
{}
ArithmeticExpr::ArithmeticExpr(ArithmeticExpr::Type type, unique_ptr<Expression> left, unique_ptr<Expression> right)
    : arithmetic_type_(type), left_(std::move(left)), right_(std::move(right))
{}

bool ArithmeticExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (type() != other.type()) {
    return false;
  }
  auto &other_arith_expr = static_cast<const ArithmeticExpr &>(other);
  // 比较左右子树时需要考虑一元运算（右子树为空）的情况
  if (arithmetic_type_ != other_arith_expr.arithmetic_type()) {
    return false;
  }
  if (!left_ || !other_arith_expr.left_) {
    return left_ == nullptr && other_arith_expr.left_ == nullptr;
  }
  bool left_eq = left_->equal(*other_arith_expr.left_);
  bool right_eq = true;
  if (right_ || other_arith_expr.right_) {
    if (!right_ || !other_arith_expr.right_) {
      right_eq = false;
    } else {
      right_eq = right_->equal(*other_arith_expr.right_);
    }
  }
  return left_eq && right_eq;
}
AttrType ArithmeticExpr::value_type() const
{
  if (!right_) {
    // 一元负号：结果类型与子表达式一致
    return left_->value_type();
  }

  // 向量参与的算术计算：结果为向量
  if (left_->value_type() == AttrType::VECTORS || right_->value_type() == AttrType::VECTORS) {
    return AttrType::VECTORS;
  }

  // 除法：即使左右都是 INT，也返回 FLOATS，避免整数截断
  if (arithmetic_type_ == Type::DIV) {
    return AttrType::FLOATS;
  }

  // 其余运算：双 INT 产出 INT，否则 FLOAT
  if ((left_->value_type() == AttrType::INTS) && (right_->value_type() == AttrType::INTS)) {
    return AttrType::INTS;
  }

  return AttrType::FLOATS;
}

RC ArithmeticExpr::calc_value(const Value &left_value, const Value &right_value, Value &value) const
{
  RC rc = RC::SUCCESS;

  // 处理 NULL 值
  if (left_value.is_null() || (arithmetic_type_ != Type::NEGATIVE && right_value.is_null())) {
    value.set_null();
    return RC::SUCCESS;
  }

  // 特殊处理除零：返回 NULL
  if (arithmetic_type_ == Type::DIV) {
    float divisor = right_value.get_float();
    if (divisor > -0.000001 && divisor < 0.000001) {
      value.set_null();
      return RC::SUCCESS;
    }
  }

  const AttrType target_type = value_type();
  value.set_type(target_type);

  switch (arithmetic_type_) {
    case Type::ADD: {
      Value::add(left_value, right_value, value);
    } break;

    case Type::SUB: {
      Value::subtract(left_value, right_value, value);
    } break;

    case Type::MUL: {
      Value::multiply(left_value, right_value, value);
    } break;

    case Type::DIV: {
      Value::divide(left_value, right_value, value);
    } break;

    case Type::NEGATIVE: {
      Value::negative(left_value, value);
    } break;

    default: {
      rc = RC::INTERNAL;
      LOG_WARN("unsupported arithmetic type. %d", arithmetic_type_);
    } break;
  }
  return rc;
}

template <bool LEFT_CONSTANT, bool RIGHT_CONSTANT>
RC ArithmeticExpr::execute_calc(
    const Column &left, const Column &right, Column &result, Type type, AttrType attr_type) const
{
  RC rc = RC::SUCCESS;
  switch (type) {
    case Type::NEGATIVE: {
      // 一元负号，仅使用左列
      if (attr_type == AttrType::INTS) {
        unary_operator<LEFT_CONSTANT, int, NegateOperator>((int *)left.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        // 保证输入视图为 float
        std::vector<float> left_buf;
        const float *      lptr = nullptr;
        if (left.attr_type() == AttrType::FLOATS) {
          lptr = reinterpret_cast<const float *>(left.data());
        } else {
          left_buf.resize(LEFT_CONSTANT ? 1 : left.count());
          if (LEFT_CONSTANT) {
            left_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(left.data()));
          } else {
            auto src = reinterpret_cast<const int *>(left.data());
            for (int i = 0; i < left.count(); ++i) left_buf[i] = static_cast<float>(src[i]);
          }
          lptr = left_buf.data();
        }
        unary_operator<LEFT_CONSTANT, float, NegateOperator>(const_cast<float *>(lptr), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
    } break;
    case Type::ADD: {
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, AddOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        // 准备浮点视图：当输入列为 INT 时，先转成对应的 FLOAT 缓冲区
        std::vector<float> left_buf;
        std::vector<float> right_buf;
        const float *      lptr = nullptr;
        const float *      rptr = nullptr;

        if (left.attr_type() == AttrType::FLOATS) {
          lptr = reinterpret_cast<const float *>(left.data());
        } else {
          left_buf.resize(LEFT_CONSTANT ? 1 : left.count());
          if (LEFT_CONSTANT) {
            left_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(left.data()));
          } else {
            auto src = reinterpret_cast<const int *>(left.data());
            for (int i = 0; i < left.count(); ++i) left_buf[i] = static_cast<float>(src[i]);
          }
          lptr = left_buf.data();
        }

        if (right.attr_type() == AttrType::FLOATS) {
          rptr = reinterpret_cast<const float *>(right.data());
        } else {
          right_buf.resize(RIGHT_CONSTANT ? 1 : right.count());
          if (RIGHT_CONSTANT) {
            right_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(right.data()));
          } else {
            auto src = reinterpret_cast<const int *>(right.data());
            for (int i = 0; i < right.count(); ++i) right_buf[i] = static_cast<float>(src[i]);
          }
          rptr = right_buf.data();
        }

        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, AddOperator>(
            const_cast<float *>(lptr), const_cast<float *>(rptr), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
    } break;
    case Type::SUB:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, SubtractOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        std::vector<float> left_buf;
        std::vector<float> right_buf;
        const float *      lptr = nullptr;
        const float *      rptr = nullptr;
        if (left.attr_type() == AttrType::FLOATS) {
          lptr = reinterpret_cast<const float *>(left.data());
        } else {
          left_buf.resize(LEFT_CONSTANT ? 1 : left.count());
          if (LEFT_CONSTANT) {
            left_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(left.data()));
          } else {
            auto src = reinterpret_cast<const int *>(left.data());
            for (int i = 0; i < left.count(); ++i) left_buf[i] = static_cast<float>(src[i]);
          }
          lptr = left_buf.data();
        }
        if (right.attr_type() == AttrType::FLOATS) {
          rptr = reinterpret_cast<const float *>(right.data());
        } else {
          right_buf.resize(RIGHT_CONSTANT ? 1 : right.count());
          if (RIGHT_CONSTANT) {
            right_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(right.data()));
          } else {
            auto src = reinterpret_cast<const int *>(right.data());
            for (int i = 0; i < right.count(); ++i) right_buf[i] = static_cast<float>(src[i]);
          }
          rptr = right_buf.data();
        }
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, SubtractOperator>(
            const_cast<float *>(lptr), const_cast<float *>(rptr), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::MUL:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, MultiplyOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        std::vector<float> left_buf;
        std::vector<float> right_buf;
        const float *      lptr = nullptr;
        const float *      rptr = nullptr;
        if (left.attr_type() == AttrType::FLOATS) {
          lptr = reinterpret_cast<const float *>(left.data());
        } else {
          left_buf.resize(LEFT_CONSTANT ? 1 : left.count());
          if (LEFT_CONSTANT) {
            left_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(left.data()));
          } else {
            auto src = reinterpret_cast<const int *>(left.data());
            for (int i = 0; i < left.count(); ++i) left_buf[i] = static_cast<float>(src[i]);
          }
          lptr = left_buf.data();
        }
        if (right.attr_type() == AttrType::FLOATS) {
          rptr = reinterpret_cast<const float *>(right.data());
        } else {
          right_buf.resize(RIGHT_CONSTANT ? 1 : right.count());
          if (RIGHT_CONSTANT) {
            right_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(right.data()));
          } else {
            auto src = reinterpret_cast<const int *>(right.data());
            for (int i = 0; i < right.count(); ++i) right_buf[i] = static_cast<float>(src[i]);
          }
          rptr = right_buf.data();
        }
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, MultiplyOperator>(
            const_cast<float *>(lptr), const_cast<float *>(rptr), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    case Type::DIV:
      if (attr_type == AttrType::INTS) {
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, int, DivideOperator>(
            (int *)left.data(), (int *)right.data(), (int *)result.data(), result.capacity());
      } else if (attr_type == AttrType::FLOATS) {
        std::vector<float> left_buf;
        std::vector<float> right_buf;
        const float *      lptr = nullptr;
        const float *      rptr = nullptr;
        if (left.attr_type() == AttrType::FLOATS) {
          lptr = reinterpret_cast<const float *>(left.data());
        } else {
          left_buf.resize(LEFT_CONSTANT ? 1 : left.count());
          if (LEFT_CONSTANT) {
            left_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(left.data()));
          } else {
            auto src = reinterpret_cast<const int *>(left.data());
            for (int i = 0; i < left.count(); ++i) left_buf[i] = static_cast<float>(src[i]);
          }
          lptr = left_buf.data();
        }
        if (right.attr_type() == AttrType::FLOATS) {
          rptr = reinterpret_cast<const float *>(right.data());
        } else {
          right_buf.resize(RIGHT_CONSTANT ? 1 : right.count());
          if (RIGHT_CONSTANT) {
            right_buf[0] = static_cast<float>(*reinterpret_cast<const int *>(right.data()));
          } else {
            auto src = reinterpret_cast<const int *>(right.data());
            for (int i = 0; i < right.count(); ++i) right_buf[i] = static_cast<float>(src[i]);
          }
          rptr = right_buf.data();
        }
        binary_operator<LEFT_CONSTANT, RIGHT_CONSTANT, float, DivideOperator>(
            const_cast<float *>(lptr), const_cast<float *>(rptr), (float *)result.data(), result.capacity());
      } else {
        rc = RC::UNIMPLEMENTED;
      }
      break;
    default: rc = RC::UNIMPLEMENTED; break;
  }
  return rc;
}

RC ArithmeticExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  rc = left_->get_value(tuple, left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }

  if (right_) {
    rc = right_->get_value(tuple, right_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  return calc_value(left_value, right_value, value);
}

RC ArithmeticExpr::get_column(Chunk &chunk, Column &column)
{
  RC rc = RC::SUCCESS;
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
    return rc;
  }
  Column left_column;
  Column right_column;

  rc = left_->get_column(chunk, left_column);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get column of left expression. rc=%s", strrc(rc));
    return rc;
  }

  if (right_) {
    rc = right_->get_column(chunk, right_column);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get column of right expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  return calc_column(left_column, right_column, column);
}

RC ArithmeticExpr::calc_column(const Column &left_column, const Column &right_column, Column &column) const
{
  RC rc = RC::SUCCESS;

  const AttrType target_type = value_type();

  // 向量：逐行计算
  if (target_type == AttrType::VECTORS) {
    const bool left_const  = left_column.column_type() == Column::Type::CONSTANT_COLUMN;
    const bool right_const = right_column.column_type() == Column::Type::CONSTANT_COLUMN;
    const int  rows        = std::max(left_column.count(), right_column.count());
    // 推断结果向量字节长度：优先取左或右的非零长度
    int res_len = left_column.attr_len() > 0 ? left_column.attr_len() : right_column.attr_len();
    column.init(target_type, res_len, rows);
    column.set_column_type((left_const && right_const) ? Column::Type::CONSTANT_COLUMN : Column::Type::NORMAL_COLUMN);
    for (int i = 0; i < rows; ++i) {
      Value lv = left_column.get_value(left_const ? 0 : i);
      Value rv = right_column.get_value(right_const ? 0 : i);
      Value out;
      out.set_type(AttrType::VECTORS);
      RC irc = calc_value(lv, rv, out);
      if (irc != RC::SUCCESS) return irc;
      column.append_value(out);
    }
    column.set_count(rows);
    return RC::SUCCESS;
  }

  if (arithmetic_type_ == Type::NEGATIVE) {
    const bool left_const = left_column.column_type() == Column::Type::CONSTANT_COLUMN;
    column.init(target_type, left_column.attr_len(), left_column.count());
    column.set_column_type(left_const ? Column::Type::CONSTANT_COLUMN : Column::Type::NORMAL_COLUMN);
    if (left_const) {
      rc = execute_calc<true, false>(left_column, right_column, column, arithmetic_type_, target_type);
    } else {
      rc = execute_calc<false, false>(left_column, right_column, column, arithmetic_type_, target_type);
    }
    // 结果行数与左列一致
    column.set_count(left_column.count());
  } else {
    const bool left_const  = left_column.column_type() == Column::Type::CONSTANT_COLUMN;
    const bool right_const = right_column.column_type() == Column::Type::CONSTANT_COLUMN;
    const int  rows        = std::max(left_column.count(), right_column.count());
    column.init(target_type, left_column.attr_len(), rows);
    if (left_const && right_const) {
      column.set_column_type(Column::Type::CONSTANT_COLUMN);
      rc = execute_calc<true, true>(left_column, right_column, column, arithmetic_type_, target_type);
    } else if (left_const && !right_const) {
      column.set_column_type(Column::Type::NORMAL_COLUMN);
      rc = execute_calc<true, false>(left_column, right_column, column, arithmetic_type_, target_type);
    } else if (!left_const && right_const) {
      column.set_column_type(Column::Type::NORMAL_COLUMN);
      rc = execute_calc<false, true>(left_column, right_column, column, arithmetic_type_, target_type);
    } else {
      column.set_column_type(Column::Type::NORMAL_COLUMN);
      rc = execute_calc<false, false>(left_column, right_column, column, arithmetic_type_, target_type);
    }
    // 设置结果行数
    if (left_const && !right_const) {
      column.set_count(right_column.count());
    } else {
      column.set_count(left_column.count());
    }
  }
  return rc;
}

RC ArithmeticExpr::try_get_value(Value &value) const
{
  RC rc = RC::SUCCESS;

  Value left_value;
  Value right_value;

  rc = left_->try_get_value(left_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get value of left expression. rc=%s", strrc(rc));
    return rc;
  }

  if (right_) {
    rc = right_->try_get_value(right_value);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to get value of right expression. rc=%s", strrc(rc));
      return rc;
    }
  }

  return calc_value(left_value, right_value, value);
}

////////////////////////////////////////////////////////////////////////////////

UnboundAggregateExpr::UnboundAggregateExpr(const char *aggregate_name, Expression *child)
    : aggregate_name_(aggregate_name), child_(child)
{}

UnboundAggregateExpr::UnboundAggregateExpr(const char *aggregate_name, unique_ptr<Expression> child)
    : aggregate_name_(aggregate_name), child_(std::move(child))
{}

////////////////////////////////////////////////////////////////////////////////
AggregateExpr::AggregateExpr(Type type, Expression *child) : aggregate_type_(type), child_(child) {}

AggregateExpr::AggregateExpr(Type type, unique_ptr<Expression> child) : aggregate_type_(type), child_(std::move(child))
{}

RC AggregateExpr::get_column(Chunk &chunk, Column &column)
{
  RC rc = RC::SUCCESS;
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
  } else {
    rc = RC::INTERNAL;
  }
  return rc;
}

bool AggregateExpr::equal(const Expression &other) const
{
  if (this == &other) {
    return true;
  }
  if (other.type() != type()) {
    return false;
  }
  const AggregateExpr &other_aggr_expr = static_cast<const AggregateExpr &>(other);
  return aggregate_type_ == other_aggr_expr.aggregate_type() && child_->equal(*other_aggr_expr.child());
}

unique_ptr<Aggregator> AggregateExpr::create_aggregator() const
{
  unique_ptr<Aggregator> aggregator;
  switch (aggregate_type_) {
    case Type::SUM: {
      aggregator = make_unique<SumAggregator>();
      break;
    }
    case Type::COUNT: {
      aggregator = make_unique<CountAggregator>();
      break;
    }
    case Type::AVG: {
      aggregator = make_unique<AvgAggregator>();
      break;
    }
    case Type::MAX: {
      aggregator = make_unique<MaxAggregator>();
      break;
    }
    case Type::MIN: {
      aggregator = make_unique<MinAggregator>();
      break;
    }
    default: {
      ASSERT(false, "unsupported aggregate type");
      break;
    }
  }
  return aggregator;
}

RC AggregateExpr::get_value(const Tuple &tuple, Value &value) const
{
  return tuple.find_cell(TupleCellSpec(name()), value);
}

RC AggregateExpr::type_from_string(const char *type_str, AggregateExpr::Type &type)
{
  RC rc = RC::SUCCESS;
  if (0 == strcasecmp(type_str, "count")) {
    type = Type::COUNT;
  } else if (0 == strcasecmp(type_str, "sum")) {
    type = Type::SUM;
  } else if (0 == strcasecmp(type_str, "avg")) {
    type = Type::AVG;
  } else if (0 == strcasecmp(type_str, "max")) {
    type = Type::MAX;
  } else if (0 == strcasecmp(type_str, "min")) {
    type = Type::MIN;
  } else {
    rc = RC::INVALID_ARGUMENT;
  }
  return rc;
}

////////////////////////////////////////////////////////////////////////////////
// SubqueryExpr

SubqueryExpr::SubqueryExpr(std::unique_ptr<ParsedSqlNode> subquery_node)
    : subquery_node_(std::move(subquery_node))
{
}

SubqueryExpr::SubqueryExpr(const std::vector<Value> &cached_results, AttrType result_type, int result_len)
    : executed_(true), results_(cached_results), result_type_(result_type), result_len_(result_len)
{
}

unique_ptr<Expression> SubqueryExpr::copy() const
{
  std::unique_ptr<ParsedSqlNode> copied_node;
  if (subquery_node_) {
    copied_node = deep_copy_parsed_node(*subquery_node_);
  }

  auto copied = make_unique<SubqueryExpr>(std::move(copied_node));
  copied->executed_ = executed_;
  copied->results_ = results_;
  copied->result_type_ = result_type_;
  copied->result_len_ = result_len_;
  copied->set_require_single_column(require_single_column_);
  return copied;
}

void SubqueryExpr::reset_cache() const
{
  if (!subquery_node_) {
    // Value-list variants already hold their results and do not need clearing.
    return;
  }
  executed_ = false;
  results_.clear();
  result_type_ = AttrType::UNDEFINED;
  result_len_  = -1;
}

RC SubqueryExpr::execute_once() const
{
  if (executed_) {
    return RC::SUCCESS;
  }
  if (!subquery_node_ || subquery_node_->flag != SCF_SELECT) {
    LOG_WARN("subquery node invalid or not select");
    return RC::INVALID_ARGUMENT;
  }

  Session *session = Session::current_session();
  if (session == nullptr) {
    LOG_WARN("no current session to execute subquery");
    return RC::INTERNAL;
  }

  Db *db = session->get_current_db();
  if (db == nullptr) {
    LOG_WARN("no current db to execute subquery");
    return RC::INTERNAL;
  }

  // 鍒涘缓 SelectStmt锛堟敞鎰忥細蹇呴』瀵?ParsedSqlNode 鍋氭繁鎷疯礉锛岄伩鍏嶅湪缁戝畾闃舵绉诲姩/淇敼鍘?AST锛?  // 褰卞搷鍚庣画(鍙兘鐨?鍐嶆鎵ц鎴栫浉鍏冲瓙鏌ヨ鏇挎崲鏃剁殑娣辨嫹璐濓級銆?
Stmt *stmt = nullptr;
  std::unique_ptr<ParsedSqlNode> sub_node_copy = deep_copy_parsed_node(*subquery_node_);
  RC rc = Stmt::create_stmt(db, *sub_node_copy, stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create stmt for subquery. rc=%s", strrc(rc));
    return rc;
  }

  std::unique_ptr<Stmt> stmt_guard(stmt);
  auto *select_stmt = dynamic_cast<SelectStmt *>(stmt);
  if (select_stmt == nullptr) {
    LOG_WARN("subquery is not select stmt");
    return RC::INVALID_ARGUMENT;
  }

  // 鐢熸垚閫昏緫/鐗╃悊璁″垝
  std::unique_ptr<LogicalOperator> logical_oper;
  LogicalPlanGenerator              logical_gen;
  rc = logical_gen.create(select_stmt, logical_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create logical plan for subquery. rc=%s", strrc(rc));
    return rc;
  }

  std::unique_ptr<PhysicalOperator> physical_oper;
  PhysicalPlanGenerator             physical_gen;
  rc = physical_gen.create(*logical_oper, physical_oper, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create physical plan for subquery. rc=%s", strrc(rc));
    return rc;
  }

  // 鎵撳紑骞舵墽琛?
rc = physical_oper->open(session->current_trx());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open physical operator for subquery. rc=%s", strrc(rc));
    return rc;
  }

  // 鏍￠獙浠呬竴鍒楄緭鍑?
TupleSchema schema;
  rc = physical_oper->tuple_schema(schema);
  if (OB_FAIL(rc)) {
    // 鏈変簺鐗╃悊绠楀瓙鍙兘鏈疄鐜?tuple_schema锛岃繖绉嶆儏鍐典笅閫氳繃棣栬鎺ㄦ柇
    LOG_TRACE("tuple_schema not provided, will infer from first row");
  }

  bool schema_checked = false;
  if (rc == RC::SUCCESS && schema.cell_num() > 0) {
    if (require_single_column_ && schema.cell_num() != 1) {
      physical_oper->close();
      LOG_WARN("subquery must return exactly one column");
      sql_debug("subquery must return exactly one column");
      return RC::INVALID_ARGUMENT;
    }
    schema_checked = true;
  }

  results_.clear();

  while ((rc = physical_oper->next()) == RC::SUCCESS) {
    Tuple *tuple = physical_oper->current_tuple();
    if (tuple == nullptr) {
      rc = RC::INTERNAL;
      LOG_WARN("null tuple from subquery operator");
      break;
    }
    if (require_single_column_ && !schema_checked && tuple->cell_num() != 1) {
      LOG_WARN("subquery must return exactly one column");
      sql_debug("subquery must return exactly one column");
      rc = RC::INVALID_ARGUMENT;
      break;
    }

    Value cell;
    rc = tuple->cell_at(0, cell);
    if (OB_FAIL(rc)) {
      LOG_WARN("failed to get cell from subquery tuple. rc=%s", strrc(rc));
      break;
    }
    results_.push_back(cell);
  }

  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }

  physical_oper->close();

  // 璁板綍缁撴灉绫诲瀷
  if (!results_.empty()) {
    result_type_ = results_.front().attr_type();
    result_len_  = results_.front().length();
    LOG_INFO("[SUBQUERY] execute_once completed: num_results=%zu, first_value=%s",
              results_.size(), results_.front().to_string().c_str());
  } else {
    // 娌℃湁缁撴灉锛岄粯璁ょ被鍨嬫部鐢?UNDEFINED
    result_type_ = AttrType::UNDEFINED;
    result_len_  = -1;
    LOG_INFO("[SUBQUERY] execute_once completed: no results");
  }

  executed_ = true;
  return rc;
}

RC SubqueryExpr::get_value(const Tuple &tuple, Value &value) const
{
  RC rc = execute_with_context(&tuple);
  if (OB_FAIL(rc)) {
    return rc;
  }
  if (results_.empty()) {
    value.set_null();
    return RC::SUCCESS;
  }
  if (results_.size() > 1) {
    LOG_WARN("scalar subquery returned more than one row: %zu", results_.size());
    sql_debug("scalar subquery returned more than one row: %zu", results_.size());
    return RC::INVALID_ARGUMENT;
  }
  value = results_[0];
  return RC::SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// ExistsExpr

ExistsExpr::ExistsExpr(std::unique_ptr<Expression> subquery, bool negated)
    : subquery_(std::move(subquery)), negated_(negated)
{}

ExistsExpr::ExistsExpr(Expression *subquery, bool negated)
    : subquery_(subquery), negated_(negated)
{}

std::unique_ptr<Expression> ExistsExpr::copy() const
{
  std::unique_ptr<Expression> sub_copy;
  if (subquery_) {
    sub_copy = subquery_->copy();
  }
  auto copied = std::make_unique<ExistsExpr>(std::move(sub_copy), negated_);
  copied->set_name(name());
  if (alias() != nullptr) {
    copied->set_alias(std::string(alias()));
  }
  return copied;
}

RC ExistsExpr::get_value(const Tuple &tuple, Value &value) const
{
  if (!subquery_ || subquery_->type() != ExprType::SUBQUERY) {
    LOG_WARN("exists expression requires subquery child");
    return RC::INVALID_ARGUMENT;
  }

  auto *subq = static_cast<SubqueryExpr *>(subquery_.get());
  RC rc      = subq->execute_with_context(&tuple);
  if (OB_FAIL(rc)) {
    LOG_WARN("EXISTS subquery execution failed. rc=%s", strrc(rc));
    return rc;
  }

  bool has_rows = !subq->results().empty();
  bool result_value = negated_ ? !has_rows : has_rows;
  LOG_INFO("[EXISTS] has_rows=%d, negated=%d, result=%d, num_results=%zu",
            has_rows, negated_, result_value, subq->results().size());
  value.set_boolean(result_value);
  return RC::SUCCESS;
}

RC ExistsExpr::try_get_value(Value &value) const
{
  if (!subquery_ || subquery_->type() != ExprType::SUBQUERY) {
    return RC::INVALID_ARGUMENT;
  }
  auto *subq = static_cast<SubqueryExpr *>(subquery_.get());
  RC rc      = subq->execute_with_context(nullptr);
  if (OB_FAIL(rc)) {
    return rc;
  }
  bool has_rows = !subq->results().empty();
  value.set_boolean(negated_ ? !has_rows : has_rows);
  return RC::SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// ScalarFunctionExpr

RC ScalarFunctionExpr::get_value(const Tuple &tuple, Value &value) const
{
  if (!child_) {
    return RC::INVALID_ARGUMENT;
  }
  Value arg;
  RC rc = child_->get_value(tuple, arg);
  if (OB_FAIL(rc)) {
    return rc;
  }
  switch (func_type_) {
    case FuncType::LENGTH: {
      if (arg.attr_type() != AttrType::CHARS) {
        return RC::INVALID_ARGUMENT;
      }
      auto s = arg.get_string_t();
      value.set_int(static_cast<int>(s.size()));
      return RC::SUCCESS;
    }
    case FuncType::ROUND: {
      if (arg.attr_type() != AttrType::FLOATS) {
        return RC::INVALID_ARGUMENT;
      }
      int scale = 0;
      if (child2_) {
        Value s;
        rc = child2_->get_value(tuple, s);
        if (OB_FAIL(rc)) return rc;
        if (s.attr_type() != AttrType::INTS) {
          return RC::INVALID_ARGUMENT;
        }
        scale = s.get_int();
      }
      double f  = static_cast<double>(arg.get_float());
      double rf;
      if (scale == 0) {
        rf = round_half_to_even(f);
      } else if (scale > 0) {
        double p = std::pow(10.0, static_cast<double>(scale));
        rf       = round_half_to_even(f * p) / p;
      } else { // scale < 0
        double p = std::pow(10.0, static_cast<double>(-scale));
        rf       = round_half_to_even(f / p) * p;
      }
      value.set_float(static_cast<float>(rf));
      return RC::SUCCESS;
    }
    case FuncType::DATE_FORMAT: {
      // 支持第一个参数为 DATE 或 CHAR(可解析为日期)
      Value date_val;
      if (arg.attr_type() == AttrType::DATES) {
        date_val = arg;
      } else if (arg.attr_type() == AttrType::CHARS) {
        RC rc2 = Value::cast_to(arg, AttrType::DATES, date_val);
        if (OB_FAIL(rc2)) return rc2;
      } else {
        return RC::INVALID_ARGUMENT;
      }

      int32_t d = date_val.get_date();
      int year  = d / 10000;
      int month = (d / 100) % 100;
      int day   = d % 100;

      // 默认格式：%Y-%m-%d
      string fmt = "%Y-%m-%d";
      if (child2_) {
        Value fmt_val;
        RC rc2 = child2_->get_value(tuple, fmt_val);
        if (OB_FAIL(rc2)) return rc2;
        if (fmt_val.attr_type() != AttrType::CHARS) return RC::INVALID_ARGUMENT;
        fmt = fmt_val.get_string();
      }

      // 仅实现必要子集：%Y/%y/%m/%d 及普通字符透传
      string out;
      out.reserve(fmt.size() + 8);
      for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
          char t = fmt[i + 1];
          i++;
          char buf[8];
          switch (t) {
            case 'Y': snprintf(buf, sizeof(buf), "%04d", year); out.append(buf); break;
            case 'y': snprintf(buf, sizeof(buf), "%02d", year % 100); out.append(buf); break;
            case 'm': snprintf(buf, sizeof(buf), "%02d", month); out.append(buf); break;
            case 'd': snprintf(buf, sizeof(buf), "%02d", day); out.append(buf); break;
            case 'D': {
              const char *suffix = "th";
              if (day % 100 < 11 || day % 100 > 13) {
                switch (day % 10) {
                  case 1: suffix = "st"; break;
                  case 2: suffix = "nd"; break;
                  case 3: suffix = "rd"; break;
                  default: break;
                }
              }
              char buf2[16];
              snprintf(buf2, sizeof(buf2), "%d%s", day, suffix);
              out.append(buf2);
            } break;
            case 'M': {
              static const char *months[] = {
                "", "January", "February", "March", "April", "May", "June",
                "July", "August", "September", "October", "November", "December"
              };
              if (month >= 1 && month <= 12) {
                out.append(months[month]);
              } else {
                out.append("?");
              }
            } break;
            case '%': out.push_back('%'); break;
            default:  // 未识别占位符，按原样输出占位符字符
              out.push_back(t);
              break;
          }
        } else {
          out.push_back(fmt[i]);
        }
      }
      value.set_string(out.c_str());
      return RC::SUCCESS;
    }
    case FuncType::L2_DISTANCE:
    case FuncType::COSINE_DISTANCE:
    case FuncType::INNER_PRODUCT: {
      if (!child2_) return RC::INVALID_ARGUMENT;
      Value arg2;
      rc = child2_->get_value(tuple, arg2);
      if (OB_FAIL(rc)) return rc;

      // 提前检查 NULL 值，避免不必要的类型转换
      if (arg.attr_type() == AttrType::NULLS || arg2.attr_type() == AttrType::NULLS) {
        value.set_null();
        return RC::SUCCESS;
      }

      // 支持字符串字面量到向量的自动转换
      Value vec_arg1 = arg;
      Value vec_arg2 = arg2;
      if (arg.attr_type() != AttrType::VECTORS) {
        rc = Value::cast_to(arg, AttrType::VECTORS, vec_arg1);
        if (OB_FAIL(rc)) {
          // 转换失败，返回错误
          return rc;
        }
      }
      if (arg2.attr_type() != AttrType::VECTORS) {
        rc = Value::cast_to(arg2, AttrType::VECTORS, vec_arg2);
        if (OB_FAIL(rc)) {
          // 转换失败，返回错误
          return rc;
        }
      }

      // 现在使用转换后的向量值
      const int len1 = vec_arg1.length();
      const int len2 = vec_arg2.length();
      if (len1 <= 0 || len2 <= 0) {
        value.set_null();
        return RC::SUCCESS;
      }
      if (len1 != len2) {
        return RC::INVALID_ARGUMENT;
      }
      const int dim = len1 / static_cast<int>(sizeof(float));
      const float *a = reinterpret_cast<const float *>(vec_arg1.data());
      const float *b = reinterpret_cast<const float *>(vec_arg2.data());
      double acc = 0.0;
      if (func_type_ == FuncType::L2_DISTANCE) {
        for (int i = 0; i < dim; ++i) {
          double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
          acc += d * d;
        }
        acc = std::sqrt(acc);
      } else if (func_type_ == FuncType::INNER_PRODUCT) {
        for (int i = 0; i < dim; ++i) acc += static_cast<double>(a[i]) * static_cast<double>(b[i]);
      } else { // COSINE_DISTANCE
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (int i = 0; i < dim; ++i) {
          double va = static_cast<double>(a[i]);
          double vb = static_cast<double>(b[i]);
          dot += va * vb; na += va * va; nb += vb * vb;
        }
        if (na <= 0.0 || nb <= 0.0) {
        // 零向量或无效向量，无法计算余弦距离
        LOG_WARN("Cannot compute cosine distance with zero vector (na=%.10f, nb=%.10f)", na, nb);
        value.set_null();
        return RC::SUCCESS;
      }
        double cos = dot / (std::sqrt(na) * std::sqrt(nb));
        acc = 1.0 - cos;
        // 数值抖动可能导致 acc 落到极小的负值，需对理论上应为 0 的结果钳位
        // 使用更严格的阈值确保数值稳定性
        if (acc < 0.0 && std::fabs(acc) < 1e-8) {
          acc = 0.0;
        }
        // 余弦距离理论上应该在[0,2]范围内，进行边界保护
        if (acc < 0.0) acc = 0.0;
        if (acc > 2.0) acc = 2.0;
      }
      // 保留两位小数（与 ROUND 使用的一致的银行家舍入）
      double p = std::pow(10.0, 2.0);
      double rf = round_half_to_even(acc * p) / p;
      value.set_float(static_cast<float>(rf));
      return RC::SUCCESS;
    }
    case FuncType::STRING_TO_VECTOR: {
      Value vec_value;
      rc = parse_string_like_to_vector(arg, vec_value);
      if (OB_FAIL(rc)) {
        return rc;
      }
      value = std::move(vec_value);
      return RC::SUCCESS;
    }
    case FuncType::TOKENIZE: {
      Value *parser_ptr = nullptr;
      Value  parser_value;
      if (child2_ != nullptr) {
        rc = child2_->get_value(tuple, parser_value);
        if (OB_FAIL(rc)) {
          return rc;
        }
        parser_ptr = &parser_value;
      }
      return eval_tokenize_value(arg, parser_ptr, value);
    }
    case FuncType::VECTOR_TO_STRING: {
      // VECTOR_TO_STRING: 将向量转换为字符串 "[1,2,3]"
      if (arg.attr_type() == AttrType::NULLS) {
        value.set_null();
        return RC::SUCCESS;
      }
      Value vec_value;
      if (arg.attr_type() == AttrType::VECTORS) {
        vec_value = arg;
      } else {
        rc = Value::cast_to(arg, AttrType::VECTORS, vec_value);
        if (OB_FAIL(rc)) {
          return rc;
        }
      }
      string result_str;
      rc = DataType::type_instance(AttrType::VECTORS)->to_string(vec_value, result_str);
      if (OB_FAIL(rc)) return rc;
      value.set_string(result_str.c_str());
      return RC::SUCCESS;
    }
    case FuncType::DISTANCE: {
      // DISTANCE(vector1, vector2, distance_type)
      if (!child2_ || !child3_) return RC::INVALID_ARGUMENT;
      Value arg2, arg3;
      rc = child2_->get_value(tuple, arg2);
      if (OB_FAIL(rc)) return rc;
      rc = child3_->get_value(tuple, arg3);
      if (OB_FAIL(rc)) return rc;

      Value vec_arg1 = arg;
      Value vec_arg2 = arg2;
      if (arg.attr_type() != AttrType::VECTORS) {
        rc = Value::cast_to(arg, AttrType::VECTORS, vec_arg1);
        if (OB_FAIL(rc)) {
          return rc;
        }
      }
      if (arg2.attr_type() != AttrType::VECTORS) {
        rc = Value::cast_to(arg2, AttrType::VECTORS, vec_arg2);
        if (OB_FAIL(rc)) {
          return rc;
        }
      }

      Value dist_literal = arg3;
      if (arg3.attr_type() != AttrType::CHARS && arg3.attr_type() != AttrType::TEXTS) {
        rc = Value::cast_to(arg3, AttrType::CHARS, dist_literal);
        if (OB_FAIL(rc)) {
          return rc;
        }
      }

      const int len1 = vec_arg1.length();
      const int len2 = vec_arg2.length();
      if (len1 <= 0 || len2 <= 0) {
        value.set_null();
        return RC::SUCCESS;
      }
      if (len1 != len2 || (len1 % static_cast<int>(sizeof(float)) != 0)) {
        return RC::INVALID_ARGUMENT;
      }

      const int dim = len1 / static_cast<int>(sizeof(float));
      const float *a = reinterpret_cast<const float *>(vec_arg1.data());
      const float *b = reinterpret_cast<const float *>(vec_arg2.data());
      string dist_type = dist_literal.get_string();

      // 转换为大写
      for (char &c : dist_type) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }

      double acc = 0.0;
      if (dist_type == "EUCLIDEAN") {
        for (int i = 0; i < dim; ++i) {
          double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
          acc += d * d;
        }
        acc = std::sqrt(acc);
      } else if (dist_type == "DOT") {
        for (int i = 0; i < dim; ++i) acc += static_cast<double>(a[i]) * static_cast<double>(b[i]);
      } else if (dist_type == "COSINE") {
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (int i = 0; i < dim; ++i) {
          double va = static_cast<double>(a[i]);
          double vb = static_cast<double>(b[i]);
          dot += va * vb; na += va * va; nb += vb * vb;
        }
        if (na <= 0.0 || nb <= 0.0) {
        // 零向量或无效向量，无法计算余弦距离
        LOG_WARN("Cannot compute cosine distance with zero vector (na=%.10f, nb=%.10f)", na, nb);
        value.set_null();
        return RC::SUCCESS;
      }
        double cos = dot / (std::sqrt(na) * std::sqrt(nb));
        acc = 1.0 - cos;
        if (acc < 0.0 && std::fabs(acc) < 1e-6) {
          acc = 0.0;
        }
      } else {
        return RC::INVALID_ARGUMENT;  // 不支持的距离类型
      }

      value.set_float(static_cast<float>(acc));
      return RC::SUCCESS;
    }
  }
  return RC::UNIMPLEMENTED;
}

RC ScalarFunctionExpr::try_get_value(Value &value) const
{
  if (!child_) {
    return RC::INVALID_ARGUMENT;
  }
  Value arg;
  RC rc = child_->try_get_value(arg);
  if (OB_FAIL(rc)) {
    return rc;
  }
  switch (func_type_) {
    case FuncType::LENGTH: {
      if (arg.attr_type() != AttrType::CHARS) {
        return RC::INVALID_ARGUMENT;
      }
      auto s = arg.get_string_t();
      value.set_int(static_cast<int>(s.size()));
      return RC::SUCCESS;
    }
    case FuncType::ROUND: {
      if (arg.attr_type() != AttrType::FLOATS) {
        return RC::INVALID_ARGUMENT;
      }
      int scale = 0;
      if (child2_) {
        Value s;
        rc = child2_->try_get_value(s);
        if (OB_FAIL(rc)) return rc;
        if (s.attr_type() != AttrType::INTS) {
          return RC::INVALID_ARGUMENT;
        }
        scale = s.get_int();
      }
      double f  = static_cast<double>(arg.get_float());
      double rf;
      if (scale == 0) {
        rf = round_half_to_even(f);
      } else if (scale > 0) {
        double p = std::pow(10.0, static_cast<double>(scale));
        rf       = round_half_to_even(f * p) / p;
      } else {
        double p = std::pow(10.0, static_cast<double>(-scale));
        rf       = round_half_to_even(f / p) * p;
      }
      value.set_float(static_cast<float>(rf));
      return RC::SUCCESS;
    }
    case FuncType::DATE_FORMAT: {
      // 支持常量折叠
      Value date_val;
      if (arg.attr_type() == AttrType::DATES) {
        date_val = arg;
      } else if (arg.attr_type() == AttrType::CHARS) {
        RC rc2 = Value::cast_to(arg, AttrType::DATES, date_val);
        if (OB_FAIL(rc2)) return rc2;
      } else {
        return RC::INVALID_ARGUMENT;
      }

      int32_t d = date_val.get_date();
      int year  = d / 10000;
      int month = (d / 100) % 100;
      int day   = d % 100;

      string fmt = "%Y-%m-%d";
      if (child2_) {
        Value fmt_val;
        RC rc2 = child2_->try_get_value(fmt_val);
        if (OB_FAIL(rc2)) return rc2;
        if (fmt_val.attr_type() != AttrType::CHARS) return RC::INVALID_ARGUMENT;
        fmt = fmt_val.get_string();
      }

      string out;
      out.reserve(fmt.size() + 8);
      for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == '%' && i + 1 < fmt.size()) {
          char t = fmt[i + 1];
          i++;
          char buf[8];
          switch (t) {
            case 'Y': snprintf(buf, sizeof(buf), "%04d", year); out.append(buf); break;
            case 'y': snprintf(buf, sizeof(buf), "%02d", year % 100); out.append(buf); break;
            case 'm': snprintf(buf, sizeof(buf), "%02d", month); out.append(buf); break;
            case 'd': snprintf(buf, sizeof(buf), "%02d", day); out.append(buf); break;
            case 'D': {
              const char *suffix = "th";
              if (day % 100 < 11 || day % 100 > 13) {
                switch (day % 10) {
                  case 1: suffix = "st"; break;
                  case 2: suffix = "nd"; break;
                  case 3: suffix = "rd"; break;
                  default: break;
                }
              }
              char buf2[16];
              snprintf(buf2, sizeof(buf2), "%d%s", day, suffix);
              out.append(buf2);
            } break;
            case 'M': {
              static const char *months[] = {
                "", "January", "February", "March", "April", "May", "June",
                "July", "August", "September", "October", "November", "December"
              };
              if (month >= 1 && month <= 12) {
                out.append(months[month]);
              } else {
                out.append("?");
              }
            } break;
            case '%': out.push_back('%'); break;
            default:  out.push_back(t); break;
          }
        } else {
          out.push_back(fmt[i]);
        }
      }
      value.set_string(out.c_str());
      return RC::SUCCESS;
    }
    case FuncType::L2_DISTANCE:
    case FuncType::COSINE_DISTANCE:
    case FuncType::INNER_PRODUCT: {
      // 支持常量折叠：如果两个参数都是常量，可以直接计算
      if (!child2_) return RC::INVALID_ARGUMENT;

      Value arg1, arg2;
      RC rc1 = child_->try_get_value(arg1);
      RC rc2 = child2_->try_get_value(arg2);

      // 如果有任一参数不是常量，返回UNIMPLEMENTED（保持原有行为）
      if (OB_FAIL(rc1) || OB_FAIL(rc2)) {
        return RC::UNIMPLEMENTED;
      }

      // 提前检查 NULL 值，避免不必要的类型转换
      if (arg1.attr_type() == AttrType::NULLS || arg2.attr_type() == AttrType::NULLS) {
        value.set_null();
        return RC::SUCCESS;
      }

      // 执行类型转换和距离计算（复用get_value中的逻辑）
      Value vec_arg1 = arg1;
      Value vec_arg2 = arg2;
      if (arg1.attr_type() != AttrType::VECTORS) {
        rc = Value::cast_to(arg1, AttrType::VECTORS, vec_arg1);
        if (OB_FAIL(rc)) {
          return rc;
        }
      }
      if (arg2.attr_type() != AttrType::VECTORS) {
        rc = Value::cast_to(arg2, AttrType::VECTORS, vec_arg2);
        if (OB_FAIL(rc)) {
          return rc;
        }
      }

      const int len1 = vec_arg1.length();
      const int len2 = vec_arg2.length();
      if (len1 <= 0 || len2 <= 0) {
        value.set_null();
        return RC::SUCCESS;
      }
      if (len1 != len2 || (len1 % static_cast<int>(sizeof(float)) != 0) ||
          (len2 % static_cast<int>(sizeof(float)) != 0)) {
        return RC::INVALID_ARGUMENT;
      }
      const int dim = len1 / static_cast<int>(sizeof(float));
      const float *a = reinterpret_cast<const float *>(vec_arg1.data());
      const float *b = reinterpret_cast<const float *>(vec_arg2.data());
      double acc = 0.0;
      if (func_type_ == FuncType::L2_DISTANCE) {
        for (int i = 0; i < dim; ++i) {
          double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
          acc += d * d;
        }
        acc = std::sqrt(acc);
      } else if (func_type_ == FuncType::INNER_PRODUCT) {
        for (int i = 0; i < dim; ++i) {
          acc += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        }
      } else {  // COSINE_DISTANCE
        double dot = 0.0, na = 0.0, nb = 0.0;
        for (int i = 0; i < dim; ++i) {
          double va = static_cast<double>(a[i]);
          double vb = static_cast<double>(b[i]);
          dot += va * vb;
          na += va * va;
          nb += vb * vb;
        }
        if (na <= 0.0 || nb <= 0.0) {
          value.set_null();
          return RC::SUCCESS;
        }
        double cos = dot / (std::sqrt(na) * std::sqrt(nb));
        acc = 1.0 - cos;
        if (acc < 0.0 && std::fabs(acc) < 1e-6) {
          acc = 0.0;
        }
      }
      // 保留两位小数
      double p = std::pow(10.0, 2.0);
      double rf = round_half_to_even(acc * p) / p;
      value.set_float(static_cast<float>(rf));
      return RC::SUCCESS;
    }
    case FuncType::STRING_TO_VECTOR: {
      // STRING_TO_VECTOR 支持常量折叠：将字符串常量解析为向量
      if (arg.attr_type() == AttrType::NULLS) {
        value.set_null();
        return RC::SUCCESS;
      }
      Value vec_value;
      rc = parse_string_like_to_vector(arg, vec_value);
      if (OB_FAIL(rc)) {
        return rc;
      }
      value = std::move(vec_value);
      return RC::SUCCESS;
    }
    case FuncType::VECTOR_TO_STRING: {
      // VECTOR_TO_STRING 支持常量折叠：将向量常量转换为字符串
      if (arg.attr_type() == AttrType::NULLS) {
        value.set_null();
        return RC::SUCCESS;
      }
      Value vec_value;
      if (arg.attr_type() == AttrType::VECTORS) {
        vec_value = arg;
      } else {
        rc = Value::cast_to(arg, AttrType::VECTORS, vec_value);
        if (OB_FAIL(rc)) {
          return rc;
        }
      }
      string result_str;
      rc = DataType::type_instance(AttrType::VECTORS)->to_string(vec_value, result_str);
      if (OB_FAIL(rc)) return rc;
      value.set_string(result_str.c_str());
      return RC::SUCCESS;
    }
    case FuncType::TOKENIZE: {
      Value *parser_ptr = nullptr;
      Value  parser_value;
      if (child2_ != nullptr) {
        RC rc2 = child2_->try_get_value(parser_value);
        if (rc2 == RC::UNIMPLEMENTED) {
          return RC::UNIMPLEMENTED;
        }
        if (OB_FAIL(rc2)) {
          return rc2;
        }
        parser_ptr = &parser_value;
      }
      return eval_tokenize_value(arg, parser_ptr, value);
    }
    case FuncType::DISTANCE: {
      // DISTANCE 支持在所有参数均为常量时直接计算
      if (!child2_ || !child3_) {
        return RC::INVALID_ARGUMENT;
      }

      Value arg1;
      Value arg2;
      Value arg3;
      RC rc1 = child_->try_get_value(arg1);
      RC rc2 = child2_->try_get_value(arg2);
      RC rc3 = child3_->try_get_value(arg3);
      // 任意一个参数无法在编译期确定,则回退到执行期计算
      if (OB_FAIL(rc1) || OB_FAIL(rc2) || OB_FAIL(rc3)) {
        return RC::UNIMPLEMENTED;
      }

      // 向量参数若不是向量类型,尝试进行类型转换
      Value vec_arg1 = arg1;
      Value vec_arg2 = arg2;
      if (arg1.attr_type() != AttrType::VECTORS) {
        rc = Value::cast_to(arg1, AttrType::VECTORS, vec_arg1);
        if (OB_FAIL(rc)) {
          return rc;
        }
      }
      if (arg2.attr_type() != AttrType::VECTORS) {
        rc = Value::cast_to(arg2, AttrType::VECTORS, vec_arg2);
        if (OB_FAIL(rc)) {
          return rc;
        }
      }
      const int len1 = vec_arg1.length();
      const int len2 = vec_arg2.length();
      if (len1 <= 0 || len2 <= 0) {
        value.set_null();
        return RC::SUCCESS;
      }
      if (len1 != len2 || (len1 % static_cast<int>(sizeof(float)) != 0) ||
          (len2 % static_cast<int>(sizeof(float)) != 0)) {
        return RC::INVALID_ARGUMENT;
      }

      Value dist_arg = arg3;
      if (arg3.attr_type() != AttrType::CHARS && arg3.attr_type() != AttrType::TEXTS) {
        rc = Value::cast_to(arg3, AttrType::CHARS, dist_arg);
        if (OB_FAIL(rc)) {
          return rc;
        }
      }
      string dist_type = dist_arg.get_string();
      for (char &c : dist_type) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
      }

      const int dim = len1 / static_cast<int>(sizeof(float));
      const float *a = reinterpret_cast<const float *>(vec_arg1.data());
      const float *b = reinterpret_cast<const float *>(vec_arg2.data());

      double acc = 0.0;
      if (dist_type == "EUCLIDEAN") {
        for (int i = 0; i < dim; i++) {
          double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
          acc += d * d;
        }
        acc = std::sqrt(acc);
      } else if (dist_type == "DOT") {
        for (int i = 0; i < dim; i++) {
          acc += static_cast<double>(a[i]) * static_cast<double>(b[i]);
        }
      } else if (dist_type == "COSINE") {
        double dot = 0.0;
        double na  = 0.0;
        double nb  = 0.0;
        for (int i = 0; i < dim; i++) {
          double va = static_cast<double>(a[i]);
          double vb = static_cast<double>(b[i]);
          dot += va * vb;
          na += va * va;
          nb += vb * vb;
        }
        if (na <= 0.0 || nb <= 0.0) {
          value.set_null();
          return RC::SUCCESS;
        }
        double cos = dot / (std::sqrt(na) * std::sqrt(nb));
        acc = 1.0 - cos;
        if (acc < 0.0 && std::fabs(acc) < 1e-6) {
          acc = 0.0;
        }
      } else {
        return RC::INVALID_ARGUMENT;
      }

      value.set_float(static_cast<float>(acc));
      return RC::SUCCESS;
    }
  }
  return RC::UNIMPLEMENTED;
}

RC ScalarFunctionExpr::get_column(Chunk &chunk, Column &column)
{
  if (pos_ != -1) {
    column.reference(chunk.column(pos_));
    return RC::SUCCESS;
  }
  if (!child_) return RC::INVALID_ARGUMENT;

  Column arg_col;
  RC rc = child_->get_column(chunk, arg_col);
  if (OB_FAIL(rc)) {
    return rc;
  }
  Column scale_col; // for ROUND scale
  bool   has_scale = (func_type_ == FuncType::ROUND) && (child2_ != nullptr);
  if (has_scale) {
    rc = child2_->get_column(chunk, scale_col);
    if (OB_FAIL(rc)) return rc;
  }
  Column fmt_col; // for DATE_FORMAT format string
  bool   has_fmt = (func_type_ == FuncType::DATE_FORMAT) && (child2_ != nullptr);
  if (has_fmt) {
    rc = child2_->get_column(chunk, fmt_col);
    if (OB_FAIL(rc)) return rc;
  }
  Column arg2_col; bool has_arg2 = false;
  if (func_type_ == FuncType::L2_DISTANCE || func_type_ == FuncType::COSINE_DISTANCE || func_type_ == FuncType::INNER_PRODUCT || func_type_ == FuncType::DISTANCE) {
    if (!child2_) return RC::INVALID_ARGUMENT;
    has_arg2 = true;
    rc = child2_->get_column(chunk, arg2_col);
    if (OB_FAIL(rc)) return rc;
  }
  Column arg3_col; bool has_arg3 = false;
  if (func_type_ == FuncType::DISTANCE) {
    if (!child3_) return RC::INVALID_ARGUMENT;
    has_arg3 = true;
    rc = child3_->get_column(chunk, arg3_col);
    if (OB_FAIL(rc)) return rc;
  }
  Column parser_col; bool has_parser = false;
  if (func_type_ == FuncType::TOKENIZE && child2_ != nullptr) {
    has_parser = true;
    rc = child2_->get_column(chunk, parser_col);
    if (OB_FAIL(rc)) return rc;
  }

  const int rows = arg_col.count();
  column.init(value_type(), value_length(), rows);
  column.set_column_type(Column::Type::NORMAL_COLUMN);

  for (int i = 0; i < rows; ++i) {
    Value arg = arg_col.get_value(i);
    Value out;
    switch (func_type_) {
      case FuncType::LENGTH: {
        if (arg.attr_type() != AttrType::CHARS) return RC::INVALID_ARGUMENT;
        auto s = arg.get_string_t();
        out.set_int(static_cast<int>(s.size()));
      } break;
      case FuncType::ROUND: {
        if (arg.attr_type() != AttrType::FLOATS) return RC::INVALID_ARGUMENT;
        int scale = 0;
        if (has_scale) {
          Value s2 = scale_col.get_value(i);
          if (s2.attr_type() != AttrType::INTS) return RC::INVALID_ARGUMENT;
          scale = s2.get_int();
        }
        double f = static_cast<double>(arg.get_float());
        double rf;
        if (scale == 0) {
          rf = round_half_to_even(f);
        } else if (scale > 0) {
          double p = std::pow(10.0, static_cast<double>(scale));
          rf       = round_half_to_even(f * p) / p;
        } else {
          double p = std::pow(10.0, static_cast<double>(-scale));
          rf       = round_half_to_even(f / p) * p;
        }
        out.set_float(static_cast<float>(rf));
      } break;
      case FuncType::DATE_FORMAT: {
        // 取日期参数，容忍字符串可解析为日期
        Value date_val;
        if (arg.attr_type() == AttrType::DATES) {
          date_val = arg;
        } else if (arg.attr_type() == AttrType::CHARS) {
          RC rc2 = Value::cast_to(arg, AttrType::DATES, date_val);
          if (OB_FAIL(rc2)) return rc2;
        } else {
          return RC::INVALID_ARGUMENT;
        }
        int32_t d = date_val.get_date();
        int year  = d / 10000;
        int month = (d / 100) % 100;
        int day   = d % 100;

        string fmt = "%Y-%m-%d";
        if (has_fmt) {
          Value f2 = fmt_col.get_value(i);
          if (f2.attr_type() != AttrType::CHARS) return RC::INVALID_ARGUMENT;
          fmt = f2.get_string();
        }

        string out_s;
        out_s.reserve(fmt.size() + 8);
        for (size_t j = 0; j < fmt.size(); ++j) {
          if (fmt[j] == '%' && j + 1 < fmt.size()) {
            char t = fmt[j + 1];
            j++;
            char buf[8];
            switch (t) {
              case 'Y': snprintf(buf, sizeof(buf), "%04d", year); out_s.append(buf); break;
              case 'y': snprintf(buf, sizeof(buf), "%02d", year % 100); out_s.append(buf); break;
              case 'm': snprintf(buf, sizeof(buf), "%02d", month); out_s.append(buf); break;
              case 'd': snprintf(buf, sizeof(buf), "%02d", day); out_s.append(buf); break;
              case 'D': {
                const char *suffix = "th";
                if (day % 100 < 11 || day % 100 > 13) {
                  switch (day % 10) {
                    case 1: suffix = "st"; break;
                    case 2: suffix = "nd"; break;
                    case 3: suffix = "rd"; break;
                    default: break;
                  }
                }
                char buf2[16];
                snprintf(buf2, sizeof(buf2), "%d%s", day, suffix);
                out_s.append(buf2);
              } break;
              case 'M': {
                static const char *months[] = {
                  "", "January", "February", "March", "April", "May", "June",
                  "July", "August", "September", "October", "November", "December"
                };
                if (month >= 1 && month <= 12) {
                  out_s.append(months[month]);
                } else {
                  out_s.append("?");
                }
              } break;
              case '%': out_s.push_back('%'); break;
              default:  out_s.push_back(t); break;
            }
          } else {
            out_s.push_back(fmt[j]);
          }
        }
        out.set_string(out_s.c_str());
      } break;
      case FuncType::L2_DISTANCE:
      case FuncType::COSINE_DISTANCE:
      case FuncType::INNER_PRODUCT: {
        Value argb = has_arg2 ? arg2_col.get_value(i) : Value();

        // 支持字符串字面量到向量的自动转换（与 get_value 保持一致）
        Value vec_arg1 = arg;
        Value vec_arg2 = argb;

        // 类型转换：如果参数不是 VECTORS 类型，尝试转换
        if (arg.attr_type() != AttrType::VECTORS) {
          RC rc_cast = Value::cast_to(arg, AttrType::VECTORS, vec_arg1);
          if (OB_FAIL(rc_cast)) {
            return rc_cast;
          }
        }
        if (argb.attr_type() != AttrType::VECTORS) {
          RC rc_cast = Value::cast_to(argb, AttrType::VECTORS, vec_arg2);
          if (OB_FAIL(rc_cast)) {
            return rc_cast;
          }
        }

        // 现在使用转换后的向量值进行计算
        const int len1 = vec_arg1.length();
        const int len2 = vec_arg2.length();
        if (len1 <= 0 || len2 <= 0 || len1 != len2) { return RC::INVALID_ARGUMENT; }
        const int dim = len1 / static_cast<int>(sizeof(float));
        const float *a = reinterpret_cast<const float *>(vec_arg1.data());
        const float *b = reinterpret_cast<const float *>(vec_arg2.data());
        double acc = 0.0;
        if (func_type_ == FuncType::L2_DISTANCE) {
          for (int j = 0; j < dim; ++j) { double d = (double)a[j] - (double)b[j]; acc += d*d; }
          acc = std::sqrt(acc);
        } else if (func_type_ == FuncType::INNER_PRODUCT) {
          for (int j = 0; j < dim; ++j) acc += (double)a[j] * (double)b[j];
        } else { // COSINE_DISTANCE
          double dot = 0.0, na = 0.0, nb = 0.0;
          for (int j = 0; j < dim; ++j) { double va = (double)a[j], vb = (double)b[j]; dot += va*vb; na += va*va; nb += vb*vb; }
          if (na <= 0.0 || nb <= 0.0) { out.set_null(); break; }
          double cos = dot / (std::sqrt(na) * std::sqrt(nb));
          acc = 1.0 - cos;
        }
        double p = std::pow(10.0, 2.0); double rf = round_half_to_even(acc * p) / p;
        out.set_float(static_cast<float>(rf));
      } break;
      case FuncType::STRING_TO_VECTOR: {
        if (arg.attr_type() == AttrType::NULLS) {
          out.set_null();
          break;
        }
        Value vec_value;
        RC rc_vec = parse_string_like_to_vector(arg, vec_value);
        if (OB_FAIL(rc_vec)) {
          return rc_vec;
        }
        out = std::move(vec_value);
      } break;
      case FuncType::TOKENIZE: {
        if (arg.attr_type() == AttrType::NULLS) {
          out.set_null();
          break;
        }
        Value *parser_ptr = nullptr;
        Value  parser_value;
        if (has_parser) {
          parser_value = parser_col.get_value(i);
          parser_ptr   = &parser_value;
        }
        RC rc2 = eval_tokenize_value(arg, parser_ptr, out);
        if (OB_FAIL(rc2)) {
          return rc2;
        }
      } break;
      case FuncType::VECTOR_TO_STRING: {
        if (arg.attr_type() == AttrType::NULLS) {
          out.set_null();
          break;
        }
        Value vec_value;
        if (arg.attr_type() == AttrType::VECTORS) {
          vec_value = arg;
        } else {
          RC rc_cast = Value::cast_to(arg, AttrType::VECTORS, vec_value);
          if (OB_FAIL(rc_cast)) {
            out.set_null();
            break;
          }
        }
        string result_str;
        RC rc2 = DataType::type_instance(AttrType::VECTORS)->to_string(vec_value, result_str);
        if (OB_FAIL(rc2)) return rc2;
        out.set_string(result_str.c_str());
      } break;
      case FuncType::DISTANCE: {
        Value argb = has_arg2 ? arg2_col.get_value(i) : Value();
        Value argc = has_arg3 ? arg3_col.get_value(i) : Value();
        if (arg.attr_type() != AttrType::VECTORS || argb.attr_type() != AttrType::VECTORS) return RC::INVALID_ARGUMENT;
        if (argc.attr_type() != AttrType::CHARS) return RC::INVALID_ARGUMENT;

        const int len1 = arg.length();
        const int len2 = argb.length();
        if (len1 <= 0 || len2 <= 0) { out.set_null(); break; }
        if (len1 != len2) { return RC::INVALID_ARGUMENT; }

        const int dim = len1 / static_cast<int>(sizeof(float));
        const float *a = reinterpret_cast<const float *>(arg.data());
        const float *b = reinterpret_cast<const float *>(argb.data());
        string dist_type = argc.get_string();
        for (char &c : dist_type) {
          c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }

        double acc = 0.0;
        if (dist_type == "EUCLIDEAN") {
          for (int j = 0; j < dim; ++j) { double d = (double)a[j] - (double)b[j]; acc += d*d; }
          acc = std::sqrt(acc);
        } else if (dist_type == "DOT") {
          for (int j = 0; j < dim; ++j) acc += (double)a[j] * (double)b[j];
        } else if (dist_type == "COSINE") {
          double dot = 0.0, na = 0.0, nb = 0.0;
          for (int j = 0; j < dim; ++j) { double va = (double)a[j], vb = (double)b[j]; dot += va*vb; na += va*va; nb += vb*vb; }
          if (na <= 0.0 || nb <= 0.0) { out.set_null(); break; }
          double cos = dot / (std::sqrt(na) * std::sqrt(nb));
          acc = 1.0 - cos;
        } else {
          return RC::INVALID_ARGUMENT;
        }
        out.set_float(static_cast<float>(acc));
      } break;
    }
    column.append_value(out);
  }
  return RC::SUCCESS;
}
RC SubqueryExpr::execute_with_context(const Tuple *outer_tuple) const
{
  // 无上下文：走一次执行 + 缓存
  if (outer_tuple == nullptr) {
    return execute_once();
  }

  // 常量集合场景：使用缓存结果（如 IN (1,2,3) 被封装为缓存结果）
  if (!subquery_node_) {
    return execute_once();
  }
  if (subquery_node_->flag != SCF_SELECT) {
    LOG_WARN("subquery node invalid or not select");
    return RC::INVALID_ARGUMENT;
  }

  // 简单子查询快速路径：如果已经执行过，直接返回缓存结果
  // 这避免了对非相关子查询重复执行 deep_copy_parsed_node_with_ctx
  if (executed_) {
    LOG_INFO("[SUBQUERY] already executed, using cached results (size=%zu)", results_.size());
    return RC::SUCCESS;
  }

  // 鏋勯€犲甫甯搁噺鏇挎崲鐨勬柊 AST
  bool did_substitute = false;
  std::unique_ptr<ParsedSqlNode> copied = deep_copy_parsed_node_with_ctx(*subquery_node_, *outer_tuple, did_substitute);
  if (!copied) {
    LOG_WARN("failed to copy subquery node with ctx - this may indicate an issue with expression copying");
    return RC::INTERNAL;
  }

  LOG_INFO("[SUBQUERY] execute_with_context: did_substitute=%d", did_substitute);

  // 鑻ヤ笉瀛樺湪鐩稿叧寮曠敤锛岃惤鍥炰竴娆℃€х紦瀛樿矾寰?
if (!did_substitute) {
    LOG_INFO("[SUBQUERY] no correlated reference, falling back to execute_once");
    return execute_once();
  }

  // 鐩稿叧瀛愭煡璇細姣忔閲嶆柊鎵ц锛屼笉鍐欏叆 executed_ 缂撳瓨
  Session *session = Session::current_session();
  if (session == nullptr) {
    LOG_WARN("no current session to execute subquery");
    return RC::INTERNAL;
  }
  Db *db = session->get_current_db();
  if (db == nullptr) {
    LOG_WARN("no current db to execute subquery");
    return RC::INTERNAL;
  }

  Stmt *stmt = nullptr;
  RC rc = Stmt::create_stmt(db, *copied, stmt);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to create stmt for correlated subquery. rc=%s", strrc(rc));
    return rc;
  }
  std::unique_ptr<Stmt> stmt_guard(stmt);
  auto *select_stmt = dynamic_cast<SelectStmt *>(stmt);
  if (select_stmt == nullptr) {
    LOG_WARN("correlated subquery is not select stmt");
    return RC::INVALID_ARGUMENT;
  }

  std::unique_ptr<LogicalOperator> logical_oper;
  LogicalPlanGenerator              logical_gen;
  rc = logical_gen.create(select_stmt, logical_oper);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create logical plan for correlated subquery. rc=%s", strrc(rc));
    return rc;
  }

  std::unique_ptr<PhysicalOperator> physical_oper;
  PhysicalPlanGenerator             physical_gen;
  rc = physical_gen.create(*logical_oper, physical_oper, session);
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to create physical plan for correlated subquery. rc=%s", strrc(rc));
    return rc;
  }

  rc = physical_oper->open(session->current_trx());
  if (OB_FAIL(rc)) {
    LOG_WARN("failed to open physical operator for correlated subquery. rc=%s", strrc(rc));
    return rc;
  }

  TupleSchema schema;
  RC schema_rc = physical_oper->tuple_schema(schema);
  if (OB_FAIL(schema_rc)) {
    LOG_TRACE("tuple_schema not provided for correlated subquery, will infer from first row");
  }
  bool schema_checked = false;
  if (schema_rc == RC::SUCCESS && schema.cell_num() > 0) {
    if (require_single_column_ && schema.cell_num() != 1) {
      physical_oper->close();
      LOG_WARN("subquery must return exactly one column");
      sql_debug("subquery must return exactly one column");
      return RC::INVALID_ARGUMENT;
    }
    schema_checked = true;
  }

  // 娓呯┖骞舵敹闆嗙粨鏋?
results_.clear();
  result_type_ = AttrType::UNDEFINED;
  result_len_  = -1;

  Tuple *tuple = nullptr;
  while (RC::SUCCESS == (rc = physical_oper->next())) {
    tuple = physical_oper->current_tuple();
    if (tuple == nullptr) {
      rc = RC::INTERNAL;
      break;
    }
    if (require_single_column_ && !schema_checked && tuple->cell_num() != 1) {
      LOG_WARN("subquery must return exactly one column");
      sql_debug("subquery must return exactly one column");
      rc = RC::INVALID_ARGUMENT;
      break;
    }
    Value v;
    // 璇诲彇棣栧垪
    RC rc2 = tuple->cell_at(0, v);
    if (rc2 != RC::SUCCESS) {
      rc = rc2;
      break;
    }
    results_.push_back(v);
    if (result_type_ == AttrType::UNDEFINED) {
      result_type_ = v.attr_type();
      result_len_  = v.length();
    }
  }
  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }
  physical_oper->close();
  return rc;
}

std::unique_ptr<ParsedSqlNode> SubqueryExpr::deep_copy_parsed_node(const ParsedSqlNode &node) const
{
  auto copied = std::make_unique<ParsedSqlNode>(node.flag);
  if (node.flag == SCF_SELECT) {
    // 娣辨嫹璐?SelectSqlNode
    const SelectSqlNode &src = node.selection;
    SelectSqlNode       &dst = copied->selection;

    // expressions
    for (const auto &expr_ptr : src.expressions) {
      if (expr_ptr) {
        dst.expressions.emplace_back(expr_ptr->copy());
      }
    }
    // relations
    dst.relations = src.relations;
    // conditions
    dst.conditions.reserve(src.conditions.size());
    for (const auto &cond : src.conditions) {
      ConditionSqlNode new_cond;
      new_cond.left_is_attr  = cond.left_is_attr;
      new_cond.right_is_attr = cond.right_is_attr;
      new_cond.left_value    = cond.left_value;
      new_cond.right_value   = cond.right_value;
      new_cond.left_attr     = cond.left_attr;
      new_cond.right_attr    = cond.right_attr;
      new_cond.comp          = cond.comp;

      if (cond.left_expr) {
        new_cond.left_expr.reset(cond.left_expr->copy().release());
      }
      if (cond.right_expr) {
        new_cond.right_expr.reset(cond.right_expr->copy().release());
      }
      dst.conditions.emplace_back(std::move(new_cond));
    }
    // where expression
    if (src.where_expr) {
      dst.where_expr = src.where_expr->copy();
    }
    // group by
    for (const auto &grp : src.group_by) {
      if (grp) {
        dst.group_by.emplace_back(grp->copy());
      }
    }
    // having
    dst.having.reserve(src.having.size());
    for (const auto &cond : src.having) {
      ConditionSqlNode new_cond;
      new_cond.left_is_attr  = cond.left_is_attr;
      new_cond.right_is_attr = cond.right_is_attr;
      new_cond.left_value    = cond.left_value;
      new_cond.right_value   = cond.right_value;
      new_cond.left_attr     = cond.left_attr;
      new_cond.right_attr    = cond.right_attr;
      new_cond.comp          = cond.comp;
      if (cond.left_expr) {
        new_cond.left_expr.reset(cond.left_expr->copy().release());
      }
      if (cond.right_expr) {
        new_cond.right_expr.reset(cond.right_expr->copy().release());
      }
      dst.having.emplace_back(std::move(new_cond));
    }
    // order by
    for (const auto &ord : src.order_by) {
      OrderBySqlNode item;
      item.asc = ord.asc;
      if (ord.expression) {
        item.expression.reset(ord.expression->copy().release());
      }
      dst.order_by.emplace_back(std::move(item));
    }
    dst.limit = src.limit;
  }
  return copied;
}

std::unique_ptr<ParsedSqlNode> SubqueryExpr::deep_copy_parsed_node_with_ctx(
    const ParsedSqlNode &node, const Tuple &outer_tuple, bool &did_substitute) const
{
  did_substitute = false;
  auto copied    = std::make_unique<ParsedSqlNode>(node.flag);
  if (node.flag != SCF_SELECT) {
    return copied;
  }
  const SelectSqlNode &src = node.selection;
  SelectSqlNode       &dst = copied->selection;

  // relations
  dst.relations = src.relations;

  // 准备内层关系名（含表名和别名），用于判断是否外层引用
  std::vector<std::string> inner_names;
  inner_names.reserve(src.relations.size() * 2);
  for (const auto &r : src.relations) {
    inner_names.push_back(r.relation_name);
    if (!r.alias.empty()) inner_names.push_back(r.alias);
  }

  // expressions (SELECT 列表) 做相关引用替换
  for (const auto &expr_ptr : src.expressions) {
    if (expr_ptr) {
      bool sub = false; RC rc = RC::SUCCESS;
      auto new_expr = copy_and_substitute_outer_refs(*expr_ptr, inner_names, outer_tuple, sub, rc);
      if (!new_expr || rc != RC::SUCCESS) {
        LOG_WARN("[SUBQUERY] failed to copy/substitute expression in SELECT list: expr_type=%d, rc=%s",
                 static_cast<int>(expr_ptr->type()), strrc(rc));
        return nullptr;
      }
      dst.expressions.emplace_back(std::move(new_expr));
      did_substitute = did_substitute || sub;
    }
  }

  // conditions（WHERE AND 链）递归复制并替换
  dst.conditions.reserve(src.conditions.size());
  for (const auto &cond : src.conditions) {
    ConditionSqlNode new_cond;
    new_cond.left_is_attr  = cond.left_is_attr;
    new_cond.right_is_attr = cond.right_is_attr;
    new_cond.left_value    = cond.left_value;
    new_cond.right_value   = cond.right_value;
    new_cond.left_attr     = cond.left_attr;
    new_cond.right_attr    = cond.right_attr;
    new_cond.comp          = cond.comp;

    RC rc = RC::SUCCESS;
    bool sub = false;
    if (cond.left_expr) {
      auto left_new = copy_and_substitute_outer_refs(*cond.left_expr, inner_names, outer_tuple, sub, rc);
      if (!left_new || rc != RC::SUCCESS) {
        LOG_WARN("[SUBQUERY] failed to copy/substitute left condition expression: expr_type=%d, rc=%s",
                 static_cast<int>(cond.left_expr->type()), strrc(rc));
        return nullptr;
      }
      new_cond.left_expr.reset(left_new.release());
      did_substitute = did_substitute || sub;
    }
    sub = false;
    if (cond.right_expr) {
      auto right_new = copy_and_substitute_outer_refs(*cond.right_expr, inner_names, outer_tuple, sub, rc);
      if (!right_new || rc != RC::SUCCESS) {
        LOG_WARN("[SUBQUERY] failed to copy/substitute right condition expression: expr_type=%d, rc=%s",
                 static_cast<int>(cond.right_expr->type()), strrc(rc));
        return nullptr;
      }
      new_cond.right_expr.reset(right_new.release());
      did_substitute = did_substitute || sub;
    }
    dst.conditions.emplace_back(std::move(new_cond));
  }

  // where 表达式相关引用替换
  if (src.where_expr) {
    bool sub = false; RC rc = RC::SUCCESS;
    auto new_where = copy_and_substitute_outer_refs(*src.where_expr, inner_names, outer_tuple, sub, rc);
    if (!new_where || rc != RC::SUCCESS) {
      LOG_WARN("[SUBQUERY] failed to copy/substitute WHERE expression: expr_type=%d, rc=%s",
               static_cast<int>(src.where_expr->type()), strrc(rc));
      return nullptr;
    }
    dst.where_expr.reset(new_where.release());
    did_substitute = did_substitute || sub;
  }

  // group by（做相关引用替换）
  for (const auto &grp : src.group_by) {
    if (grp) {
      bool sub = false; RC rc = RC::SUCCESS;
      auto new_grp = copy_and_substitute_outer_refs(*grp, inner_names, outer_tuple, sub, rc);
      if (!new_grp || rc != RC::SUCCESS) {
        LOG_WARN("[SUBQUERY] failed to copy/substitute GROUP BY expression: expr_type=%d, rc=%s",
                 static_cast<int>(grp->type()), strrc(rc));
        return nullptr;
      }
      dst.group_by.emplace_back(std::move(new_grp));
      did_substitute = did_substitute || sub;
    }
  }
  // having（做相关引用替换）
  dst.having.reserve(src.having.size());
  for (const auto &cond : src.having) {
    ConditionSqlNode new_cond;
    new_cond.left_is_attr  = cond.left_is_attr;
    new_cond.right_is_attr = cond.right_is_attr;
    new_cond.left_value    = cond.left_value;
    new_cond.right_value   = cond.right_value;
    new_cond.left_attr     = cond.left_attr;
    new_cond.right_attr    = cond.right_attr;
    new_cond.comp          = cond.comp;
    RC rc = RC::SUCCESS;
    bool sub = false;
    if (cond.left_expr) {
      auto left_new = copy_and_substitute_outer_refs(*cond.left_expr, inner_names, outer_tuple, sub, rc);
      if (!left_new || rc != RC::SUCCESS) {
        LOG_WARN("[SUBQUERY] failed to copy/substitute left HAVING expression: expr_type=%d, rc=%s",
                 static_cast<int>(cond.left_expr->type()), strrc(rc));
        return nullptr;
      }
      new_cond.left_expr.reset(left_new.release());
      did_substitute = did_substitute || sub;
    }
    sub = false;
    if (cond.right_expr) {
      auto right_new = copy_and_substitute_outer_refs(*cond.right_expr, inner_names, outer_tuple, sub, rc);
      if (!right_new || rc != RC::SUCCESS) {
        LOG_WARN("[SUBQUERY] failed to copy/substitute right HAVING expression: expr_type=%d, rc=%s",
                 static_cast<int>(cond.right_expr->type()), strrc(rc));
        return nullptr;
      }
      new_cond.right_expr.reset(right_new.release());
      did_substitute = did_substitute || sub;
    }
    dst.having.emplace_back(std::move(new_cond));
  }
  // order by（做相关引用替换）
  for (const auto &ord : src.order_by) {
    OrderBySqlNode item;
    item.asc = ord.asc;
    if (ord.expression) {
      bool sub = false; RC rc = RC::SUCCESS;
      auto new_ord = copy_and_substitute_outer_refs(*ord.expression, inner_names, outer_tuple, sub, rc);
      if (!new_ord || rc != RC::SUCCESS) {
        LOG_WARN("[SUBQUERY] failed to copy/substitute ORDER BY expression: expr_type=%d, rc=%s",
                 static_cast<int>(ord.expression->type()), strrc(rc));
        return nullptr;
      }
      item.expression.reset(new_ord.release());
      did_substitute = did_substitute || sub;
    }
    dst.order_by.emplace_back(std::move(item));
  }
  dst.limit = src.limit;
  return copied;
}std::unique_ptr<Expression> SubqueryExpr::copy_and_substitute_outer_refs(
    const Expression &expr,
    const std::vector<std::string> &inner_relations,
    const Tuple &outer_tuple,
    bool &did_substitute,
    RC &rc) const
{
  rc             = RC::SUCCESS;
  did_substitute = false;

  auto is_inner_table = [&](const char *tname) -> bool {
    if (tname == nullptr || *tname == '\0') return false;
    for (const auto &r : inner_relations) {
      if (0 == strcasecmp(r.c_str(), tname)) return true;
    }
    return false;
  };

  if (expr.type() == ExprType::FIELD || expr.type() == ExprType::UNBOUND_FIELD) {
    const char *rel_name = nullptr;
    const char *field_name = nullptr;
    if (expr.type() == ExprType::FIELD) {
      const auto &field_expr = static_cast<const FieldExpr &>(expr);
      if (!field_expr.relation_name().empty()) {
        rel_name = field_expr.relation_name().c_str();
      } else {
        rel_name = field_expr.table_name();
      }
      field_name = field_expr.field_name();
    } else {
      const auto &u = static_cast<const UnboundFieldExpr &>(expr);
      rel_name      = u.table_name();
      field_name    = u.field_name();
    }

    auto belongs_to_inner = [&](const char *name) -> bool {
      if (name == nullptr || *name == '\0') {
        return false;
      }
      return is_inner_table(name);
    };

    const char *outer_table = nullptr;
    if (!belongs_to_inner(rel_name) && rel_name != nullptr && *rel_name != '\0') {
      outer_table = rel_name;
    } else if (expr.type() == ExprType::FIELD) {
      const auto &field_expr = static_cast<const FieldExpr &>(expr);
      const char *physical   = field_expr.table_name();
      if (!belongs_to_inner(physical) && physical != nullptr && *physical != '\0') {
        outer_table = physical;
      }
    }

    if (outer_table != nullptr && field_name != nullptr && *field_name != '\0') {
      LOG_INFO("[CORRELATED] identified outer reference: %s.%s", outer_table, field_name);
      Value v;
      RC    rc2 = outer_tuple.find_cell(TupleCellSpec(outer_table, field_name), v);
      if (rc2 != RC::SUCCESS) {
        string tf = string(outer_table) + "." + string(field_name);
        rc2       = outer_tuple.find_cell(TupleCellSpec(tf), v);
      }
      if (rc2 != RC::SUCCESS) {
        rc2 = outer_tuple.find_cell(TupleCellSpec(field_name), v);
      }
      if (rc2 != RC::SUCCESS) {
        LOG_WARN("failed to fetch correlated value %s.%s from outer tuple", outer_table, field_name);
        rc = rc2;
        return nullptr;
      }
      LOG_INFO("[CORRELATED] substituting %s.%s with value: %s",
                outer_table, field_name, v.to_string().c_str());
      did_substitute = true;
      auto ve        = std::make_unique<ValueExpr>(v);
      if (expr.alias() != nullptr) {
        ve->set_alias(expr.alias());
      }
      if (expr.name() != nullptr) {
        ve->set_name(expr.name());
      }
      return ve;
    }

    return expr.copy();
  }

  if (expr.type() == ExprType::SUBQUERY) {
    // 閫掑綊娣卞叆瀛愭煡璇紝缁х画瀵规洿鍐呭眰鐨勭浉鍏冲紩鐢ㄥ仛鏇挎崲
    const auto &sub_e = static_cast<const SubqueryExpr &>(expr);
    if (sub_e.subquery_node_ == nullptr) {
      // 宸茬紦瀛樼粨鏋滅殑瀛愭煡璇紝鐩存帴澶嶅埗
      return expr.copy();
    }
    bool inner_substituted = false;
    auto copied_node       = deep_copy_parsed_node_with_ctx(*sub_e.subquery_node_, outer_tuple, inner_substituted);
    if (!copied_node) {
      rc = RC::INTERNAL;
      return nullptr;
    }
    if (inner_substituted) {
      did_substitute = true;
    }
    auto new_sub = std::make_unique<SubqueryExpr>(std::move(copied_node));
    new_sub->set_require_single_column(sub_e.require_single_column());
    new_sub->set_name(expr.name());
    return new_sub;
  }

  // 鍏跺畠琛ㄨ揪寮忥細澶嶅埗鍚庨€掑綊澶勭悊瀛愯妭鐐?
auto copied = expr.copy();
  RC   tmp_rc = ExpressionIterator::iterate_child_expr(*copied, [&](std::unique_ptr<Expression> &child) -> RC {
    bool sub_flag = false;
    RC   inner_rc = RC::SUCCESS;
    auto new_ch   = copy_and_substitute_outer_refs(*child, inner_relations, outer_tuple, sub_flag, inner_rc);
    if (!new_ch || inner_rc != RC::SUCCESS) {
      return inner_rc;
    }
    if (sub_flag) did_substitute = true;
    child.reset(new_ch.release());
    return RC::SUCCESS;
  });
  if (tmp_rc != RC::SUCCESS) {
    rc = tmp_rc;
    return nullptr;
  }
  return copied;
}

////////////////////////////////////////////////////////////////////////////////
// InExpr

RC InExpr::get_value(const Tuple &tuple, Value &value) const
{
  // 姹傚乏鍊?
Value left_val;
  RC rc = test_expr_->get_value(tuple, left_val);
  if (OB_FAIL(rc)) {
    return rc;
  }

  if (left_val.is_null()) {
    // 绠€鍖栧鐞嗭細NULL 涓庨泦鍚堟瘮杈冧负 false锛堜笌鏍囧噯 SQL 鐨勪笁鍊奸€昏緫鍙兘涓嶅悓锛?
value.set_boolean(false);
    return RC::SUCCESS;
  }

  // Parser wraps the right side (including value lists) as SubqueryExpr.
  auto *subq = static_cast<SubqueryExpr *>(set_expr_.get());
  rc = subq->execute_with_context(&tuple);
  if (OB_FAIL(rc)) {
    return rc;
  }

  bool found = false;
  const auto &vals = subq->results();
  for (const auto &rv : vals) {
    if (rv.is_null()) {
      continue; // 蹇界暐 NULL
    }
    if (left_val.compare(rv) == 0) {
      found = true;
      break;
    }
  }

  bool result = not_in_ ? !found : found;
  value.set_boolean(result);
  return RC::SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// MatchAgainstExpr

RC MatchAgainstExpr::get_value(const Tuple &tuple, Value &value) const
{
  // 1. 获取搜索文本
  Value search_value;
  RC rc = search_text_->get_value(tuple, search_value);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get search text value. rc=%s", strrc(rc));
    return rc;
  }

  if (search_value.attr_type() != AttrType::CHARS && search_value.attr_type() != AttrType::TEXTS) {
    LOG_WARN("search text must be string type");
    return RC::INVALID_ARGUMENT;
  }

  std::string query = search_value.to_string();

  // 2. 如果索引未绑定，返回0分
  if (index_ == nullptr || table_ == nullptr) {
    LOG_WARN("full-text index not bound");
    value.set_float(0.0f);
    return RC::SUCCESS;
  }

  // 3. 从tuple中获取当前记录的RID
  RID rid;
  rc = tuple.get_record_id(rid);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get record id from tuple. rc=%s", strrc(rc));
    value.set_float(0.0f);
    return RC::SUCCESS;
  }

  // 4. 调用FullTextIndex的calculate_bm25_score方法计算评分
  // 需要将Index*转换为FullTextIndex*
  FullTextIndex *ft_index = dynamic_cast<FullTextIndex *>(index_);
  if (ft_index == nullptr) {
    LOG_WARN("index is not a full-text index");
    value.set_float(0.0f);
    return RC::SUCCESS;
  }

  float score = 0.0f;
  rc = ft_index->calculate_bm25_score(rid, query, score);
  if (rc != RC::SUCCESS) {
    // 如果计算失败（比如文档不在索引中），返回0分
    value.set_float(0.0f);
    return RC::SUCCESS;
  }

  value.set_float(score);
  return RC::SUCCESS;
}

////////////////////////////////////////////////////////////////////////////////
// 全局分词函数

RC jieba_tokenize(const std::string &text, const std::string &parser, std::vector<std::string> &tokens)
{
  return JiebaTokenizer::tokenize(text, parser, tokens);
}
