/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/compiler/plugin/poplar/driver/platform.h"
#include "tensorflow/compiler/plugin/poplar/driver/trace.pb.h"

#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/platform/env.h"
#include "tensorflow/core/util/stream_executor_util.h"

namespace xp = ::xla::poplarplugin;

namespace tensorflow {

class IpuSummaryOp : public OpKernel {
 public:
  explicit IpuSummaryOp(OpKernelConstruction* ctx) : OpKernel(ctx) {}
  ~IpuSummaryOp() override{};

  void Compute(OpKernelContext* ctx) override {
    auto platform = se::MultiPlatformManager::PlatformWithName("Poplar");
    OP_REQUIRES(ctx, platform.ok(), platform.status());

    auto* p = static_cast<xp::PoplarPlatform*>(platform.ValueOrDie());

    std::list<tensorflow::IpuTraceEvent> out;
    OP_REQUIRES_OK(ctx, p->GetCompilerEvents(out));

    int num = out.size();

    Tensor* output_tensor = nullptr;
    OP_REQUIRES_OK(
        ctx, ctx->allocate_output("out", TensorShape({num}), &output_tensor));
    auto output_flat = output_tensor->flat<string>();

    unsigned i = 0;
    for (auto& e : out) {
      std::string str;
      e.SerializeToString(&str);

      output_flat(i) = str;
      i++;
    }
  }

 private:
  TF_DISALLOW_COPY_AND_ASSIGN(IpuSummaryOp);
};

REGISTER_KERNEL_BUILDER(Name("IpuEventTrace").Device(DEVICE_CPU), IpuSummaryOp);

}  // namespace tensorflow
