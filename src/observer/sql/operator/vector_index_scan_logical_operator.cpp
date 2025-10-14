/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "sql/operator/vector_index_scan_logical_operator.h"

#include "storage/table/table.h"
#include "storage/index/index.h"

VectorIndexScanLogicalOperator::VectorIndexScanLogicalOperator(
    Table *table, Index *index, std::vector<float> query_vector, int limit)
    : table_(table), index_(index), query_vector_(std::move(query_vector)), limit_(limit)
{}
