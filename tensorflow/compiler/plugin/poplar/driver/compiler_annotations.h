/* Copyright 2017 Graphcore Ltd
 */

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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_COMPILER_ANNOTATIONS_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_COMPILER_ANNOTATIONS_H_

#include "tensorflow/compiler/plugin/poplar/driver/allocation_finder.h"
#include "tensorflow/compiler/plugin/poplar/driver/convolution_classifier.h"
#include "tensorflow/compiler/plugin/poplar/driver/inplace_finder.h"
#include "tensorflow/compiler/plugin/poplar/driver/inplace_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/input_output_aliasing_map.h"

#include "absl/container/flat_hash_map.h"

namespace xla {
namespace poplarplugin {

// This structure contains all information which we generate that pertains
// to the XLA graph, as opposed to the poplar lowering of that graph.
struct CompilerAnnotations {
  CompilerAnnotations(const HloModule* module)
      : input_output_aliasing_map(module) {}

  InputOutputAliasingMap input_output_aliasing_map;

  TensorAllocationMap tensor_allocation_map;

  ConvClassification classification_map;

  absl::flat_hash_map<const HloInstruction*,
                      InplaceUtil::InplaceHloInstructionDescription>
      inplace_calls;

  InplaceUtil::InplaceInstructions inplace_instructions;

  std::map<const HloComputation*, const HloInstruction*> fusion_map;

  // A map from a while instruction to the repeat count. If the while
  // instruction is not present in this map then it can't be executed as a
  // repeat.
  std::map<const HloInstruction*, uint64> while_loop_num_iterations;
};

}  // namespace poplarplugin
}  // namespace xla

#endif
