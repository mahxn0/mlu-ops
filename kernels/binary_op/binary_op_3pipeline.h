/*************************************************************************
 * Copyright (C) [2022] by Cambricon, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *************************************************************************/
#ifndef KERNELS_BINARY_OP_BINARY_OP_3PIPELINE_H_
#define KERNELS_BINARY_OP_BINARY_OP_3PIPELINE_H_

#include "kernels/kernel.h"
#include "kernels/debug.h"
#include "kernels/utils/common.h"

#define BINARY_ALIGN_NUM 64
#define BINARY_NRAM_SIZE (MAX_NRAM_SIZE + REM_FOR_STACK - 112 * 1024)
#define BINARY_SRAM_SIZE (CORE_DIM * BINARY_NRAM_SIZE)

#define BINARY_OP_KERNEL_3PIPELINE_DECLARE(Op, Prefer)                  \
  template <typename DType_in1, typename DType_in2, typename DType_out, \
            typename... Args>                                           \
  __mlu_global__ void MLUBlockKernel3StagePipeline##Op##Prefer(         \
      char *x, char *y, char *z, size_t element_num, Args... args);

#define BINARY_OP_KERNEL_3PIPELINE(Op, Prefer)                                \
  template <typename DType_in1, typename DType_in2, typename DType_out,       \
            typename... Args>                                                 \
  __mlu_global__ void MLUBlockKernel3StagePipeline##Op##Prefer(               \
      char *input1_gdram, char *input2_gdram, char *output_gdram,             \
      size_t element_num, Args... args) {                                     \
    if (__is_mpu()) {                                                         \
      return;                                                                 \
    }                                                                         \
    size_t output_input1_gap = 0, output_input2_gap = 0, ping_pong_gap = 0;   \
    size_t auxiliary_a_gap = 0, auxiliary_b_gap = 0, auxiliary_c_gap = 0;     \
    size_t span_num_deal = 0;                                                 \
    size_t align_num =                                                        \
        (sizeof(DType_out) == 1) ? NFU_ALIGN_SIZE : BINARY_ALIGN_NUM;         \
    auxFunc3##Op##Prefer<DType_in1, DType_in2, DType_out>(                    \
        output_input1_gap, output_input2_gap, ping_pong_gap, auxiliary_a_gap, \
        auxiliary_b_gap, auxiliary_c_gap, span_num_deal, align_num, args...); \
    const size_t num_rem = element_num % taskDim;                             \
    size_t num_per_core = element_num / taskDim;                              \
    const char *const input1_start =                                          \
        input1_gdram + taskId * num_per_core * sizeof(DType_in1);             \
    const char *const input2_start =                                          \
        input2_gdram + taskId * num_per_core * sizeof(DType_in2);             \
    char *const output_start =                                                \
        output_gdram + taskId * num_per_core * sizeof(DType_out);             \
    if (num_rem > 0 && taskId == taskDim - 1) {                               \
      num_per_core = num_per_core + num_rem;                                  \
    }                                                                         \
                                                                              \
    const int32_t repeat = num_per_core / span_num_deal;                      \
    const size_t rem = num_per_core % span_num_deal;                          \
    const size_t align_rem = CEIL_ALIGN(rem, align_num);                      \
    char *ping_output = nram_buffer;                                          \
    char *ping_input1 = nram_buffer + output_input1_gap;                      \
    char *ping_input2 = nram_buffer + output_input2_gap;                      \
    char *auxiliary_a = nram_buffer + auxiliary_a_gap;                        \
    char *auxiliary_b = nram_buffer + auxiliary_b_gap;                        \
    char *auxiliary_c = nram_buffer + auxiliary_c_gap;                        \
    const size_t span_load_input1_size = span_num_deal * sizeof(DType_in1);   \
    const size_t span_load_input2_size = span_num_deal * sizeof(DType_in2);   \
    const size_t span_store_size = span_num_deal * sizeof(DType_out);         \
                                                                              \
    if (repeat > 0) {                                                         \
      __memcpy_async(ping_input1, input1_start, span_load_input1_size,        \
                     GDRAM2NRAM);                                             \
      __memcpy_async(ping_input2, input2_start, span_load_input2_size,        \
                     GDRAM2NRAM);                                             \
      __asm__ volatile("sync;");                                              \
    }                                                                         \
    if (repeat > 1) {                                                         \
      __memcpy_async(ping_input1 + ping_pong_gap,                             \
                     input1_start + span_load_input1_size,                    \
                     span_load_input1_size, GDRAM2NRAM);                      \
      __memcpy_async(ping_input2 + ping_pong_gap,                             \
                     input2_start + span_load_input2_size,                    \
                     span_load_input2_size, GDRAM2NRAM);                      \
      compute##Op##Prefer<DType_in1, DType_in2, DType_out>(                   \
          ping_output, ping_input1, ping_input2, auxiliary_a, auxiliary_b,    \
          auxiliary_c, span_num_deal, span_num_deal, args...);                \
      __asm__ volatile("sync;");                                              \
    }                                                                         \
    for (int32_t i = 0; i < repeat - 2; ++i) {                                \
      __memcpy_async(output_start + i * span_store_size,                      \
                     ping_output + (i % 2) * ping_pong_gap, span_store_size,  \
                     NRAM2GDRAM);                                             \
      __memcpy_async(ping_input1 + (i % 2) * ping_pong_gap,                   \
                     input1_start + (i + 2) * span_load_input1_size,          \
                     span_load_input1_size, GDRAM2NRAM);                      \
      __memcpy_async(ping_input2 + (i % 2) * ping_pong_gap,                   \
                     input2_start + (i + 2) * span_load_input2_size,          \
                     span_load_input2_size, GDRAM2NRAM);                      \
      compute##Op##Prefer<DType_in1, DType_in2, DType_out>(                   \
          ping_output + ((i + 1) % 2) * ping_pong_gap,                        \
          ping_input1 + ((i + 1) % 2) * ping_pong_gap,                        \
          ping_input2 + ((i + 1) % 2) * ping_pong_gap, auxiliary_a,           \
          auxiliary_b, auxiliary_c, span_num_deal, span_num_deal, args...);   \
      __asm__ volatile("sync;");                                              \
    }                                                                         \
    if (repeat > 1) {                                                         \
      __memcpy_async(output_start + (repeat - 2) * span_store_size,           \
                     ping_output + ((repeat - 2) % 2) * ping_pong_gap,        \
                     span_store_size, NRAM2GDRAM);                            \
    }                                                                         \
    if (rem > 0) {                                                            \
      __memcpy_async(ping_input1 + (repeat % 2) * ping_pong_gap,              \
                     input1_start + repeat * span_load_input1_size,           \
                     rem * sizeof(DType_in1), GDRAM2NRAM);                    \
      __memcpy_async(ping_input2 + (repeat % 2) * ping_pong_gap,              \
                     input2_start + repeat * span_load_input2_size,           \
                     rem * sizeof(DType_in2), GDRAM2NRAM);                    \
    }                                                                         \
    if (repeat > 0) {                                                         \
      compute##Op##Prefer<DType_in1, DType_in2, DType_out>(                   \
          ping_output + ((repeat - 1) % 2) * ping_pong_gap,                   \
          ping_input1 + ((repeat - 1) % 2) * ping_pong_gap,                   \
          ping_input2 + ((repeat - 1) % 2) * ping_pong_gap, auxiliary_a,      \
          auxiliary_b, auxiliary_c, span_num_deal, span_num_deal, args...);   \
    }                                                                         \
    __asm__ volatile("sync;");                                                \
    if (repeat > 0) {                                                         \
      __memcpy_async(output_start + (repeat - 1) * span_store_size,           \
                     ping_output + ((repeat - 1) % 2) * ping_pong_gap,        \
                     span_store_size, NRAM2GDRAM);                            \
    }                                                                         \
    if (rem > 0) {                                                            \
      compute##Op##Prefer<DType_in1, DType_in2, DType_out>(                   \
          ping_output + (repeat % 2) * ping_pong_gap,                         \
          ping_input1 + (repeat % 2) * ping_pong_gap,                         \
          ping_input2 + (repeat % 2) * ping_pong_gap, auxiliary_a,            \
          auxiliary_b, auxiliary_c, align_rem, rem, args...);                 \
      __asm__ volatile("sync;");                                              \
      __memcpy_async(output_start + repeat * span_store_size,                 \
                     ping_output + (repeat % 2) * ping_pong_gap,              \
                     rem * sizeof(DType_out), NRAM2GDRAM);                    \
    }                                                                         \
  }

