#include <torch/extension.h>
#include <torch_npu/csrc/core/npu/NPUStream.h>
#include "aclnn_swi_glu_ex.h"
#include "aclnn_grouped_mat_mul_ex.h"
#include "aclnn_add_rms_norm_ex.h"
#include "aclnn_reshape_and_cache_ex.h"
#include "aclnn_paged_attention_ex.h"
#include <tuple>

namespace native {

at::Tensor swiglu(at::Tensor x) {
  TORCH_CHECK(x.dim() == 2,
              "swiglu: input tensor must be 2D, got ", x.dim(), "D tensor");
  TORCH_CHECK(x.size(-1) >= 64 && x.size(-1) % 64 == 0,
              "swiglu: last dimension must be a multiple of 64, got ", x.size(-1));
  TORCH_CHECK(x.is_contiguous(),
              "swiglu: input tensor must be contiguous");

  at::ScalarType scalar_type = x.scalar_type();
  auto x_sizes = x.sizes();
  auto x_strides = x.strides();
  std::vector<int64_t> y_sizes(x_sizes.begin(), x_sizes.end());
  y_sizes.back() = y_sizes.back() / 2;
  at::Tensor y = at::empty(y_sizes, x.options());
  auto y_strides = y.strides();

  aclrtStream stream = c10_npu::getCurrentNPUStream().stream();
  aclTensor *x_acl = aclCreateTensor(x_sizes.data(), x.dim(), ACL_FLOAT16, x_strides.data(), 0, ACL_FORMAT_ND, x_sizes.data(), x.dim(), x.data_ptr());
  if (x_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor");
  }
  aclTensor *y_acl = aclCreateTensor(y_sizes.data(), y.dim(), ACL_FLOAT16, y_strides.data(), 0, ACL_FORMAT_ND, y_sizes.data(), y.dim(), y.data_ptr());
  if (y_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor");
  }

  uint64_t workspace_size = 0;
  aclOpExecutor* handle = nullptr;
  if (aclnnSwiGluExGetWorkspaceSize(x_acl, y_acl, &workspace_size, &handle) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to get workspace size");
  }
  auto options = at::TensorOptions().dtype(torch::kUInt8).device(x.device());
  auto workspace_tensor = at::empty({(int64_t)workspace_size}, options);
  if (aclnnSwiGluEx(workspace_tensor.data_ptr(), workspace_size, handle, stream) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to execute swiglu");
  }

  if (aclDestroyTensor(x_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for x");
  }
  if (aclDestroyTensor(y_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for y");
  }
  return y;
}


at::Tensor grouped_matmul(at::Tensor x, at::Tensor w, at::Tensor group_list) {
  TORCH_CHECK(x.dim() == 2 && w.dim() == 3 && group_list.dim() == 1,
              "grouped_matmul: input tensors must be 2D and group_list must be 1D");
  at::ScalarType scalar_type = x.scalar_type();

  int num_tokens = x.size(0);
  int dim = x.size(1);
  int num_exports = w.size(0);
  int inner_dim = w.size(2);

  TORCH_CHECK(dim == w.size(1),
              "grouped_matmul: last dimension of x must match second dimension of w, got ", dim, " and ", w.size(1));
  TORCH_CHECK(x.is_contiguous() && group_list.is_contiguous(),
              "grouped_matmul: x and group_list must be contiguous tensors");
  TORCH_CHECK(w.stride(1) == 1 && w.stride(2) == dim,
              "grouped_matmul: w must be K-major order, got strides ", w.strides());
  TORCH_CHECK(w.size(1) % 64 == 0 && w.size(2) % 64 == 0,
              "grouped_matmul: second and third dimensions of w must be multiples of 64, got ", w.size(1), " and ", w.size(2));

  uint8_t* x_ptr = reinterpret_cast<uint8_t*>(x.data_ptr());
  uint8_t* w_ptr = reinterpret_cast<uint8_t*>(w.data_ptr());
  uint8_t* group_list_ptr = reinterpret_cast<uint8_t*>(group_list.data_ptr());
  at::Tensor y = at::empty({num_tokens, inner_dim}, x.options());
  uint8_t* y_ptr = reinterpret_cast<uint8_t*>(y.data_ptr());

  aclrtStream stream = c10_npu::getCurrentNPUStream().stream();
  auto x_sizes = x.sizes();
  auto x_strides = x.strides();
  aclTensor* x_acl = aclCreateTensor(x_sizes.data(), x.dim(), ACL_FLOAT16, x_strides.data(), 0, ACL_FORMAT_ND, x_sizes.data(), x.dim(), x.data_ptr());
  if (x_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for x");
  }
  // auto w_sizes = w.sizes();
  auto w_strides = w.strides();
  std::vector<int64_t> w_storage_sizes({w.size(0), w.size(2), w.size(1)});
  std::vector<int64_t> w_storage_strides({w_strides[0], w_strides[2], w_strides[1]});
  aclTensor* w_acl = aclCreateTensor(w_storage_sizes.data(), w.dim(), ACL_FLOAT16, w_storage_strides.data(), 0, ACL_FORMAT_ND, w_storage_sizes.data(), w.dim(), w.data_ptr());
  if (w_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for w");
  }
  auto group_list_sizes = group_list.sizes();
  auto group_list_strides = group_list.strides();
  aclTensor* group_list_acl = aclCreateTensor(group_list_sizes.data(), group_list.dim(), ACL_INT64, group_list_strides.data(), 0, ACL_FORMAT_ND, group_list_sizes.data(), group_list.dim(), group_list.data_ptr());
  if (group_list_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for group_list");
  }
  auto y_sizes = y.sizes();
  auto y_strides = y.strides();
  aclTensor* y_acl = aclCreateTensor(y_sizes.data(), y.dim(), ACL_FLOAT16, y_strides.data(), 0, ACL_FORMAT_ND, y_sizes.data(), y.dim(), y.data_ptr());
  if (y_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for y");
  }
  uint64_t workspace_size = 0;
  aclOpExecutor* handle = nullptr;
  if (aclnnGroupedMatMulExGetWorkspaceSize(x_acl, w_acl, group_list_acl, y_acl, &workspace_size, &handle) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to get workspace size");
  }
  auto options = at::TensorOptions().dtype(torch::kUInt8).device(x.device());
  auto workspace_tensor = at::empty({(int64_t)workspace_size}, options);
  if (aclnnGroupedMatMulEx(workspace_tensor.data_ptr(), workspace_size, handle, stream) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to execute grouped_matmul");
  }

  if (aclDestroyTensor(x_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for x");
  }
  if (aclDestroyTensor(w_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for w");
  }
  if (aclDestroyTensor(group_list_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for group_list");
  }
  if (aclDestroyTensor(y_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for y");
  }
  return y;
}


std::tuple<at::Tensor, at::Tensor> add_rms_norm(at::Tensor x, at::Tensor residual, at::Tensor weight, float epsilon) {
  TORCH_CHECK(x.dim() == 2 && residual.dim() == 2 && weight.dim() == 1,
              "add_rms_norm: x and residual must be 2D, weight must be 1D");
  TORCH_CHECK(x.size(1) == weight.size(0),
              "add_rms_norm: last dimension of x must match weight size, got ", x.size(1), " and ", weight.size(0));
  TORCH_CHECK(x.size(0) == residual.size(0) && x.size(1) == residual.size(1),
              "add_rms_norm: x and residual must have the same shape, got ", x.sizes(), " and ", residual.sizes());
  TORCH_CHECK(x.is_contiguous() && residual.is_contiguous() && weight.is_contiguous(),
              "add_rms_norm: all input tensors must be contiguous");
  TORCH_CHECK(x.size(1) % 64 == 0,
              "add_rms_norm: last dimension must be a multiple of 64, got ", x.size(1));

  int num_tokens = x.size(0);
  int dim = x.size(1);
  at::Tensor y = at::empty({num_tokens, dim}, x.options());
  at::Tensor residual_output = at::empty({num_tokens, dim}, x.options());

  aclrtStream stream = c10_npu::getCurrentNPUStream().stream();

  // Create ACL tensors
  auto x_sizes = x.sizes();
  auto x_strides = x.strides();
  aclTensor* x_acl = aclCreateTensor(x_sizes.data(), x.dim(), ACL_FLOAT16, x_strides.data(), 0, ACL_FORMAT_ND, x_sizes.data(), x.dim(), x.data_ptr());
  if (x_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for x");
  }

  auto residual_sizes = residual.sizes();
  auto residual_strides = residual.strides();
  aclTensor* residual_acl = aclCreateTensor(residual_sizes.data(), residual.dim(), ACL_FLOAT16, residual_strides.data(), 0, ACL_FORMAT_ND, residual_sizes.data(), residual.dim(), residual.data_ptr());
  if (residual_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for residual");
  }

  auto weight_sizes = weight.sizes();
  auto weight_strides = weight.strides();
  aclTensor* weight_acl = aclCreateTensor(weight_sizes.data(), weight.dim(), ACL_FLOAT16, weight_strides.data(), 0, ACL_FORMAT_ND, weight_sizes.data(), weight.dim(), weight.data_ptr());
  if (weight_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for weight");
  }

  // Create epsilon tensor (optional parameter)
  at::Tensor epsilon_tensor = at::tensor({epsilon}, at::TensorOptions().dtype(torch::kFloat32).device(x.device()));
  auto epsilon_sizes = epsilon_tensor.sizes();
  auto epsilon_strides = epsilon_tensor.strides();
  aclTensor* epsilon_acl = aclCreateTensor(epsilon_sizes.data(), epsilon_tensor.dim(), ACL_FLOAT, epsilon_strides.data(), 0, ACL_FORMAT_ND, epsilon_sizes.data(), epsilon_tensor.dim(), epsilon_tensor.data_ptr());
  if (epsilon_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for epsilon");
  }

  auto y_sizes = y.sizes();
  auto y_strides = y.strides();
  aclTensor* y_acl = aclCreateTensor(y_sizes.data(), y.dim(), ACL_FLOAT16, y_strides.data(), 0, ACL_FORMAT_ND, y_sizes.data(), y.dim(), y.data_ptr());
  if (y_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for y");
  }

  auto residual_output_sizes = residual_output.sizes();
  auto residual_output_strides = residual_output.strides();
  aclTensor* residual_output_acl = aclCreateTensor(residual_output_sizes.data(), residual_output.dim(), ACL_FLOAT16, residual_output_strides.data(), 0, ACL_FORMAT_ND, residual_output_sizes.data(), residual_output.dim(), residual_output.data_ptr());
  if (residual_output_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for residual_output");
  }

  // Get workspace size and execute
  uint64_t workspace_size = 0;
  aclOpExecutor* handle = nullptr;
  if (aclnnAddRMSNormExGetWorkspaceSize(x_acl, residual_acl, weight_acl, epsilon_acl, y_acl, residual_output_acl, &workspace_size, &handle) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to get workspace size");
  }
  auto options = at::TensorOptions().dtype(torch::kUInt8).device(x.device());
  auto workspace_tensor = at::empty({(int64_t)workspace_size}, options);
  if (aclnnAddRMSNormEx(workspace_tensor.data_ptr(), workspace_size, handle, stream) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to execute add_rms_norm");
  }

  // Clean up ACL tensors
  if (aclDestroyTensor(x_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for x");
  }
  if (aclDestroyTensor(residual_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for residual");
  }
  if (aclDestroyTensor(weight_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for weight");
  }
  if (aclDestroyTensor(epsilon_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for epsilon");
  }
  if (aclDestroyTensor(y_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for y");
  }
  if (aclDestroyTensor(residual_output_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for residual_output");
  }

  return std::make_tuple(y, residual_output);
}


void reshape_and_cache(at::Tensor key, at::Tensor value, at::Tensor key_cache, at::Tensor value_cache, at::Tensor slot_indices) {
  TORCH_CHECK(key.dim() == 3 && key_cache.dim() == 4 && slot_indices.dim() == 1,
              "reshape_and_cache: key must be 3D, key_cache must be 4D, slot_indices must be 1D");
  TORCH_CHECK(key.is_contiguous() && key_cache.is_contiguous() && slot_indices.is_contiguous(),
              "reshape_and_cache: key, key_cache, slot_indices must be contiguous tensors");
  // value/value_cache can be empty tensor
  if (value.numel() > 0) {
      TORCH_CHECK(value.dim() == 3 && value.is_contiguous(), "reshape_and_cache: value must be 3D and contiguous if not None");
  }
  if (value_cache.numel() > 0) {
      TORCH_CHECK(value_cache.dim() == 4 && value_cache.is_contiguous(), "reshape_and_cache: value_cache must be 4D and contiguous if not None");
  }
  aclrtStream stream = c10_npu::getCurrentNPUStream().stream();

  // create ACL tensor
  auto key_sizes = key.sizes();
  auto key_strides = key.strides();
  aclTensor* key_acl = aclCreateTensor(key_sizes.data(), key.dim(), ACL_FLOAT16, key_strides.data(), 0, ACL_FORMAT_ND, key_sizes.data(), key.dim(), key.data_ptr());
  TORCH_CHECK(key_acl != nullptr, "Failed to create ACL tensor for key");

  // value can be empty
  aclTensor* value_acl = nullptr;
  if (value.numel() > 0) {
    auto value_sizes = value.sizes();
    auto value_strides = value.strides();
    value_acl = aclCreateTensor(value_sizes.data(), value.dim(), ACL_FLOAT16, value_strides.data(), 0, ACL_FORMAT_ND, value_sizes.data(), value.dim(), value.data_ptr());
    TORCH_CHECK(value_acl != nullptr, "Failed to create ACL tensor for value");
  }

  auto key_cache_sizes = key_cache.sizes();
  auto key_cache_strides = key_cache.strides();
  aclTensor* key_cache_acl = aclCreateTensor(key_cache_sizes.data(), key_cache.dim(), ACL_FLOAT16, key_cache_strides.data(), 0, ACL_FORMAT_ND, key_cache_sizes.data(), key_cache.dim(), key_cache.data_ptr());
  TORCH_CHECK(key_cache_acl != nullptr, "Failed to create ACL tensor for key_cache");

  // value_cache can be empty
  aclTensor* value_cache_acl = nullptr;
  if (value_cache.numel() > 0) {
    auto value_cache_sizes = value_cache.sizes();
    auto value_cache_strides = value_cache.strides();
    value_cache_acl = aclCreateTensor(value_cache_sizes.data(), value_cache.dim(), ACL_FLOAT16, value_cache_strides.data(), 0, ACL_FORMAT_ND, value_cache_sizes.data(), value_cache.dim(), value_cache.data_ptr());
    TORCH_CHECK(value_cache_acl != nullptr, "Failed to create ACL tensor for value_cache");
  }

  auto slot_indices_sizes = slot_indices.sizes();
  auto slot_indices_strides = slot_indices.strides();
  aclTensor* slot_indices_acl = aclCreateTensor(slot_indices_sizes.data(), slot_indices.dim(), ACL_INT32, slot_indices_strides.data(), 0, ACL_FORMAT_ND, slot_indices_sizes.data(), slot_indices.dim(), slot_indices.data_ptr());
  TORCH_CHECK(slot_indices_acl != nullptr, "Failed to create ACL tensor for slot_indices");

  // get workspace and handle
  uint64_t workspace_size = 0;
  aclOpExecutor* handle = nullptr;
  if (aclnnReshapeAndCacheExGetWorkspaceSize(key_acl, value_acl, key_cache_acl, value_cache_acl, slot_indices_acl, &workspace_size, &handle) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to get workspace size for reshape_and_cache");
  }
  auto options = at::TensorOptions().dtype(torch::kUInt8).device(key.device());
  auto workspace_tensor = at::empty({(int64_t)workspace_size}, options);

  // execute kernel
  if (aclnnReshapeAndCacheEx(workspace_tensor.data_ptr(), workspace_size, handle, stream) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to execute reshape_and_cache");
  }

  // clean up
  aclDestroyTensor(key_acl);
  if (value_acl) aclDestroyTensor(value_acl);
  aclDestroyTensor(key_cache_acl);
  if (value_cache_acl) aclDestroyTensor(value_cache_acl);
  aclDestroyTensor(slot_indices_acl);
  return;
}


at::Tensor paged_attention(at::Tensor q, at::Tensor key_cache, at::Tensor value_cache, at::Tensor block_tables, at::Tensor context_lens) {
  int bs = q.size(0);
  int num_heads = q.size(1);
  int head_dim = q.size(2);
  // kvcache: [num_pages, num_kv_heads * head_dim / 16, page_size, 16]
  int num_pages = key_cache.size(0);
  int num_kv_heads = key_cache.size(1) * 16 / head_dim;
  int page_size = key_cache.size(2);
  printf("bs: %d, num_heads: %d, head_dim: %d, num_pages: %d, num_kv_heads: %d, page_size: %d\n", bs, num_heads, head_dim, num_pages, num_kv_heads, page_size);

  uint8_t* q_ptr = reinterpret_cast<uint8_t*>(q.data_ptr());
  uint8_t* key_cache_ptr = reinterpret_cast<uint8_t*>(key_cache.data_ptr());
  uint8_t* value_cache_ptr = reinterpret_cast<uint8_t*>(value_cache.data_ptr());
  uint8_t* block_tables_ptr = reinterpret_cast<uint8_t*>(block_tables.data_ptr());
  uint8_t* context_lens_ptr = reinterpret_cast<uint8_t*>(context_lens.data_ptr());

  at::Tensor y = at::empty_like(q);

  aclrtStream stream = c10_npu::getCurrentNPUStream().stream();
  aclTensor* q_acl = aclCreateTensor(q.sizes().data(), q.dim(), ACL_FLOAT16, q.strides().data(), 0, ACL_FORMAT_ND, q.sizes().data(), q.dim(), q.data_ptr());
  if (q_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for q");
  }
  aclTensor* key_cache_acl = aclCreateTensor(key_cache.sizes().data(), key_cache.dim(), ACL_FLOAT16, key_cache.strides().data(), 0, ACL_FORMAT_ND, key_cache.sizes().data(), key_cache.dim(), key_cache.data_ptr());
  if (key_cache_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for key_cache");
  }
  aclTensor* value_cache_acl = aclCreateTensor(value_cache.sizes().data(), value_cache.dim(), ACL_FLOAT16, value_cache.strides().data(), 0, ACL_FORMAT_ND, value_cache.sizes().data(), value_cache.dim(), value_cache.data_ptr());
  if (value_cache_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for value_cache");
  }
  aclTensor* block_tables_acl = aclCreateTensor(block_tables.sizes().data(), block_tables.dim(), ACL_INT32, block_tables.strides().data(), 0, ACL_FORMAT_ND, block_tables.sizes().data(), block_tables.dim(), block_tables.data_ptr());
  if (block_tables_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for block_tables");
  }
  aclTensor* context_lens_acl = aclCreateTensor(context_lens.sizes().data(), context_lens.dim(), ACL_INT32, context_lens.strides().data(), 0, ACL_FORMAT_ND, context_lens.sizes().data(), context_lens.dim(), context_lens.data_ptr());
  if (context_lens_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for context_lens");
  }
  aclTensor* y_acl = aclCreateTensor(y.sizes().data(), y.dim(), ACL_FLOAT16, y.strides().data(), 0, ACL_FORMAT_ND, y.sizes().data(), y.dim(), y.data_ptr());
  if (y_acl == nullptr) {
    throw std::runtime_error("Failed to create ACL tensor for y");
  }

  uint64_t workspace_size = 0;
  aclOpExecutor* handle = nullptr;
  if (aclnnPagedAttentionExGetWorkspaceSize(q_acl, key_cache_acl, value_cache_acl, block_tables_acl, context_lens_acl, y_acl, &workspace_size, &handle) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to get workspace size");
  }
  auto options = at::TensorOptions().dtype(torch::kUInt8).device(q.device());
  auto workspace_tensor = at::empty({(int64_t)workspace_size}, options);
  if (aclnnPagedAttentionEx(workspace_tensor.data_ptr(), workspace_size, handle, stream) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to execute paged_attention");
  }

  if (aclDestroyTensor(q_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for q");
  }
  if (aclDestroyTensor(key_cache_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for key_cache");
  }
  if (aclDestroyTensor(value_cache_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for value_cache");
  }
  if (aclDestroyTensor(block_tables_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for block_tables");
  }
  if (aclDestroyTensor(context_lens_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for context_lens");
  }
  if (aclDestroyTensor(y_acl) != ACL_SUCCESS) {
    throw std::runtime_error("Failed to destroy ACL tensor for y");
  }
  return y;
}

void init_ffi_ops(py::module_ &&m) {
  m.def("swiglu", &swiglu, "Swiglu");
  m.def("grouped_matmul", &grouped_matmul, "GroupedMatMul");
  m.def("add_rms_norm", &add_rms_norm, "AddRMSNorm");
  m.def("reshape_and_cache", &reshape_and_cache, "ReshapeAndCache");
  m.def("paged_attention", &paged_attention, "PagedAttention");
}

}
