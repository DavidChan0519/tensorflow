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

#include "tensorflow/compiler/plugin/poplar/driver/passes/pipeline_recomputation.h"

#include <list>
#include <memory>
#include <utility>

#include "tensorflow/compiler/plugin/poplar/driver/backend_config.pb.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/inplace_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/fifo.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/stateful_noop.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matcher_predicates.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/pipeline_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"

#include "tensorflow/compiler/xla/service/hlo_creation_utils.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"
#include "tensorflow/compiler/xla/shape_util.h"

#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"
#include "tensorflow/core/lib/strings/str_util.h"

namespace xla {
namespace poplarplugin {

namespace {
std::list<HloInstruction*> GetStatefulInstructions(HloComputation* comp) {
  std::list<HloInstruction*> stateful_ops;
  for (auto inst : comp->instructions()) {
    if (!IsPoplarInstruction(PoplarOp::StatefulNoop)(inst) &&
        inst->HasSideEffect()) {
      stateful_ops.emplace_back(inst);
    }
  }
  return stateful_ops;
}

// Create a new stage which returns the original stage's outputs followed by
// the output of all the stateful ops.
StatusOr<HloComputation*> CloneStageCompWithStates(
    HloInstruction* stage, HloComputation* pipeline_comp) {
  HloComputation* stage_comp = pipeline_comp->parent()->AddEmbeddedComputation(
      stage->to_apply()->Clone("state"));

  // Return the output of all the stateful ops to pipeline
  auto* root = stage_comp->root_instruction();
  CHECK(root->opcode() == HloOpcode::kTuple);
  HloInstruction::InstructionVector tuple_elts = root->operands();

  // Add the output of the stateful ops to the output of the computation.
  absl::c_copy(GetStatefulInstructions(stage_comp),
               std::back_inserter(tuple_elts));

  Shape shape = stage->shape();
  HloInstruction* new_root =
      stage_comp->AddInstruction(HloInstruction::CreateTuple(tuple_elts));
  root->SetupDerivedInstruction(new_root);
  auto optional_sharding = stage->sharding().ExtractSingleSharding();
  if (!optional_sharding) {
    return FailedPrecondition("Could not extract single sharding.");
  }
  new_root->set_sharding(
      HloSharding::SingleTuple(stage->shape(), *optional_sharding));
  stage_comp->set_root_instruction(new_root, true);
  TF_RETURN_IF_ERROR(stage_comp->RemoveInstruction(root));
  return stage_comp;
}

StatusOr<HloInstruction*> CreateRecomputationStage(
    HloComputation* original_stage_comp, HloInstruction* stage,
    HloComputation* pipeline_comp, const HloSharding& sharding) {
  absl::flat_hash_map<const HloInstruction*, std::unique_ptr<HloInstruction>>
      replacements;
  HloInstruction::InstructionVector recomp_operands = stage->operands();
  // Outputs of the stateful ops are passed as extra parameters after
  // the regular stage operands.
  const int64 first_parameter_idx = stage->operands().size();
  int64 tuple_index =
      original_stage_comp->root_instruction()->operands().size();
  {
    int64 parameter_idx = first_parameter_idx;
    // Create the gte (Fifos will be created later)
    for (auto inst : GetStatefulInstructions(original_stage_comp)) {
      TF_ASSIGN_OR_RETURN(HloInstruction * gte,
                          MakeGetTupleElementHlo(stage, tuple_index++));
      gte->set_sharding(sharding);

      recomp_operands.push_back(gte);
      auto param = HloInstruction::CreateParameter(
          parameter_idx++, gte->shape(), inst->name() + "_state");
      param->set_sharding(sharding);
      replacements.emplace(inst, std::move(param));
    }
  }
  HloComputation* recomp_stage_comp =
      pipeline_comp->parent()->AddEmbeddedComputation(
          original_stage_comp->CloneWithReplacements(
              std::move(replacements), {}, nullptr, "recomputation"));

  HloInstruction* recomp_stage =
      pipeline_comp->AddInstruction(stage->CloneWithNewOperands(
          original_stage_comp->root_instruction()->shape(), recomp_operands));
  recomp_stage->set_sharding(sharding);
  recomp_stage->set_to_apply(recomp_stage_comp);
  recomp_stage->SetAndSanitizeName(recomp_stage_comp->name());
  return recomp_stage;
}

}  // namespace
PipelineRecomputation::PipelineRecomputation(bool allow_recomputation,
                                             bool allow_stateful_recomputation)
    : allow_recomputation_(allow_recomputation),
      allow_stateful_recomputation_(allow_stateful_recomputation) {}

StatusOr<bool> PipelineRecomputation::RecomputePipeline(
    HloInstruction* pipeline_op) {
  HloComputation* pipeline_comp = pipeline_op->to_apply();
  TF_ASSIGN_OR_RETURN(PipelineStages stages, GetPipelineStages(pipeline_comp));
  // Do not perform recomputation if there are no backward stages.
  if (stages.backward.empty()) {
    return false;
  }

  bool changed = false;
  // Go through all the forward stages (apart from the last one which does not
  // need recomputation).
  for (int64 stage_id = 0;
       stage_id != static_cast<int64>(stages.forward.size()) - 1; ++stage_id) {
    HloInstruction* fwd_stage = stages.forward[stage_id];
    HloInstruction* bwd_stage = stages.backward[stage_id];
    // Do not recompute a stage if it has no outputs which go into the
    // corresponding backward stage (i.e. there is no FIFO).
    const bool bwd_uses_fwd = absl::c_any_of(
        bwd_stage->operands(), IsPoplarInstruction(PoplarOp::Fifo));
    if (!bwd_uses_fwd) {
      continue;
    }

    // Stages containing stateful ops require special treatment.
    // Note that to prevent DCE each pipeline stage has had a stateful noop
    // inserted inside, so we cannot just call `HasSideEffect` on the stage
    // computation.
    const bool has_side_effects = absl::c_any_of(
        fwd_stage->to_apply()->instructions(), [](const HloInstruction* inst) {
          return IsPoplarInstruction(PoplarOp::StatefulNoop)(inst)
                     ? false
                     : inst->HasSideEffect();
        });
    HloInstruction* recomp_stage;

    if (has_side_effects) {
      if (!allow_stateful_recomputation_) {
        LOG(INFO)
            << "Recomputation has been enabled however the pipeline stage "
            << stage_id
            << " cannot be recomputed because recomputation of instructions "
               "with side-effect hasn't been enabled and it contains some.";
        continue;
      }
      // Find all the stateful instructions in that stage.
      HloComputation* original_fwd_stage_comp = fwd_stage->to_apply();
      TF_ASSIGN_OR_RETURN(auto comp_states,
                          CloneStageCompWithStates(fwd_stage, pipeline_comp));
      fwd_stage->set_to_apply(comp_states);
      // Update the stage shape.
      *fwd_stage->mutable_shape() =
          fwd_stage->to_apply()->root_instruction()->shape();

      CHECK_EQ(fwd_stage->to_apply()->root_instruction()->sharding(),
               fwd_stage->sharding());
      TF_ASSIGN_OR_RETURN(
          recomp_stage,
          CreateRecomputationStage(original_fwd_stage_comp, fwd_stage,
                                   pipeline_comp, fwd_stage->sharding()));
      TF_RETURN_IF_ERROR(pipeline_comp->parent()->RemoveUnusedComputations());
    } else {
      HloComputation* fwd_stage_comp = fwd_stage->to_apply();
      // Clone the stage and its computation.
      HloComputation* recomp_stage_comp =
          pipeline_comp->parent()->AddEmbeddedComputation(
              fwd_stage_comp->Clone("_recomputation"));
      recomp_stage =
          pipeline_comp->AddInstruction(fwd_stage->Clone("_recomputation"));
      recomp_stage->set_to_apply(recomp_stage_comp);
    }

    // Mark this stage as a Recomputation stage.
    TF_ASSIGN_OR_RETURN(PoplarBackendConfig config,
                        recomp_stage->backend_config<PoplarBackendConfig>());
    config.mutable_call_config()->set_type(
        PoplarBackendConfig::CallConfig::PipelineStageRecomputation);
    recomp_stage->set_backend_config(config);
    CHECK(IsPipelineStageRecomputation(recomp_stage));

    TF_ASSIGN_OR_RETURN(PoplarBackendConfig pipeline_config,
                        pipeline_op->backend_config<PoplarBackendConfig>());

    const auto schedule =
        pipeline_config.call_config().pipeline_config().schedule();
    TF_ASSIGN_OR_RETURN(const int fifo_depth_multiplier,
                        ScheduleToFifoDepthMultiplier(schedule));

    // Replace all the non parameter inputs with FIFOs.
    auto recomp_operands = recomp_stage->operands();
    for (size_t op_idx = 0; op_idx != recomp_operands.size(); ++op_idx) {
      HloInstruction* operand = recomp_operands[op_idx];
      if (operand->opcode() == HloOpcode::kParameter) {
        continue;
      }
      // Create the FIFO.
      HloInstruction* fifo_inst = pipeline_comp->AddInstruction(CreateFifo(
          operand,
          fifo_depth_multiplier * (stages.forward.size() - stage_id - 1)));
      fifo_inst->SetAndSanitizeName(operand->name() + ".fifo");
      fifo_inst->set_sharding(operand->sharding());
      // Use the fifo as the input.
      TF_RETURN_IF_ERROR(recomp_stage->ReplaceOperandWith(op_idx, fifo_inst));
      // If there is an inplace user of the operand, then we need to add a
      // control dependency from the new FIFO instruction to that user.
      auto optional_inplace_user = GetInplaceModifier(operand);
      if (optional_inplace_user) {
        TF_RETURN_IF_ERROR(
            fifo_inst->AddControlDependencyTo(*optional_inplace_user));
      }
    }

    // Wire inputs to the bwd stage which are FIFOs to use the recomp stage.
    auto bwd_operands = bwd_stage->operands();
    for (size_t op_idx = 0; op_idx != bwd_operands.size(); ++op_idx) {
      HloInstruction* operand = bwd_operands[op_idx];
      if (!IsPoplarInstruction(PoplarOp::Fifo)(operand)) {
        continue;
      }
      // We expect the FIFO input to be a GTE on a forward stage.
      HloInstruction* gte = operand->mutable_operand(0);
      CHECK_EQ(gte->opcode(), HloOpcode::kGetTupleElement);
      CHECK_EQ(gte->operand(0), fwd_stage);
      // Create a GTE from the recomputation output and wire it to the BWD
      // stage.
      HloInstruction* new_gte = pipeline_comp->AddInstruction(
          gte->CloneWithNewOperands(gte->shape(), {recomp_stage}));
      TF_RETURN_IF_ERROR(bwd_stage->ReplaceOperandWith(op_idx, new_gte));

      // Remove the old operand.
      TF_RETURN_IF_ERROR(operand->DropAllControlDeps());
      TF_RETURN_IF_ERROR(
          pipeline_comp->RemoveInstructionAndUnusedOperands(operand));
    }

    // Make sure that the fwd pass is executed before the recomputation
    fwd_stage->AddControlDependencyTo(recomp_stage);

    VLOG(1) << "Added recomputation for pipeline stage " << stage_id;
    changed = true;
  }
  return changed;
}

StatusOr<bool> PipelineRecomputation::Run(HloModule* module) {
  if (!allow_recomputation_) {
    return false;
  }

  TF_ASSIGN_OR_RETURN(std::vector<HloInstruction*> pipeline_ops,
                      GetPipelines(module));
  if (pipeline_ops.empty()) {
    // No pipeline ops found - nothing to fix.
    return false;
  }
  CHECK_EQ(pipeline_ops.size(), 1);
  VLOG(2) << "Before PipelineRecomputation:";
  XLA_VLOG_LINES(2, module->ToString(HloPrintOptions::ShortParsable()));

  TF_ASSIGN_OR_RETURN(bool changed, RecomputePipeline(pipeline_ops[0]));

  if (changed) {
    VLOG(2) << "After PipelineRecomputation:";
    XLA_VLOG_LINES(2, module->ToString());
  } else {
    VLOG(2) << "No changes were made to the Pipeline.";
  }
  return changed;
}

}  // namespace poplarplugin
}  // namespace xla
