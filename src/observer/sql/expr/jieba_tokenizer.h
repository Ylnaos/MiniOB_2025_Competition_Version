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

#include "common/sys/rc.h"
#include <string>
#include <vector>

/**
 * @brief 提供基于 cppjieba 的中文分词能力
 *
 * 该工具类负责定位词典目录、初始化分词器，并封装去停用词逻辑。
 * 对外暴露 tokenize 接口，返回过滤停用词后的分词结果。
 */
class JiebaTokenizer
{
public:
  /**
   * @brief 对文本进行分词
   *
   * @param text   输入文本
   * @param parser 分词器名称，仅支持 "jieba" 或空串
   * @param tokens 输出分词结果，已去除停用词与空白
   *
   * @return RC::SUCCESS 表示分词成功，其他返回码表示初始化或分词失败
   */
  static RC tokenize(const std::string &text, const std::string &parser, std::vector<std::string> &tokens);
};

