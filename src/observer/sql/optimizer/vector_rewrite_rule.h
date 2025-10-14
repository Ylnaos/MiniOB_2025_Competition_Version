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

#include "sql/optimizer/rewrite_rule.h"

/**
 * @brief 向量检索改写规则
 * @details 识别 order by L2_DISTANCE(...) + limit 查询并替换为向量索引扫描
 */
class VectorRewriteRule : public RewriteRule
{
public:
  VectorRewriteRule()  = default;
  ~VectorRewriteRule() override = default;

  RC rewrite(std::unique_ptr<LogicalOperator> &oper, bool &change_made) override;
};
