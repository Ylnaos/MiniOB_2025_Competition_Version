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
// Created by Wangyunlai on 2022/5/22.
//

#pragma once

#include "common/sys/rc.h"
#include "common/value.h"
#include "sql/stmt/stmt.h"

class Table;
class Db;

/**
 * @brief 描述一次底层表的插入任务
 */
struct InsertTask
{
  Table                      *table = nullptr;
  vector<vector<Value>>       rows;
};

/**
 * @brief 插入语句
 * @ingroup Statement
 */
class InsertStmt : public Stmt
{
public:
  InsertStmt() = default;
  explicit InsertStmt(vector<InsertTask> tasks);
  InsertStmt(Table *table, vector<vector<Value>> values_rows);

  StmtType type() const override { return StmtType::INSERT; }

public:
  static RC create(Db *db, const InsertSqlNode &insert_sql, Stmt *&stmt);

public:
  Table *table() const { return insert_tasks_.empty() ? nullptr : insert_tasks_[0].table; }
  const vector<InsertTask> &insert_tasks() const { return insert_tasks_; }

private:
  vector<InsertTask> insert_tasks_;
};
