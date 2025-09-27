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

#include <cstdint>

// 记录内用于引用 LOB 文件中实际数据的位置与长度。
// 说明：offset 为在 .lob 文件中的起始偏移，length 为数据长度（字节）。
// 该结构仅用于 TEXT 字段的行内占位，固定长度，避免单行受文本长度影响而超页。
struct LobRef
{
  int64_t offset = 0;  // 起始偏移
  int32_t length = 0;  // 数据长度
};

