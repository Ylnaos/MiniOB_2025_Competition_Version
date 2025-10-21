/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#pragma once

#include <string>
#include <vector>

#include "common/sys/rc.h"

/**
 * @brief 中文分词辅助工具
 *
 * 该工具统一封装了 jieba 分词的加载、停用词过滤、标点/拉丁字符切分等逻辑，
 * 供 SQL 表达式 TOKENIZE 以及全文检索 MATCH ... AGAINST 复用。
 */
class TextTokenizer
{
public:
  /**
   * @brief 将输入文本分词
   *
   * @param text    原始文本内容
   * @param parser  分词器名称，当前仅支持 "jieba"（大小写不敏感）
   * @param tokens  输出的分词结果（会先清空再填充）
   */
  static RC tokenize(const std::string &text, const std::string &parser, std::vector<std::string> &tokens);
};

