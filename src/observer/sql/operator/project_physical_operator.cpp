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
// Created by WangYunlai on 2022/07/01.
//

#include "sql/operator/project_physical_operator.h"
#include "common/log/log.h"
#include "storage/record/record.h"
#include "storage/table/table.h"

using namespace std;

// 全局计数器，用于追踪投影操作
static int project_open_count = 0;
static int project_next_count = 0;
static int project_tuples_output = 0;

ProjectPhysicalOperator::ProjectPhysicalOperator(vector<unique_ptr<Expression>> &&expressions)
  : expressions_(std::move(expressions)), tuple_(expressions_)
{
}

RC ProjectPhysicalOperator::open(Trx *trx)
{
  project_open_count++;
  LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::open - 操作符=%p, 表达式数量=%zu, 打开次数=%d",
            this, expressions_.size(), project_open_count);

  if (children_.empty()) {
    LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::open - 操作符=%p, 无子操作符", this);
    return RC::SUCCESS;
  }

  LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::open子操作符 - 操作符=%p, 子操作符=%p", this, children_[0].get());

  PhysicalOperator *child = children_[0].get();
  RC                rc    = child->open(trx);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to open child operator: %s", strrc(rc));
    LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::open失败 - 操作符=%p, 子操作符打开失败, RC=%s", this, strrc(rc));
    return rc;
  }

  LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::open成功 - 操作符=%p, 子操作符打开成功", this);
  return RC::SUCCESS;
}

RC ProjectPhysicalOperator::next()
{
  project_next_count++;
  LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::next开始 - 操作符=%p, next调用次数=%d", this, project_next_count);

  if (children_.empty()) {
    LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::next - 操作符=%p, 无子操作符，返回EOF", this);
    return RC::RECORD_EOF;
  }

  RC rc = children_[0]->next();
  if (rc == RC::SUCCESS) {
    project_tuples_output++;
    LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::next成功 - 操作符=%p, 获取到数据，已输出元组数=%d", this, project_tuples_output);

    // 记录投影表达式计算结果
    if (children_[0]->current_tuple()) {
      LOG_INFO("DATA_FLOW: ProjectPhysicalOperator输入元组 - 操作符=%p, 输入数据=%s", this, children_[0]->current_tuple()->to_string().c_str());
      LOG_INFO("DATA_FLOW: ProjectPhysicalOperator输出元组 - 操作符=%p, 输出数据=%s", this, tuple_.to_string().c_str());
    }
  } else if (rc == RC::RECORD_EOF) {
    LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::next完成 - 操作符=%p, 数据读取完毕, 总next调用=%d, 总输出元组=%d", this, project_next_count, project_tuples_output);
  } else {
    LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::next错误 - 操作符=%p, RC=%s", this, strrc(rc));
  }

  return rc;
}

RC ProjectPhysicalOperator::close()
{
  if (!children_.empty()) {
    children_[0]->close();
  }
  return RC::SUCCESS;
}
Tuple *ProjectPhysicalOperator::current_tuple()
{
  if (!children_.empty() && children_[0]->current_tuple()) {
    tuple_.set_tuple(children_[0]->current_tuple());
    LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::current_tuple - 操作符=%p, 输出元组=%s", this, tuple_.to_string().c_str());
  } else {
    LOG_INFO("DATA_FLOW: ProjectPhysicalOperator::current_tuple - 操作符=%p, 无有效数据", this);
  }
  return &tuple_;
}

RC ProjectPhysicalOperator::tuple_schema(TupleSchema &schema) const
{
  for (const unique_ptr<Expression> &expression : expressions_) {
    schema.append_cell(expression->name());
  }
  return RC::SUCCESS;
}