//  Copyright (c) 2022 Intel Corporation
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.

#include "kernels/layernorm_ba_ref.hpp"

namespace jd {

bool layernorm_ba_ref_kd_t::init() {
  auto tensor_desc = op_desc_.tensor_descs();
  LOG_IF(FATAL, tensor_desc.size() != 3);
  // TODO(zhe1wang): support more data_type.
  auto input_dt = tensor_desc[0].dtype();
  LOG_IF(FATAL, input_dt != data_type::fp32);
  LOG_IF(FATAL, tensor_desc[0].ftype() != format_type::ba);

  auto tensor_shape = tensor_desc[0].shape();
  int col_num = tensor_shape.back();
  // TODO(zhe1wang): support col nums can't divded by 16.
  LOG_IF(FATAL, col_num % 16 != 0)
      << "Transposed LayerNorm currently only supports column number be able to be devided by 16";
  return true;
}

bool layernorm_ba_ref_k_t::execute(const std::vector<const void*>& rt_data) const {
  auto op_desc = derived_kd()->operator_desc();
  auto tensor_desc = op_desc.tensor_descs();
  int row = tensor_desc[0].reduce_rows();
  int col = tensor_desc[0].shape().back();
  auto dst_dt = tensor_desc[1].dtype();

  float* src = reinterpret_cast<float*>(const_cast<void*>(rt_data[0]));
  float* alpha = reinterpret_cast<float*>(const_cast<void*>(rt_data[2]));
  float* beta = reinterpret_cast<float*>(const_cast<void*>(rt_data[3]));

  void* dst_data = const_cast<void*>(rt_data[1]);
  auto dst_fp32 = static_cast<float*>(dst_data);
  auto dst_u8 = static_cast<uint8_t*>(dst_data);
  auto dst_s8 = static_cast<int8_t*>(dst_data);

  std::vector<float> v_mean, v_var;
  for (int i = 0; i < col; i++) {
    // calculate mean.
    float mean = 0;
    for (int j = 0; j < row; j++) mean += src[j * col + i];
    mean /= row;
    v_mean.push_back(mean);
    // calculate var
    float var = 0;
    for (int j = 0; j < row; j++) var += (src[j * col + i] - mean) * (src[j * col + i] - mean);
    var /= row;
    v_var.push_back(var);
    var += 1e-5;
    var = sqrt(var);
    var = 1 / var;
    // calculate layernorm.
    for (int j = 0; j < row; j++) {
      int dst_idx = j * col + i;
      float value = (src[dst_idx] - mean) * var;
      value = alpha[j] * value + beta[j];
      value = apply_postop_list(value, op_desc.apply_postops_list());
      if (dst_dt == data_type::fp32) {
        dst_fp32[dst_idx] = static_cast<float>(value);
      } else if (dst_dt == data_type::s8) {
        dst_s8[dst_idx] = static_cast<int8_t>(value);
      } else if (dst_dt == data_type::u8) {
        dst_u8[dst_idx] = static_cast<uint8_t>(value);
      }
    }
  }
  return true;
}

}  // namespace jd
