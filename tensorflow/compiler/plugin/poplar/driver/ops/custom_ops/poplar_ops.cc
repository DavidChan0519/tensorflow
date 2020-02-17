/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/plugin/poplar/driver/ops/custom_ops/poplar_ops.h"

#include <memory>
#include <utility>

#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/plugin/poplar/kernels/custom_kernels_util.h"

#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/compiler/xla/util.h"
#include "tensorflow/core/lib/core/errors.h"

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_split.h"

namespace xla {
namespace poplarplugin {

void PoplarOpManager::RegsiterOp(PoplarOp op,
                                 std::unique_ptr<PoplarOpDef> poplibs_op_def) {
  auto& ops = GetInstance().ops;

  if (ops.contains(op)) {
    LOG(FATAL) << "Trying to register the same op twice (" << PoplarOp_Name(op)
               << ").";
  }
  ops[op] = std::move(poplibs_op_def);
}

PoplarOpManager& PoplarOpManager::GetInstance() {
  static PoplarOpManager instance;
  return instance;
}

StatusOr<PoplarOpDef*> PoplarOpManager::GetOp(const HloInstruction* inst) {
  // Find the poplibs info given a CustomCall instruction.
  auto ret = GetPoplibsCustomOp(inst);
  if (!ret) {
    return FailedPrecondition("Could not find poplar op %s.",
                              inst->ToString().c_str());
  }

  auto& ops = GetInstance().ops;
  auto itr = ops.find(*ret);
  if (itr != ops.end()) {
    return itr->second.get();
  }
  return FailedPrecondition("Could not find definition for %s.",
                            PoplarOp_Name(*ret).c_str());
}

PoplarOpRegistrar::PoplarOpRegistrar(
    PoplarOp op, std::unique_ptr<PoplarOpDef> poplibs_op_def) {
  PoplarOpManager::RegsiterOp(op, std::move(poplibs_op_def));
}

}  // namespace poplarplugin
}  // namespace xla
