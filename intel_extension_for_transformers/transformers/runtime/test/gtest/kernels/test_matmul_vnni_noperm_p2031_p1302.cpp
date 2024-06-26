//  Copyright (c) 2021 Intel Corporation
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

#include <omp.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <numeric>
#include <exception>
#include "interface.hpp"
#include "gtest/gtest.h"
#include "unit_test_utils.hpp"
#include "kernels/matmul_types.hpp"

#define OMP_NUM_THREADS "OMP_NUM_THREADS"

namespace test {
using io = jd::ssd::matmul_io::io;

struct op_args_t {
  jd::operator_desc op_desc;
  std::vector<const void*> rt_data;
  int nthr;  // 0 for not touching OMP_NUM_THREADS and using what set outside
};

struct test_params_t {
  std::pair<op_args_t, op_args_t> args;
  bool expect_to_fail;
};

void get_true_data(const jd::operator_desc& op_desc, const std::vector<const void*>& rt_data) {
  // configure alias
  const auto shapes = op_desc.tensor_shapes();
  const auto dtypes = op_desc.tensor_dtypes();
  auto attrs = op_desc.attrs();

  const dim_t M = shapes[io::SRC0][2];  // aka src0_perm_shape[2]
  const dim_t K = shapes[io::SRC0][3];  // aka src0_perm_shape[3]
  const dim_t N = shapes[io::SRC1][1];  // aka src1_perm_shape[3]
  const dim_t bs0 = shapes[io::SRC0][0];
  const dim_t bs1 = shapes[io::SRC0][1];

  // scale * src0 x src1 = dst.

  const auto& left_dt = dtypes[io::SRC0];
  const auto& right_dt = dtypes[io::SRC1];
  const auto& dst_dt = dtypes[io::DST0];

  /**
   *   src0: bs0 bs1 m   k ===========> bs0 bs1 m k
   *   src1: bs1 n   bs0 k ==perm2031=> bs0 bs1 k n
   *   dst:  bs1 n   bs0 m <=perm1302== bs0 bs1 m n
   */
  std::vector<dim_t> left_stride = {bs1 * M * K, M * K, K, 1};
  std::vector<dim_t> right_stride = {N * bs0 * K, bs0 * K, K, 1};
  std::vector<dim_t> dst_stride = {N * bs0 * M, bs0 * M, M, 1};

  // runtime data alias
  const auto left_data = rt_data[io::SRC0];
  const auto right_data = rt_data[io::SRC1];
  auto dst_data = const_cast<void*>(rt_data[io::DST0]);
  const auto scale_data = rt_data[io::SCALE0];
  const auto zp_data = rt_data[io::ZP0];

  // ptr alias
  auto left_u8 = static_cast<const uint8_t*>(left_data);
  auto right_s8 = static_cast<const int8_t*>(right_data);
  auto dst_u8 = static_cast<uint8_t*>(dst_data);
  auto scale_f32 = static_cast<const float*>(scale_data);
  auto zp_f32 = static_cast<const float*>(zp_data);
  const auto scale_value = scale_f32[0];
  const auto zp_value = zp_f32[0];

  // Computing the kernel
#pragma omp parallel for collapse(4)
  for (dim_t ibs0 = 0; ibs0 < bs0; ++ibs0)
    for (dim_t ibs1 = 0; ibs1 < bs1; ++ibs1)
      for (dim_t i = 0; i < M; ++i)
        for (dim_t j = 0; j < N; ++j) {
          float value = 0;
          dim_t dst_idx = ibs1 * dst_stride[0] + j * dst_stride[1] + ibs0 * dst_stride[2] + i * dst_stride[3];
#pragma omp simd
          for (dim_t k = 0; k < K; ++k) {
            dim_t l_idx = ibs0 * left_stride[0] + ibs1 * left_stride[1] + i * left_stride[2] + k * left_stride[3];
            dim_t r_idx = ibs1 * right_stride[0] + j * right_stride[1] + ibs0 * right_stride[2] + k * right_stride[3];
            auto l_value = left_dt == jd::data_type::u8 ? left_u8[l_idx] : 0.f;
            auto r_value = right_dt == jd::data_type::s8 ? right_s8[r_idx] : 0.f;
            value += l_value * r_value;
          }

          // Quantize dst data
          if (dst_dt == jd::data_type::u8) {
            jd::postop_attr quantize{
                jd::data_type::u8,
                jd::postop_type::eltwise,
                jd::postop_alg::quantize,
                zp_value,         // zp
                0,                // beta
                1 / scale_value,  // scale
            };
            float quantized_value = apply_postop_list(value, {quantize});
            dst_u8[dst_idx] = static_cast<uint8_t>(quantized_value);
          } else {
            LOG(FATAL) << "unsupported dst type";
          }
        }
}

bool check_result(const test_params_t& t) {
  const auto& p = t.args.first;
  const auto& q = t.args.second;
  get_true_data(q.op_desc, q.rt_data);
  try {
    n_thread_t with_n_thread(p.nthr);
    const auto& op_desc = p.op_desc;
    jd::transpose_matmul_desc kernel_desc(op_desc);
    jd::transpose_matmul kernel(kernel_desc);
    kernel.execute(p.rt_data);
  } catch (const std::exception& e) {
    if (t.expect_to_fail) {
      return true;
    } else {
      return false;
    }
  }
  if (!t.expect_to_fail) {
    auto buf1 = p.rt_data[io::DST0];
    auto size1 = p.op_desc.tensor_descs()[io::DST0].size();
    auto buf2 = q.rt_data[io::DST0];
    auto size2 = q.op_desc.tensor_descs()[io::DST0].size();
    // Should compare buffer with different addresses
    EXPECT_NE(buf1, buf2);
    const auto& dst_type = p.op_desc.tensor_descs()[io::DST0].dtype();
    if (dst_type == jd::data_type::u8) {
      return compare_data<uint8_t>(buf1, size1, buf2, size2, 0);  // no tolerance for quantized value
    } else {
      LOG(FATAL) << "unsupported dst type";
    }
  }
  return false;
}

class MMVNNINopermP2031P1302KernelTest : public testing::TestWithParam<test_params_t> {
 protected:
  MMVNNINopermP2031P1302KernelTest() {}
  virtual ~MMVNNINopermP2031P1302KernelTest() {}
  void SetUp() override {}
  void TearDown() override {}
};

TEST_P(MMVNNINopermP2031P1302KernelTest, ) {
  test_params_t t = testing::TestWithParam<test_params_t>::GetParam();
  EXPECT_TRUE(check_result(t));
  for (auto op_args : {t.args.first, t.args.second})
    for (auto rt_data : op_args.rt_data) {
      char* data = reinterpret_cast<char*>(const_cast<void*>(rt_data));
      delete[] data;
    }
}
std::pair<op_args_t, op_args_t> gen_case(dim_t M, dim_t K, dim_t N, dim_t bs0, dim_t bs1, int nthr = 0,
                                         std::unordered_map<std::string, std::string> attrs = {}) {
  /**
   * Step 1: Construct operator config
   *
   * Dimension details:
   *   src0: bs0 bs1 m   k ===========> bs0 bs1 m k
   *   src1: bs1 n   bs0 k ==perm2031=> bs0 bs1 k n
   *   dst:  bs1 n   bs0 m <=perm1302== bs0 bs1 m n
   */
  jd::tensor_desc src0_desc = {{bs0, bs1, M, K}, jd::data_type::u8, jd::format_type::ab};
  jd::tensor_desc src1_desc = {{bs1, N, bs0, K}, jd::data_type::s8, jd::format_type::ab};
  jd::tensor_desc dst_desc = {{bs1, N, bs0, M}, jd::data_type::u8, jd::format_type::ab};
  jd::tensor_desc src2_desc = {{}, jd::data_type::fp32, jd::format_type::ab};  // binary postop not supported
  jd::tensor_desc scale_desc = {{1}, jd::data_type::fp32, jd::format_type::a};
  jd::tensor_desc zp_desc = {{1}, jd::data_type::fp32, jd::format_type::a};
  std::vector<jd::tensor_desc> ts_descs = {src0_desc, src1_desc, dst_desc, src2_desc, scale_desc, zp_desc};

  // Step 2: Construct runtime data
  std::vector<const void*> rt_data1;
  std::vector<const void*> rt_data2;
  int tensor_num = ts_descs.size();
  for (int index = 0; index < tensor_num; ++index) {
    if (index == io::SRC2) {
      // insert nullptr as placeholder
      rt_data1.emplace_back(nullptr);
      rt_data2.emplace_back(nullptr);
      continue;
    }
    auto& tsd = ts_descs[index];
    bool is_clear = (index == io::DST0);
    auto ranges = std::vector<float>{-10, 10};
    auto data_pair = make_data_obj(tsd.shape(), tsd.dtype(), is_clear, ranges);
    rt_data1.emplace_back(data_pair.first);
    rt_data2.emplace_back(data_pair.second);
  }

  jd::operator_desc op_desc(jd::kernel_kind::transpose_matmul, jd::kernel_prop::forward_inference, jd::engine_kind::cpu,
                            ts_descs, attrs);

  // Step 3: op_args_t testcase pair
  op_args_t op_args = {op_desc, rt_data1, nthr};
  op_args_t op_args_copy = {op_desc, rt_data2, nthr};

  return {op_args, op_args_copy};
}

static auto case_func = []() {
  google::InitGoogleLogging("MMVNNINopermP2031P1302KernelTest");
  std::vector<int> nthr_cases = {1, 2, 3, 4, 0};

  std::vector<test_params_t> cases;

  for (int nthr : nthr_cases) {
    n_thread_t with_n_thread(nthr);
    // seq = 32
    cases.push_back({gen_case(32, 32, 64, 1, 12, nthr, {})});
    cases.push_back({gen_case(32, 32, 64, 6, 12, nthr, {})});
    cases.push_back({gen_case(32, 32, 64, 8, 12, nthr, {})});
    // seq = 32 unified kernel
    cases.push_back({gen_case(32, 32, 64, 1, 12, nthr, {{"unified", "1"}})});
    cases.push_back({gen_case(32, 32, 64, 6, 12, nthr, {{"unified", "1"}})});
    cases.push_back({gen_case(32, 32, 64, 8, 12, nthr, {{"unified", "1"}})});

    // seq = 384
    cases.push_back({gen_case(384, 384, 64, 1, 12, nthr, {})});
    cases.push_back({gen_case(384, 384, 64, 2, 12, nthr, {})});
    cases.push_back({gen_case(384, 384, 64, 4, 12, nthr, {})});

    // huge seq
    cases.push_back({gen_case(4096, 4096, 32, 1, 1, nthr, {})});
  }
  return ::testing::ValuesIn(cases);
};

std::string test_suffix(testing::TestParamInfo<test_params_t> tpi) {
  const auto shapes = tpi.param.args.first.op_desc.tensor_shapes();
  auto attrs = tpi.param.args.first.op_desc.attrs();

  const dim_t bs0 = shapes[io::SRC0][0];
  const dim_t bs1 = shapes[io::SRC0][1];
  const dim_t M = shapes[io::SRC0][2];  // aka src0_perm_shape[2]
  const dim_t K = shapes[io::SRC0][3];  // aka src0_perm_shape[3]
  const dim_t N = shapes[io::SRC1][1];  // aka src1_perm_shape[3]
  std::vector<std::string> params;
  params.push_back("c" + std::to_string(static_cast<int>(tpi.param.args.first.nthr)));
  params.push_back(std::to_string(bs0));
  params.push_back(std::to_string(bs1));
  params.push_back(std::to_string(M));
  params.push_back(std::to_string(K));
  params.push_back(std::to_string(N));
  if (attrs["unified"] == "1") params.push_back("unified");
  return join_str(params, "_");
}

INSTANTIATE_TEST_SUITE_P(SparseLib, MMVNNINopermP2031P1302KernelTest, case_func(), test_suffix);
}  // namespace test
