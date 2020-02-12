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

#include "tensorflow/compiler/plugin/poplar/driver/passes/gradient_accumulation_fuser.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/replication_factor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/stateful_gradient_accumulate.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/hlo_matcher.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matcher_predicates.h"

#include "tensorflow/compiler/xla/literal.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"

#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"

#include <set>

namespace xla {
namespace poplarplugin {
namespace {
// The
// `tensorflow/compiler/plugin/poplar/graph_optimizer_passes/reorder_gradient_accumulation_pass.cc`
// pass guarantees the order of operations.
// clang-format off
static const std::vector<HloMatcherPattern> patterns = {
  HloMatcherPattern(
    PatternType("all_reduce_then_normalize_then_grad_accum"),
    PatternMetaTarget(0),
    PatternInputs({3}),
    PatternOutputs({0}),
    Pattern({
      {HloOpcode::kCustomCall, NodeOperands({1}), IsPoplarInstruction(PoplarOp::StatefulGradientAccumulate)},
      {HloOpcode::kCustomCall, NodeOperands({2}), IsPoplarInstruction(PoplarOp::ReplicationNormalise)},
      {HloOpcode::kAllReduce, NodeOperands({3}), IsSupportedAllReduce},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
    })
  ),
  HloMatcherPattern(
    PatternType("all_reduce_then_grad_accum"),
    PatternMetaTarget(0),
    PatternInputs({2}),
    PatternOutputs({0}),
    Pattern({
      {HloOpcode::kCustomCall, NodeOperands({1}), IsPoplarInstruction(PoplarOp::StatefulGradientAccumulate)},
      {HloOpcode::kAllReduce, NodeOperands({2}), IsSupportedAllReduce},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
    })
  ),
};
// clang-format on
}  // namespace

GradientAccumulationFuser::GradientAccumulationFuser(
    struct CompilerAnnotations& annotations)
    : HloMatcher(patterns, annotations, false, true) {}

bool GradientAccumulationFuser::HandleMatch(
    HloMatcherMatched& match, const absl::optional<int64> sharding_device) {
  auto pattern = patterns_[match.pattern_idx];
  auto comp = match.computation;
  // Get the input output id.
  CHECK_EQ(pattern.GetInputs().size(), 1);
  NodeId input_id = pattern.GetInputs()[0];
  CHECK_EQ(pattern.GetOutputs().size(), 1);
  NodeId output_id = pattern.GetOutputs()[0];

  // Don't bother replacing the instructions if there is an instruction in
  // matched_instructions - {input, output} which has more than one user.
  for (auto pair : match.instruction_mapping) {
    auto id = pair.first;
    auto inst = pair.second;
    if (!(id == input_id || id == output_id) && inst->users().size() > 1) {
      return false;
    }
  }
  // Get the gradient accumulation instruction.
  auto grad_accum =
      Cast<HloStatefulGradientAccumulate>(match.instruction_mapping.at(0));

  auto input = match.instruction_mapping.at(input_id);

  // Create the accumulated gradient instructions.
  auto new_output =
      comp->AddInstruction(CreateStatefulGradientAccumulateAndAllReduce(
          {input}, grad_accum->MiniBatchesToAccumulate()));
  // Set the sharding device if there was any.
  if (sharding_device) {
    new_output->set_device_sharding(*sharding_device);
  }

  // Get the output so that we can replace the uses.
  auto output = match.instruction_mapping.at(output_id);
  // If there was a normalization.
  if (match.pattern_idx == 0) {
    // Make the normalization take the all reduced accumulated gradient as input
    // and use that as the output of the match. We can swap the order of these
    // operations because we know the normalization can be delayed after the
    // accumulation.
    auto normalization = output->mutable_operand(0);
    CHECK(IsPoplarInstruction(PoplarOp::ReplicationNormalise)(normalization));
    normalization->ReplaceOperandWith(0, new_output);
    new_output = normalization;
  }

  output->ReplaceAllUsesWith(new_output);
  comp->RemoveInstructionAndUnusedOperands(output);

  // If there was a normalization.
  if (match.pattern_idx == 0) {
    // Explicitly remove the all-reduce as it has side-effects and it will not
    // be removed.
    HloInstruction* all_reduce =
        match.instruction_mapping.at(match.instruction_mapping.size() - 2);
    CHECK_EQ(all_reduce->opcode(), HloOpcode::kAllReduce);
    CHECK_EQ(all_reduce->user_count(), 0);
    comp->RemoveInstructionAndUnusedOperands(all_reduce);
  }

  return true;
}

}  // namespace poplarplugin
}  // namespace xla
