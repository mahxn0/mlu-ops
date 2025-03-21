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
#include "voxel_pooling_forward.h"

#include <string>

#include "core/cnnl_helper.h"
#include "core/context.h"
#include "core/gen_case.h"
#include "core/logging.h"
#include "core/runtime/device.h"
#include "core/tensor.h"
#include "core/type.h"

static void policyFunc(const mluOpHandle_t handle, const int num_points_total,
                       cnrtDim3_t *k_dim, cnrtFunctionType_t *k_type) {
  uint32_t cluster_num = mluop::runtime::getClusterLimitCapability(handle);
  uint32_t core_in_cluster = handle->core_num_per_cluster;
  *k_type = cnrtFuncTypeUnion1;
  k_dim->x = core_in_cluster;
  uint32_t use_cluster =
      (num_points_total + core_in_cluster - 1) / core_in_cluster;
  k_dim->y = use_cluster > cluster_num ? cluster_num : use_cluster;
  k_dim->z = 1;
}

mluOpStatus_t VoxelPoolingForwardParamCheck(
    const std::string &op_name, const mluOpHandle_t handle,
    const int batch_size, const int num_points, const int num_channels,
    const int num_voxel_x, const int num_voxel_y, const int num_voxel_z,
    const mluOpTensorDescriptor_t geom_xyz_desc, const void *geom_xyz,
    const mluOpTensorDescriptor_t input_features_desc,
    const void *input_features,
    const mluOpTensorDescriptor_t output_features_desc,
    const void *output_features, const mluOpTensorDescriptor_t pos_memo_desc,
    const void *pos_memo) {
  // check descriptor and data
  PARAM_CHECK(op_name, handle != NULL);
  PARAM_CHECK(op_name, geom_xyz_desc != NULL);
  PARAM_CHECK(op_name, input_features_desc != NULL);
  PARAM_CHECK(op_name, output_features_desc != NULL);
  PARAM_CHECK(op_name, pos_memo_desc != NULL);
  // check data type
  PARAM_CHECK(op_name, geom_xyz_desc->getDtype() == MLUOP_DTYPE_INT32);
  PARAM_CHECK(op_name, input_features_desc->getDtype() == MLUOP_DTYPE_FLOAT);
  PARAM_CHECK(op_name, output_features_desc->getDtype() == MLUOP_DTYPE_FLOAT);
  PARAM_CHECK(op_name, pos_memo_desc->getDtype() == MLUOP_DTYPE_INT32);
  // check tensor dims and shape
  PARAM_CHECK(op_name, geom_xyz_desc->getDim() == 3);
  PARAM_CHECK(op_name, input_features_desc->getDim() == 3);
  PARAM_CHECK(op_name, output_features_desc->getDim() == 4);
  PARAM_CHECK(op_name, pos_memo_desc->getDim() == 3);
  PARAM_CHECK(op_name, geom_xyz_desc->getDimIndex(0) == batch_size);
  PARAM_CHECK(op_name, geom_xyz_desc->getDimIndex(1) == num_points);
  PARAM_CHECK(op_name, geom_xyz_desc->getDimIndex(2) == 3);
  PARAM_CHECK(op_name, input_features_desc->getDimIndex(0) == batch_size);
  PARAM_CHECK(op_name, input_features_desc->getDimIndex(1) == num_points);
  PARAM_CHECK(op_name, input_features_desc->getDimIndex(2) == num_channels);
  PARAM_CHECK(op_name, output_features_desc->getDimIndex(0) == batch_size);
  PARAM_CHECK(op_name, output_features_desc->getDimIndex(1) == num_voxel_y);
  PARAM_CHECK(op_name, output_features_desc->getDimIndex(2) == num_voxel_x);
  PARAM_CHECK(op_name, output_features_desc->getDimIndex(3) == num_channels);
  PARAM_CHECK(op_name, pos_memo_desc->getDimIndex(0) == batch_size);
  PARAM_CHECK(op_name, pos_memo_desc->getDimIndex(1) == num_points);
  PARAM_CHECK(op_name, pos_memo_desc->getDimIndex(2) == 3);
  // check dim prams
  PARAM_CHECK(op_name, batch_size > 0);
  PARAM_CHECK(op_name, num_points > 0);
  PARAM_CHECK(op_name, num_channels > 0);
  PARAM_CHECK(op_name, num_voxel_x > 0);
  PARAM_CHECK(op_name, num_voxel_y > 0);
  PARAM_CHECK(op_name, num_voxel_z > 0);
  // check stride
  STRIDE_TENSOR_CHECK("[mluOpVoxelPoolingForward]:", geom_xyz_desc,
                      "geom_xyz_desc must be contiguous");
  STRIDE_TENSOR_CHECK("[mluOpVoxelPoolingForward]:", input_features_desc,
                      "input_features_desc must be contiguous");
  STRIDE_TENSOR_CHECK("[mluOpVoxelPoolingForward]:", output_features_desc,
                      "output_features_desc must be contiguous");
  STRIDE_TENSOR_CHECK("[mluOpVoxelPoolingForward]:", pos_memo_desc,
                      "pos_memo_desc must be contiguous");
  // check large tensor
  if ((mluOpGetTensorElementNum(geom_xyz_desc) >= LARGE_TENSOR_NUM) ||
      (mluOpGetTensorElementNum(input_features_desc) >= LARGE_TENSOR_NUM) ||
      (mluOpGetTensorElementNum(output_features_desc) >= LARGE_TENSOR_NUM) ||
      (mluOpGetTensorElementNum(pos_memo_desc) >= LARGE_TENSOR_NUM)) {
    LOG(ERROR) << op_name << " Overflow max tensor num."
               << " Currently, MLU-OPS supports tensor num smaller than 2^31.";
    return MLUOP_STATUS_NOT_SUPPORTED;
  }
  // check ptr
  PARAM_CHECK(op_name, geom_xyz != NULL);
  PARAM_CHECK(op_name, input_features != NULL);
  PARAM_CHECK(op_name, output_features != NULL);
  PARAM_CHECK(op_name, pos_memo != NULL);
  // check arch
  if (handle->arch < MLUOP_MLU370) {
    LOG(ERROR) << op_name
               << " The operator does not match the current architecture.";
    return MLUOP_STATUS_ARCH_MISMATCH;
  }
  return MLUOP_STATUS_SUCCESS;
}

