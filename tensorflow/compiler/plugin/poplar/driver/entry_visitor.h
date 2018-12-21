/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_ENTRY_VISITOR_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_ENTRY_VISITOR_H_

#include "tensorflow/compiler/plugin/poplar/driver/executor.h"
#include "tensorflow/compiler/plugin/poplar/driver/visitor_full.h"

namespace xla {
namespace poplarplugin {

struct CompilerResources;

/*
 * This visitor handles inputs and outputs of the entry computation in a module.
 */
class EntryVisitor : public FullVisitor {
 public:
  EntryVisitor(CompilerResources& resources,
               const bool always_rearrange_copies_on_the_host)
      : FullVisitor(resources),
        always_rearrange_copies_on_the_host(
            always_rearrange_copies_on_the_host) {}

  Status HandleParameter(HloInstruction* inst);
  Status FinishVisit(HloInstruction* root);

  const std::set<const HloInstruction*>& GetNonStandardParameterLayout() const;

  const poplar::program::Sequence& GetHostToDevice() const;
  const poplar::program::Sequence& GetDeviceToHost() const;

 private:
  Status StreamOutputs(HloInstruction* inst, uint64 start_idx,
                       OutVector outputs);

  std::set<const HloInstruction*> non_standard_parameter_layout;
  std::set<HloInstruction*> non_standard_parameter_layout_defer;

  poplar::program::Sequence host_to_device;
  poplar::program::Sequence device_to_host;

  const bool always_rearrange_copies_on_the_host;
};

}  // namespace poplarplugin
}  // namespace xla

#endif
