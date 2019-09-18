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

#include "tensorflow/compiler/plugin/poplar/driver/passes/parse_poplar_backend_config.h"

#include "tensorflow/compiler/plugin/poplar/driver/backend_config.pb.h"
#include "tensorflow/compiler/plugin/poplar/driver/config.pb.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/util.h"

namespace xla {

namespace poplarplugin {

StatusOr<bool> ParsePoplarBackendConfig::Run(HloModule* module) {
  bool changed = false;

  for (auto* comp : module->computations()) {
    for (auto instr : comp->instructions()) {
      auto attributes = instr->frontend_attributes();
      PoplarBackendConfig poplar_config;
      // Check if the calls they have the type field set from tf2xla.
      if (instr->opcode() == HloOpcode::kCall) {
        auto call_config_type_attribute =
            attributes.map().find(FrontendAttributeId_Name(CALL_CONFIG_TYPE));
        if (call_config_type_attribute != attributes.map().end()) {
          PoplarBackendConfig::CallConfig::Type type;
          bool type_parsed = PoplarBackendConfig_CallConfig_Type_Parse(
              call_config_type_attribute->second, &type);
          if (!type_parsed) {
            return xla::FailedPrecondition("Could not parse the call type.");
          }
          auto* call_config = poplar_config.mutable_call_config();
          call_config->set_type(type);
          switch (type) {
            case PoplarBackendConfig::CallConfig::Pipeline: {
              // Get the repeat count.
              auto itr = attributes.map().find(
                  FrontendAttributeId_Name(PIPELINE_DEPTH));
              if (itr == attributes.map().end()) {
                return xla::FailedPrecondition(
                    "Expected the pipeline to contain the `pipeline_depth` "
                    "attribute.");
              }
              auto* pipeline_config = call_config->mutable_pipeline_config();
              int64 pipeline_depth = std::stoll(itr->second);
              pipeline_config->set_pipeline_depth(pipeline_depth);
              break;
            }
            case PoplarBackendConfig::CallConfig::PipelineStage:
            case PoplarBackendConfig::CallConfig::PipelineStageBackward: {
              // Get the stage id.
              auto itr = attributes.map().find(
                  FrontendAttributeId_Name(PIPELINE_STAGE_ID));
              if (itr == attributes.map().end()) {
                return xla::FailedPrecondition(
                    "Expected the pipeline stage to contain the `stage_id` "
                    "attribute.");
              }
              auto* pipeline_stage_config =
                  call_config->mutable_pipeline_stage_config();
              int64 stage_id = std::stoll(itr->second);
              pipeline_stage_config->set_stage_id(stage_id);
              break;
            }
            default: { break; }
          }
          changed = true;
        }
      }
      instr->set_backend_config(poplar_config);
    }
  }
  return changed;
}

}  // namespace poplarplugin
}  // namespace xla
