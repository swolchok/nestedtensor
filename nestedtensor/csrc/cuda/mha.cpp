#include <c10/cuda/CUDAStream.h>
#include <nestedtensor/csrc/creation.h>
#include <nestedtensor/csrc/cuda/attention.h>
#include <nestedtensor/csrc/cuda/bert_transformer_op.h>
#include <nestedtensor/csrc/cuda/cuda_kernels.h>
#include <nestedtensor/csrc/masking.h>
#include <nestedtensor/csrc/nested_tensor_impl.h>
#include <nestedtensor/csrc/python_functions.h>
#include <nestedtensor/csrc/utils/nested_node_functions.h>
#include <nestedtensor/csrc/utils/python_nested_node.h>
#include <torch/csrc/Size.h>
#include <torch/csrc/autograd/python_variable_indexing.h>
#include <torch/extension.h>
#include <chrono>
namespace py = pybind11;

using namespace torch::nested_tensor;
using namespace at;

namespace torch {
namespace nested_tensor {

at::Tensor bt_min_mha(
    int64_t num_heads,
    int64_t head_dim,
    double dropout_p,
    bool training,
    at::Tensor input_mask,
    at::Tensor query,
    at::Tensor key,
    at::Tensor value,
    at::Tensor attr_kernel_Q,
    at::Tensor attr_kernel_K,
    at::Tensor attr_kernel_V,
    at::Tensor attr_bias_Q,
    at::Tensor attr_bias_K,
    at::Tensor attr_bias_V,
    double scaling,
    at::Tensor out_proj_weight,
    at::Tensor out_proj_bias,
    at::Tensor attr_mask) {
  // TODO: Assert that max seq_len is 1024!
  TORCH_CHECK(query.dim() == 3, "query needs to be 3 dim.");
  TORCH_CHECK(key.dim() == 3, "key needs to be 3 dim.");
  TORCH_CHECK(value.dim() == 3, "value needs to be 3 dim.");
  // TORCH_CHECK(in_proj_bias, "Input projection bias needs to be defined.");
  // auto opt_sizes = get_opt_sizes(query);
  // if (!opt_sizes[2]) {
  //   throw std::runtime_error("query's third dimension must be regular.");
  // }
  // TODO: Add explicit check that verifies query, key and value are the same
  // auto start = std::chrono::system_clock::now();
  int64_t batch_size = input_mask.size(0);
  int64_t seq_len = input_mask.size(1);
  int64_t embedding_dim = head_dim * num_heads; //*(opt_sizes[2]);
  int64_t head_num = num_heads;
  int64_t size_per_head = embedding_dim / head_num;
  auto float_options =
      torch::TensorOptions().dtype(torch::kFloat).device(torch::kCUDA);
  auto options =
      torch::TensorOptions().dtype(torch::kInt32).device(torch::kCUDA);
  at::cuda::CUDAStream defaultStream = at::cuda::getDefaultCUDAStream();
  at::cuda::setCurrentCUDAStream(defaultStream);

  int64_t input_tensor_size = batch_size * head_num * seq_len * size_per_head;
  int64_t attn_tensor_size = batch_size * head_num * seq_len * seq_len;
  int64_t buf_size = input_tensor_size * 2 + attn_tensor_size;
  at::Tensor buf_tensor = torch::zeros({buf_size}, float_options);
  Tensor tmp_int = torch::zeros(
      {input_mask.size(0) * input_mask.size(1) * 2 + batch_size * seq_len +
       batch_size * seq_len},
      options);

  int* prefix_sum_ptr = tmp_int.data_ptr<int>();
  int* batch_idx_ptr =
      prefix_sum_ptr + input_mask.size(0) * input_mask.size(1) * 2;
  int* word_idx_ptr = batch_idx_ptr + batch_size * seq_len;
  int word_num = batch_size * seq_len;

  at::Tensor tmp = get_buffer(query);

  effectivetransformer::exclusiveScan_kernelLauncher(
      prefix_sum_ptr,
      input_mask.data_ptr<int>(),
      input_mask.size(0) * input_mask.size(1),
      defaultStream);

  effectivetransformer::compressBertInput_kernelLauncher(
      input_mask.data_ptr<int>(),
      prefix_sum_ptr,
      batch_idx_ptr,
      word_idx_ptr,
      (int32_t)(batch_size),
      (int32_t)(seq_len),
      (int32_t)(embedding_dim),
      defaultStream);

  defaultStream.synchronize();

  at::Tensor q, k, v;
  q = at::addmm(
      attr_bias_Q.contiguous(), query, attr_kernel_Q.t().contiguous());
  k = at::addmm(attr_bias_K.contiguous(), key, attr_kernel_K.t().contiguous());
  v = at::addmm(
      attr_bias_V.contiguous(), value, attr_kernel_V.t().contiguous());
  at::Tensor q_buf = get_buffer(q);
  at::Tensor k_buf = get_buffer(k);
  at::Tensor v_buf = get_buffer(v);

  int valid_word_num = tmp_int.reshape({-1})[word_num - 1].item<int>();
  int last_mask = input_mask.reshape({-1})[word_num - 1].item<int>();
  if (last_mask == 1) {
    valid_word_num++;
  }

  float* query_ptr = buf_tensor.data_ptr<float>() + 0 * input_tensor_size;
  float* key_ptr = buf_tensor.data_ptr<float>() + 1 * input_tensor_size;
  float* value_ptr = buf_tensor.data_ptr<float>() + 2 * input_tensor_size;
  effectivetransformer::cuda::add_QKV_bias_padding_kernelLauncher<float>(
      q_buf.data_ptr<float>(),
      attr_bias_Q.data_ptr<float>(),
      k_buf.data_ptr<float>(),
      attr_bias_K.data_ptr<float>(),
      v_buf.data_ptr<float>(),
      attr_bias_V.data_ptr<float>(),
      query_ptr,
      key_ptr,
      value_ptr,
      valid_word_num,
      batch_size,
      seq_len,
      head_num,
      size_per_head,
      batch_idx_ptr,
      word_idx_ptr,
      defaultStream);

  at::Tensor query_buf =
      at::slice(buf_tensor, 0, 0, input_tensor_size)
          .reshape({batch_size, head_num, seq_len, size_per_head});
  at::Tensor key_buf =
      at::slice(buf_tensor, 0, input_tensor_size, 2 * input_tensor_size)
          .reshape({batch_size, head_num, seq_len, size_per_head});
  key_buf = key_buf.transpose(2, 3);
  at::Tensor attn_output_weights = at::matmul(query_buf, key_buf).contiguous();

  effectivetransformer::bt_mha(
      tmp.data_ptr<float>(),
      tmp.data_ptr<float>(),
      attn_output_weights.data_ptr<float>(),
      value_ptr,
      batch_idx_ptr,
      word_idx_ptr,
      attr_mask.data_ptr<float>(),
      batch_size,
      head_num,
      seq_len,
      size_per_head,
      buf_tensor.data_ptr<float>(),
      (float)(scaling),
      prefix_sum_ptr,
      input_mask.data_ptr<int>(),
      valid_word_num);
  at::Tensor attr_out = at::slice(buf_tensor, 0, 0, valid_word_num * embedding_dim);
  attr_out = attr_out.reshape({-1, embedding_dim});
  // TODO: Bias is variably sized, need to add support for that.
  // result = at::addmm(out_proj_bias, attr_out, out_proj_weight.t());
  at::Tensor result = at::matmul(attr_out, out_proj_weight.t());
  result = result.reshape({-1});
  return wrap_buffer(std::move(result), get_nested_size(query));
}

TORCH_LIBRARY_FRAGMENT(nestedtensor, m) {
  m.def(
      "bt_min_mha(int num_heads, int head_dim, float dropout_p, bool training, Tensor input_mask, Tensor query, Tensor key, Tensor value, Tensor attr_kernel_Q, Tensor attr_kernel_K, Tensor attr_kernel_V, Tensor attr_bias_Q, Tensor attr_bias_K, Tensor attr_bias_V, float scaling, Tensor out_proj_weight, Tensor out_proj_bias, Tensor attr_mask) -> Tensor");
  m.impl("bt_min_mha", NestedTensorKey, &bt_min_mha);
}

} // namespace nested_tensor
} // namespace torch