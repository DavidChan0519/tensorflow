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

#include "tensorflow/compiler/plugin/poplar/driver/passes/allocation_finder.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/convolution_classifier.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/inplace_finder.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/inplace_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_feed_config.pb.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/input_output_aliasing_map.h"

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"

namespace xla {
class HloInfeedInstruction;
namespace poplarplugin {

struct FeedInfo {
  std::string stream_prefix;
  PoplarFeedConfig config;
  Shape shape;
};
using OutfeedInfos = std::vector<FeedInfo>;
using InfeedInfos = std::vector<FeedInfo>;

// This structure contains all information which we generate that pertains
// to the XLA graph, as opposed to the poplar lowering of that graph.
struct CompilerAnnotations {
  CompilerAnnotations(const HloModule* module)
      : input_output_aliasing_map(module) {}

  InputOutputAliasingMap input_output_aliasing_map;

  TensorAllocationMap tensor_allocation_map;

  DeferredAllocations deferred_allocations;

  ConvClassification classification_map;

  InplaceInstructions inplace_instructions;

  InfeedInfos infeed_infos;

  OutfeedInfos outfeed_infos;

  TensorsWithLayouts tensors_with_layout;
};

}  // namespace poplarplugin
}  // namespace xla

#endif
