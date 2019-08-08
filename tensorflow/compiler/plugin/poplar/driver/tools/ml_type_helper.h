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
#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_ML_TYPE_HELPER_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TOOLS_ML_TYPE_HELPER_H_

#include "tensorflow/compiler/plugin/poplar/driver/backend_config.pb.h"

#include "tensorflow/compiler/xla/statusor.h"

#include "absl/container/flat_hash_map.h"

namespace xla {

class HloInstruction;
class HloModule;

namespace poplarplugin {

// Sets the ML type of the instruction.
Status SetInstructionMLType(HloInstruction* inst, const MLType& type);
// Given an instruction, get its MLType.
StatusOr<MLType> GetMLType(const HloInstruction* inst);
// Given an instruction, get its MLType as a string.
StatusOr<std::string> GetMLTypeAsString(const HloInstruction* inst);
bool IsTrainingForward(const HloInstruction* inst);
bool IsTrainingBackward(const HloInstruction* inst);
bool IsTrainingWU(const HloInstruction* inst);

// A function which returns all the instructions which have a MLType which is
// not NONE (default).
// Used for testing.
StatusOr<absl::flat_hash_map<const HloInstruction*, MLType>> GetAllNotNoneTypes(
    const HloModule* module);

}  // namespace poplarplugin
}  // namespace xla

#endif
