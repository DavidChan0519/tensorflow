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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_WHILE_LOOP_CONDITION_SIMPLIFY_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_WHILE_LOOP_CONDITION_SIMPLIFY_H_

#include "tensorflow/compiler/xla/service/hlo_pass_interface.h"

namespace xla {

class HloModule;

namespace poplarplugin {

/** While loops in the Python frontend can sometimes have 2 loop conditionals
    which are of form "cond0 < const0 and cond1 < const1", where condn starts
    from 0, is incremented by 1 every iteration and constn is a constant.
    This pass attempts to simplify this conditional to simplify to
    "cond < max(cond1,cond2)".
 */
class WhileLoopConditionSimplify : public HloModulePass {
 public:
  WhileLoopConditionSimplify();

  ~WhileLoopConditionSimplify() override = default;

  absl::string_view name() const override {
    return "while-loop-condition-simplify";
  }

  StatusOr<bool> Run(HloModule *module) override;
};

}  // namespace poplarplugin
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_WHILE_LOOP_CONDITION_SIMPLIFY_H_
