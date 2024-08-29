/*******************************************************************************
* Copyright 2020-2024 Intel Corporation
* Copyright 2020 Codeplay Software Limited
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef GPU_NVIDIA_CUDNN_MATMUL_HPP
#define GPU_NVIDIA_CUDNN_MATMUL_HPP

#include "gpu/gpu_matmul_pd.hpp"

#include "gpu/nvidia/cudnn_matmul_executor.hpp"
#include "gpu/nvidia/cudnn_matmul_impl.hpp"
#include "gpu/nvidia/cudnn_matmul_base.hpp"
#include "gpu/nvidia/sycl_cuda_utils.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace nvidia {

struct cudnn_matmul_t : cudnn_matmul_base_t {
    using cudnn_matmul_base_t::cudnn_matmul_base_t;

    struct pd_t : public cudnn_matmul_base_pd_t {
        using cudnn_matmul_base_pd_t::cudnn_matmul_base_pd_t;

        DECLARE_COMMON_PD_T("cuda:cudnn:any", cudnn_matmul_t);

        status_t init(impl::engine_t *engine) override {
            using namespace data_type;
            using smask_t = primitive_attr_t::skip_mask_t;

            data_type_t src_dt = src_md()->data_type;
            data_type_t dst_dt = dst_md()->data_type;
            data_type_t wei_dt = weights_md(0)->data_type;
            data_type_t bia_dt
                    = with_bias() ? weights_md(1)->data_type : data_type::f32;

            bool f32_case = utils::everyone_is(f32, src_dt, wei_dt, dst_dt);
            bool f16_case = utils::everyone_is(f16, src_dt, wei_dt, dst_dt);
            bool bf16_case = utils::everyone_is(bf16, src_dt, wei_dt, dst_dt);
            bool s8_case = utils::everyone_is(s8, src_dt, wei_dt)
                    && utils::one_of(dst_dt, s8, f32);

            auto *sycl_engine_impl
                    = utils::downcast<const xpu::sycl::engine_impl_t *>(
                            engine->impl());

            bool ok = is_dense_format_kind() && blocking_ok()
                    && attr()->has_default_values(
                            smask_t::scales_runtime | smask_t::post_ops)
                    && scales_ok() && attr_post_ops_ok(attr())
                    && IMPLICATION(bf16_case,
                            has_bf16_support(sycl_engine_impl->device()))
                    && set_default_formats()
                    && (f32_case || f16_case || bf16_case || s8_case)
                    && IMPLICATION(with_bias(),
                            (IMPLICATION(f32_case, utils::one_of(bia_dt, f32))
                                    && IMPLICATION(f16_case,
                                            utils::one_of(bia_dt, f16, f32))
                                    && IMPLICATION(bf16_case,
                                            utils::one_of(bia_dt, bf16, f32))
                                    && IMPLICATION(s8_case,
                                            utils::one_of(bia_dt, s8, f32))))
                    && !(with_bias() && s8_case);
            if (!ok) return status::unimplemented;

            if (src_md()->ndims > 3) return status::unimplemented;

            return status::success;
        }

        size_t scratchpad_size(const memory_desc_t *dst_md) const {
            const auto dst_nelems = memory_desc_wrapper(dst_md).nelems(true);
            return dst_nelems * sizeof(float);
        }

        bool scales_ok() const {
            const auto &scales = attr()->scales_;
            const auto &supported_args
                    = {DNNL_ARG_SRC, DNNL_ARG_WEIGHTS, DNNL_ARG_DST};
            if (!scales.has_default_values(supported_args)) return false;
            // cuDNN does not support scaling per dimension.
            for (auto arg : supported_args)
                if (scales.get(arg).mask_ != 0) return false;
            return true;
        }
    };

    status_t init(impl::engine_t *engine) override {
        matmul_impl_.reset(new cudnn_matmul_impl_t());
        auto status = matmul_impl_->init((matmul_pd_t *)pd());
        if (status != status::success) return status;

        bool has_runtime_args = matmul_impl_->has_runtime_params();

        if (has_runtime_args) {
            executor_.reset(new cudnn_matmul_runtime_args_exec_t);
        } else {
            executor_.reset(new cudnn_matmul_exec_t);
        }
        return status;
    }

    status_t execute(const exec_ctx_t &ctx) const override;

    std::shared_ptr<cudnn_matmul_impl_t> matmul_impl_;
    std::shared_ptr<cudnn_matmul_base_exec_t> executor_;

private:
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd().get(); }
};

} // namespace nvidia
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif
