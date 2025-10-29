/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include <stdint.h>
#include "common/math/simd_util.h"

#if defined(USE_SIMD)

int mm256_extract_epi32_var_indx(const __m256i vec, const unsigned int i)
{
  __m128i idx = _mm_cvtsi32_si128(i);
  __m256i val = _mm256_permutevar8x32_epi32(vec, _mm256_castsi128_si256(idx));
  return _mm_cvtsi128_si32(_mm256_castsi256_si128(val));
}

int mm256_sum_epi32(const int *values, int size)
{
  if (values == nullptr || size <= 0) {
    return 0;
  }

  __m256i vec_sum = _mm256_setzero_si256();
  int     i       = 0;

  const int simd_step = SIMD_WIDTH;
  for (; i + simd_step <= size; i += simd_step) {
    __m256i vec = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(values + i));
    vec_sum     = _mm256_add_epi32(vec_sum, vec);
  }

  __m128i low128  = _mm256_castsi256_si128(vec_sum);
  __m128i high128 = _mm256_extracti128_si256(vec_sum, 1);
  __m128i sum128  = _mm_add_epi32(low128, high128);
  sum128          = _mm_hadd_epi32(sum128, sum128);
  sum128          = _mm_hadd_epi32(sum128, sum128);
  int sum         = _mm_cvtsi128_si32(sum128);

  for (; i < size; ++i) {
    sum += values[i];
  }
  return sum;
}

float mm256_sum_ps(const float *values, int size)
{
  if (values == nullptr || size <= 0) {
    return 0.0f;
  }

  __m256 vec_sum = _mm256_setzero_ps();
  int    i       = 0;

  const int simd_step = SIMD_WIDTH;
  for (; i + simd_step <= size; i += simd_step) {
    __m256 vec = _mm256_loadu_ps(values + i);
    vec_sum    = _mm256_add_ps(vec_sum, vec);
  }

  __m128 low128  = _mm256_castps256_ps128(vec_sum);
  __m128 high128 = _mm256_extractf128_ps(vec_sum, 1);
  __m128 sum128  = _mm_add_ps(low128, high128);
  sum128         = _mm_hadd_ps(sum128, sum128);
  sum128         = _mm_hadd_ps(sum128, sum128);
  float sum      = _mm_cvtss_f32(sum128);

  for (; i < size; ++i) {
    sum += values[i];
  }
  return sum;
}

template <typename V>
void selective_load(V *memory, int offset, V *vec, __m256i &inv)
{
  int *inv_ptr = reinterpret_cast<int *>(&inv);
  for (int i = 0; i < SIMD_WIDTH; i++) {
    if (inv_ptr[i] == -1) {
      vec[i] = memory[offset++];
    }
  }
}
template void selective_load<uint32_t>(uint32_t *memory, int offset, uint32_t *vec, __m256i &inv);
template void selective_load<int>(int *memory, int offset, int *vec, __m256i &inv);
template void selective_load<float>(float *memory, int offset, float *vec, __m256i &inv);

#endif
