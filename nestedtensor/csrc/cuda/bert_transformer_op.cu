/*
 * Copyright (C) 2020 ByteDance Inc
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <nestedtensor/csrc/cuda/bert_transformer_op.h>
namespace effectivetransformer {

template <typename DataType_>
void bt_mha(
    DataType_* from_tensor,
    DataType_* to_tensor,
    DataType_* qk_buf_,
    DataType_* value_,
    int* batch_idx,
    int* word_idx,
    DataType_* attr_mask,
    int64_t batch_size_,
    int64_t head_num_,
    int64_t seq_len_,
    int64_t size_per_head_,
    DataType_* buf,
    DataType_ scaler,
    int* prefix_sum_ptr,
    int* input_mask_ptr,
    int valid_word_num) {
  at::cuda::CUDAStream stream = at::cuda::getDefaultCUDAStream();
  at::cuda::setCurrentCUDAStream(stream);
  cublasHandle_t cublas_handle = at::cuda::getCurrentCUDABlasHandle();
  check_cuda_error(cublasSetStream(cublas_handle, stream));

  /// 1. Set compute type
  cudaDataType_t computeType, AType, BType, CType;
  int cublasAlgo[3];
  if constexpr (std::is_same<DataType_, float>::value) {
    computeType = CUDA_R_32F;
    AType = CUDA_R_32F;
    BType = CUDA_R_32F;
    CType = CUDA_R_32F;
    cublasAlgo[0] = -1;
    cublasAlgo[1] = -1;
    cublasAlgo[2] = -1;
  } else {
    computeType = CUDA_R_16F;
    AType = CUDA_R_16F;
    BType = CUDA_R_16F;
    CType = CUDA_R_16F;
    cublasAlgo[0] = 99;
    cublasAlgo[1] = 99;
    cublasAlgo[2] = 99;
  }
  DataType_ alpha = (DataType_)1.0f, beta = (DataType_)0.0f;

  /// 2. allocate buffer for transformer
  int batch_size = batch_size_;
  int head_num = head_num_;
  int from_seq_len = seq_len_;
  int size_per_head = size_per_head_;
  int input_tensor_size = batch_size * head_num * from_seq_len * size_per_head;
  int attn_tensor_size = batch_size * head_num * from_seq_len * from_seq_len;

   DataType_* attr_out_buf_     = buf + 0 * input_tensor_size;
   DataType_* transpose_dst_    = buf + 1 * input_tensor_size;

  auto float_options =
      torch::TensorOptions().dtype(torch::kFloat).device(torch::kCUDA);

  {
     cuda::softmax_kernel_kernelLauncher<DataType_>(
         qk_buf_, attr_mask, batch_size, head_num, from_seq_len, scaler, stream);

    check_cuda_error(cublasGemmStridedBatchedEx(
        cublas_handle,
        CUBLAS_OP_N,
        CUBLAS_OP_N,
        size_per_head,
        from_seq_len,
        from_seq_len,
        &alpha,
        value_,
        AType,
        size_per_head,
        from_seq_len * size_per_head,
        qk_buf_,
        BType,
        from_seq_len,
        from_seq_len * from_seq_len,
        &beta,
        transpose_dst_,
        CType,
        size_per_head,
        from_seq_len * size_per_head,
        batch_size * head_num,
        computeType,
        static_cast<cublasGemmAlgo_t>(cublasAlgo[2])));

    cuda::transpose_rm_padding_kernelLauncher<DataType_>(
        transpose_dst_,
        attr_out_buf_,
        valid_word_num,
        batch_size,
        from_seq_len,
        head_num,
        size_per_head,
        batch_idx,
        word_idx,
        stream);
  }
};

template void bt_mha<float>(
    float* from_tensor,
    float* to_tensor,
    float* qk_buf_,
    float* value_,
    int* batch_idx,
    int* word_idx,
    float* attr_mask,
    int64_t batch_size_,
    int64_t head_num_,
    int64_t seq_len_,
    int64_t size_per_head_,
    float* buf,
    float scaler,
    int* prefix_sum_ptr,
    int* input_mask_ptr,
    int valid_word_num);
} // namespace effectivetransformer