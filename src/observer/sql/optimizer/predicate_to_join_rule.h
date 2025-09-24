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

#include "common/lang/vector.h"
#include "sql/optimizer/rewrite_rule.h"

/**
 * @brief 将跨表谓词（典型ON条件）从全局谓词下推到JOIN算子
 * @ingroup Rewriter
 */
class PredicateToJoinRule : public RewriteRule
{
public:
  PredicateToJoinRule()          = default;
  virtual ~PredicateToJoinRule() = default;

  RC rewrite(std::unique_ptr<LogicalOperator> &oper, bool &change_made) override;
};