mluOpStatus_t MLUOP_WIN_API mluOpVoxelPoolingForward(
    mluOpHandle_t handle, const int batch_size, const int num_points,
    const int num_channels, const int num_voxel_x, const int num_voxel_y,
    const int num_voxel_z, const mluOpTensorDescriptor_t geom_xyz_desc,
    const void *geom_xyz, const mluOpTensorDescriptor_t input_features_desc,
    const void *input_features,
    const mluOpTensorDescriptor_t output_features_desc, void *output_features,
    const mluOpTensorDescriptor_t pos_memo_desc, void *pos_memo) {
  // check params
  mluOpStatus_t param_check = VoxelPoolingForwardParamCheck(
      "[mluOpVoxelPoolingForward]", handle, batch_size, num_points,
      num_channels, num_voxel_x, num_voxel_y, num_voxel_z, geom_xyz_desc,
      geom_xyz, input_features_desc, input_features, output_features_desc,
      output_features, pos_memo_desc, pos_memo);

  if (param_check != MLUOP_STATUS_SUCCESS) {
    return param_check;
  }

  if (MLUOP_GEN_CASE_ON_NEW) {
    GEN_CASE_START("voxel_pooling_forward", "VOXEL_POOLING_FORWARD");
    GEN_CASE_HANDLE(handle);
    GEN_CASE_DATA_REAL(true, "geom_xyz", geom_xyz, geom_xyz_desc);
    GEN_CASE_DATA_REAL(true, "input_features", input_features,
                       input_features_desc);
    GEN_CASE_DATA(false, "output_features", output_features,
                  output_features_desc, 0, 0);
    GEN_CASE_DATA(false, "pos_memo", pos_memo, pos_memo_desc, 0, 0);
    GEN_CASE_OP_PARAM_SINGLE(0, "voxel_pooling_forward", "batch_size",
                             batch_size);
    GEN_CASE_OP_PARAM_SINGLE(1, "voxel_pooling_forward", "num_points",
                             num_points);
    GEN_CASE_OP_PARAM_SINGLE(2, "voxel_pooling_forward", "num_channels",
                             num_channels);
    GEN_CASE_OP_PARAM_SINGLE(3, "voxel_pooling_forward", "num_voxel_x",
                             num_voxel_x);
    GEN_CASE_OP_PARAM_SINGLE(4, "voxel_pooling_forward", "num_voxel_y",
                             num_voxel_y);
    GEN_CASE_OP_PARAM_SINGLE(5, "voxel_pooling_forward", "num_voxel_z",
                             num_voxel_z);
    GEN_CASE_TEST_PARAM_NEW(true, true, false, 0.003, 0.003, 0);
  }

  const float output_features_init_vaule = 0;
  {
    DEFINE_CREATE_AND_SET_CNNL_HANDLE(handle, cnnl_handle);
    DEFINE_CREATE_AND_SET_CNNL_TENSOR_DESCRIPTOR(output_features_desc,
                                                 cnnl_output_desc);
    CALL_CNNL(cnnlFill_v3(cnnl_handle, CNNL_POINTER_MODE_HOST,
                          &output_features_init_vaule, cnnl_output_desc,
                          output_features));
    DESTROY_CNNL_TENSOR_DESCRIPTOR(cnnl_output_desc);
    DESTROY_CNNL_HANDLE(cnnl_handle);
  }
  const int pos_memo_init_vaule = -1;
  {
    DEFINE_CREATE_AND_SET_CNNL_HANDLE(handle, cnnl_handle);
    DEFINE_CREATE_AND_SET_CNNL_TENSOR_DESCRIPTOR(pos_memo_desc,
                                                 cnnl_output_desc);
    CALL_CNNL(cnnlFill_v3(cnnl_handle, CNNL_POINTER_MODE_HOST,
                          &pos_memo_init_vaule, cnnl_output_desc, pos_memo));
    DESTROY_CNNL_TENSOR_DESCRIPTOR(cnnl_output_desc);
    DESTROY_CNNL_HANDLE(cnnl_handle);
  }

  uint32_t num_points_total = batch_size * num_points;
  cnrtDim3_t k_dim;
  cnrtFunctionType_t k_type;

  policyFunc(handle, num_points_total, &k_dim, &k_type);
  VLOG(5) << "[mluOpVoxelPoolingForward] launch kernel policyFunc[" << k_dim.x
          << ", " << k_dim.y << ", " << k_dim.z << "].";

  CHECK_RETURN("[mluOpVoxelPoolingForward]",
               KernelVoxelPoolingForward(
                   k_dim, k_type, handle->queue, batch_size, num_points,
                   num_channels, num_voxel_x, num_voxel_y, num_voxel_z,
                   geom_xyz, input_features, output_features, pos_memo));
  VLOG(5) << "Launch Kernel KernelVoxelPoolingForward.";
  GEN_CASE_END();
  return MLUOP_STATUS_SUCCESS;
}
