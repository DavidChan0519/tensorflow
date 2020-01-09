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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_VISITORS_PIPELINE_STAGE_VISITOR_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_VISITORS_PIPELINE_STAGE_VISITOR_H_

#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/visitors/entry_visitor.h"
#include "tensorflow/compiler/plugin/poplar/driver/visitors/visitor_subcomputation.h"

namespace xla {
namespace poplarplugin {

struct CompilerResources;

class PipelineStageVisitor : public InplaceSubComputationVisitor {
 public:
  PipelineStageVisitor(CompilerResources& res, const ArgVectors& inputs);

  Status HandleTuple(HloInstruction* inst) override;

  // When recomputation of the pipline is enabled, the forward and the
  // recomputation stage share the Poplar program, meaning that their outputs
  // will be in the same tensor. To prevent clobbering of the tensors, copies
  // need to be inserted. This function takes a PipelineStage instruction and
  // returns for which output (flat_index) tensors we need to add copies.
  StatusOr<std::vector<bool>> GetOutputCopies(const HloInstruction* inst,
                                              bool used_for_recomputation);

  poplar::program::Sequence GetSequence() const override;

 private:
  // Caching fields for the GetSequence call
  mutable bool has_function_ = false;
  mutable poplar::Function function_;
};

}  // namespace poplarplugin
}  // namespace xla

#endif
