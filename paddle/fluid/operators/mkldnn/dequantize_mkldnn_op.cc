/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/fluid/operators/dequantize_op.h"

#include "paddle/fluid/framework/tensor.h"
#include "paddle/fluid/platform/mkldnn_helper.h"
#include "paddle/phi/backends/onednn/onednn_reuse.h"
#include "paddle/phi/core/errors.h"

namespace paddle {
namespace operators {

using dnnl::memory;
using dnnl::primitive;
using dnnl::reorder;
using dnnl::stream;

template <typename T>
class DeQuantOpKernel : public framework::OpKernel<T> {
 public:
  void Compute(const framework::ExecutionContext& ctx) const override {
    auto* x = ctx.Input<phi::DenseTensor>("Input");
    const auto quantization_scale = ctx.Attr<float>("Scale");
    const auto quantization_shift =
        static_cast<int32_t>(ctx.Attr<float>("Shift"));
    const bool with_shift = quantization_shift != 0.0f;
    auto* out = ctx.Output<phi::DenseTensor>("Output");

    PADDLE_ENFORCE(quantization_scale != 0.0f,
                   phi::errors::InvalidArgument(
                       "Dequantization scale must be different than 0.0f"));

    PADDLE_ENFORCE(quantization_shift <= 255 && quantization_shift >= 0,
                   phi::errors::InvalidArgument(
                       "Dequantization shift must be lower or equal to ",
                       "255 and greater or equal to 0, but got %f",
                       quantization_shift));

    auto& dev_ctx = ctx.template device_context<phi::OneDNNContext>();

    auto x_tz = phi::vectorize<int64_t>(x->dims());
    auto x_type = phi::funcs::ToOneDNNDataType(x->dtype());
    auto out_type = phi::funcs::ToOneDNNDataType(out->dtype());

    dnnl::primitive_attr attrs;
    static constexpr int32_t mask = 0;  // same shift and scale for whole tensor

    const float reorder_scale = quantization_scale;
    //    attrs.set_output_scales(mask, {reorder_scale});
    attrs.set_scales_mask(DNNL_ARG_DST, mask);

    if (with_shift) {
      attrs.set_zero_points_mask(DNNL_ARG_SRC, mask);
    }

    phi::funcs::ReorderOneDNNHandler reorder_handler(
        x_tz, x->dtype(), x_type, out->dtype(), out_type, dev_ctx.GetEngine());

    auto reorder_src_memory_p = reorder_handler.AcquireSrcMemory(
        x->mem_desc(), phi::funcs::to_void_cast(x->data<T>()));
    auto reorder_dst_memory_p = reorder_handler.AcquireDstMemory(
        out, x->mem_desc(), dev_ctx.GetPlace());

    auto reorder_p = reorder_handler.AcquireReorder(
        reorder_dst_memory_p, reorder_src_memory_p, attrs);

    auto& astream = phi::OneDNNContext::tls().get_stream();

    auto scales_md = dnnl::memory::desc(
        {1}, dnnl::memory::data_type::f32, dnnl::memory::format_tag::x);
    auto scales_mem =
        dnnl::memory(scales_md,
                     dev_ctx.GetEngine(),
                     phi::funcs::to_void_cast<float>(&reorder_scale));

    auto zero_points_md = dnnl::memory::desc(
        {1}, dnnl::memory::data_type::s32, dnnl::memory::format_tag::x);
    auto zero_points_mem =
        dnnl::memory(zero_points_md,
                     dev_ctx.GetEngine(),
                     phi::funcs::to_void_cast<int32_t>(&quantization_shift));
    std::unordered_map<int, dnnl::memory> reorder_args;
    reorder_args.insert({DNNL_ARG_SRC, *reorder_src_memory_p});
    reorder_args.insert({DNNL_ARG_DST, *reorder_dst_memory_p});
    reorder_args.insert({DNNL_ARG_ATTR_SCALES | DNNL_ARG_DST, scales_mem});
    if (with_shift) {
      reorder_args.insert(
          {DNNL_ARG_ATTR_ZERO_POINTS | DNNL_ARG_SRC, zero_points_mem});
    }
    reorder_p->execute(astream, reorder_args);
    astream.wait();

    out->set_mem_desc(reorder_dst_memory_p->get_desc());
  }
};

}  // namespace operators
}  // namespace paddle

namespace ops = paddle::operators;

REGISTER_OP_KERNEL(dequantize,
                   MKLDNN,
                   ::phi::CPUPlace,
                   ops::DeQuantOpKernel<uint8_t>,
                   ops::DeQuantOpKernel<int8_t>,
                   ops::DeQuantOpKernel<paddle::platform::bfloat16>);