// Divide tasks in host
#define BINARY_OP_KERNEL_3PIPELINE_V2_DECLARE(Op, Prefer)               \
  template <typename DType_in1, typename DType_in2, typename DType_out, \
            typename... Args>                                           \
  __mlu_global__ void MLUBlockKernel3StagePipelineV2##Op##Prefer(       \
      char *x, char *y, char *z, size_t normal_core_elem_num,           \
      size_t tail_core_elem_num, Args... args);

#define BINARY_OP_KERNEL_3PIPELINE_V2(Op, Prefer)                             \
  template <typename DType_in1, typename DType_in2, typename DType_out,       \
            typename... Args>                                                 \
  __mlu_global__ void MLUBlockKernel3StagePipelineV2##Op##Prefer(             \
      char *input1_gdram, char *input2_gdram, char *output_gdram,             \
      size_t normal_core_elem_num, size_t tail_core_elem_num, Args... args) { \
    const char *const input1_start =                                          \
        input1_gdram + taskId * normal_core_elem_num * sizeof(DType_in1);     \
    const char *const input2_start =                                          \
        input2_gdram + taskId * normal_core_elem_num * sizeof(DType_in2);     \
    char *const output_start =                                                \
        output_gdram + taskId * normal_core_elem_num * sizeof(DType_out);     \
    const size_t num_cur_core =                                               \
        (taskId + 1 == taskDim) ? tail_core_elem_num : normal_core_elem_num;  \
                                                                              \
    size_t output_input1_gap = 0, output_input2_gap = 0, ping_pong_gap = 0;   \
    size_t auxiliary_a_gap = 0, auxiliary_b_gap = 0, auxiliary_c_gap = 0;     \
    size_t span_num_deal = 0;                                                 \
    size_t align_num =                                                        \
        (sizeof(DType_out) == 1) ? NFU_ALIGN_SIZE : BINARY_ALIGN_NUM;         \
    auxFunc3##Op##Prefer<DType_in1, DType_in2, DType_out>(                    \
        output_input1_gap, output_input2_gap, ping_pong_gap, auxiliary_a_gap, \
        auxiliary_b_gap, auxiliary_c_gap, span_num_deal, align_num, args...); \
                                                                              \
    const uint32_t repeat = num_cur_core / span_num_deal;                     \
    const size_t rem = num_cur_core % span_num_deal;                          \
    const size_t align_rem = CEIL_ALIGN(rem, align_num);                      \
    char *ping_output = nram_buffer;                                          \
    char *ping_input1 = nram_buffer + output_input1_gap;                      \
    char *ping_input2 = nram_buffer + output_input2_gap;                      \
    char *auxiliary_a = nram_buffer + auxiliary_a_gap;                        \
    char *auxiliary_b = nram_buffer + auxiliary_b_gap;                        \
    char *auxiliary_c = nram_buffer + auxiliary_c_gap;                        \
    const size_t span_load_input1_size = span_num_deal * sizeof(DType_in1);   \
    const size_t span_load_input2_size = span_num_deal * sizeof(DType_in2);   \
    const size_t span_store_size = span_num_deal * sizeof(DType_out);         \
                                                                              \
    if (repeat > 0) {                                                         \
      __memcpy_async(ping_input1, input1_start, span_load_input1_size,        \
                     GDRAM2NRAM);                                             \
      __memcpy_async(ping_input2, input2_start, span_load_input2_size,        \
                     GDRAM2NRAM);                                             \
      __asm__ volatile("sync;");                                              \
    }                                                                         \
    if (repeat > 1) {                                                         \
      __memcpy_async(ping_input1 + ping_pong_gap,                             \
                     input1_start + span_load_input1_size,                    \
                     span_load_input1_size, GDRAM2NRAM);                      \
      __memcpy_async(ping_input2 + ping_pong_gap,                             \
                     input2_start + span_load_input2_size,                    \
                     span_load_input2_size, GDRAM2NRAM);                      \
      compute##Op##Prefer<DType_in1, DType_in2, DType_out>(                   \
          ping_output, ping_input1, ping_input2, auxiliary_a, auxiliary_b,    \
          auxiliary_c, span_num_deal, span_num_deal, args...);                \
      __asm__ volatile("sync;");                                              \
    }                                                                         \
    const uint32_t lcs_loop_num = repeat >= 2 ? (repeat - 2) : 0;             \
    uint32_t ith_buffer_offset = 0;                                           \
    uint32_t i_plus_th_buffer_offset = ith_buffer_offset ^ ping_pong_gap;     \
    for (uint32_t i = 0; i < lcs_loop_num; ++i) {                             \
      __memcpy_async(output_start + i * span_store_size,                      \
                     ping_output + ith_buffer_offset, span_store_size,        \
                     NRAM2GDRAM);                                             \
      __memcpy_async(ping_input1 + ith_buffer_offset,                         \
                     input1_start + (i + 2) * span_load_input1_size,          \
                     span_load_input1_size, GDRAM2NRAM);                      \
      __memcpy_async(ping_input2 + ith_buffer_offset,                         \
                     input2_start + (i + 2) * span_load_input2_size,          \
                     span_load_input2_size, GDRAM2NRAM);                      \
      compute##Op##Prefer<DType_in1, DType_in2, DType_out>(                   \
          ping_output + i_plus_th_buffer_offset,                              \
          ping_input1 + i_plus_th_buffer_offset,                              \
          ping_input2 + i_plus_th_buffer_offset, auxiliary_a, auxiliary_b,    \
          auxiliary_c, span_num_deal, span_num_deal, args...);                \
      ith_buffer_offset = ith_buffer_offset ^ ping_pong_gap;                  \
      i_plus_th_buffer_offset = ith_buffer_offset ^ ping_pong_gap;            \
      __asm__ volatile("sync;");                                              \
    }                                                                         \
    if (repeat > 1) {                                                         \
      const uint32_t lcs_loop_num_th_buffer_offset = ith_buffer_offset;       \
      __memcpy_async(output_start + lcs_loop_num * span_store_size,           \
                     ping_output + lcs_loop_num_th_buffer_offset,             \
                     span_store_size, NRAM2GDRAM);                            \
    }                                                                         \
    const uint32_t repeat_th_buffer_offset =                                  \
        (repeat == 1) ? i_plus_th_buffer_offset : ith_buffer_offset;          \
    if (rem > 0) {                                                            \
      __memcpy_async(ping_input1 + repeat_th_buffer_offset,                   \
                     input1_start + repeat * span_load_input1_size,           \
                     rem * sizeof(DType_in1), GDRAM2NRAM);                    \
      __memcpy_async(ping_input2 + repeat_th_buffer_offset,                   \
                     input2_start + repeat * span_load_input2_size,           \
                     rem * sizeof(DType_in2), GDRAM2NRAM);                    \
    }                                                                         \
    /* if repeat ==1, repeat-1 == i  */                                       \
    const uint32_t repeat_minus_1_th_buffer_offset =                          \
        (repeat == 1) ? ith_buffer_offset : i_plus_th_buffer_offset;          \
    if (repeat > 0) {                                                         \
      compute##Op##Prefer<DType_in1, DType_in2, DType_out>(                   \
          ping_output + repeat_minus_1_th_buffer_offset,                      \
          ping_input1 + repeat_minus_1_th_buffer_offset,                      \
          ping_input2 + repeat_minus_1_th_buffer_offset, auxiliary_a,         \
          auxiliary_b, auxiliary_c, span_num_deal, span_num_deal, args...);   \
    }                                                                         \
    __asm__ volatile("sync;");                                                \
    if (repeat > 0) {                                                         \
      __memcpy_async(output_start + (repeat - 1) * span_store_size,           \
                     ping_output + repeat_minus_1_th_buffer_offset,           \
                     span_store_size, NRAM2GDRAM);                            \
    }                                                                         \
    if (rem > 0) {                                                            \
      compute##Op##Prefer<DType_in1, DType_in2, DType_out>(                   \
          ping_output + repeat_th_buffer_offset,                              \
          ping_input1 + repeat_th_buffer_offset,                              \
          ping_input2 + repeat_th_buffer_offset, auxiliary_a, auxiliary_b,    \
          auxiliary_c, align_rem, rem, args...);                              \
      __asm__ volatile("sync;");                                              \
      __memcpy_async(output_start + repeat * span_store_size,                 \
                     ping_output + repeat_th_buffer_offset,                   \
                     rem * sizeof(DType_out), NRAM2GDRAM);                    \
    }                                                                         \
  }

#endif  // KERNELS_BINARY_OP_BINARY_OP_3PIPELINE_H_
