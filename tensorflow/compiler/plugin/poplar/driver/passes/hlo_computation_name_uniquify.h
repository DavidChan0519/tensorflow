/* Copyright 2018 Graphcore Ltd

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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_HLO_COMPUTATION_NAME_UNIQUIFY_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_HLO_COMPUTATION_NAME_UNIQUIFY_H_

#include "tensorflow/compiler/xla/service/hlo_pass_interface.h"

namespace xla {

class HloModule;

namespace poplarplugin {

class HloComputationNameUniquify : public HloModulePass {
 public:
  HloComputationNameUniquify() = default;

  ~HloComputationNameUniquify() override = default;

  absl::string_view name() const override {
    return "hlo-computation-name-uniquify";
  }

  StatusOr<bool> Run(HloModule* module) override;
};

}  // namespace poplarplugin
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_PASSES_HLO_COMPUTATION_NAME_UNIQUIFY_H_