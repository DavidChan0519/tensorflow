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

#include "tensorflow/compiler/plugin/poplar/driver/visitors/visitor_base.h"
#include "tensorflow/compiler/plugin/poplar/driver/backend_config.pb.h"
#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"

#include "tensorflow/compiler/plugin/poplar/driver/passes/inplace_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/plugin/poplar/driver/visitors/visitor_arithmetic_expr.h"

#include "tensorflow/compiler/xla/layout_util.h"
#include "tensorflow/compiler/xla/service/buffer_assignment.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/lib/core/errors.h"

#include <stddef.h>
#include <string.h>
#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tensorflow/stream_executor/lib/initialize.h"

#include <poplar/CSRFunctions.hpp>
#include <poplar/Engine.hpp>
#include <poplar/GraphElements.hpp>
#include <poplar/Tensor.hpp>
#include <poplar/exceptions.hpp>
#include <poputil/Util.hpp>

using tensorflow::str_util::StartsWith;

namespace xla {
namespace poplarplugin {

typedef StatusOr<poplar::program::Program> (*CustomCallFn)(
    CompilerResources&, const HloInstruction*, const xla::Shape&, TensorMap&);

static std::map<std::string, CustomCallFn> custom_call_map = {
    {"conv_biasadd", CreateConvBiasAddOp},
    {"matmul_biasadd", CreateMatMulBiasAddOp},
    {"norm_scale_add", RandomNormalScale},
    {"uniform_scale_add", RandomUniformScale},
    {"wide_const", CreateWideConstant},
    {"depthwise_conv", CreateConv2D},
    {"conv_with_reverse", Create2DConvWithReverse},
    {"bias_apply", CreateBiasApply},
    {"zero_pad", CreateZeroPadOp},
    {"depthwise_filter", CreateDepthwiseBackpropFilter},
    {"scaled_inplace", CreateScaledInplace},
    {"conv_scaled_inplace", CreateConvScaledInplace},
    {"padding_reduce_window", CreatePaddingReduceWindow},
    {"implicit_binary", CreateBinaryElementwiseOp},
    {"implicit_binary_inplace", CreateBinaryElementwiseOp},
    {"implicit_ternary", CreateTernaryElementwiseOp},
    {"implicit_ternary_inplace", CreateTernaryElementwiseOp},
    {"scatter_update_inplace", CreateScatterUpdateOp},
};

BaseVisitor::BaseVisitor(CompilerResources& res) : resources_(res) {
  stochastic_rounding_enabled_ = res.global_floating_point_behaviour.esr();
}

const Shape& BaseVisitor::GetOutputShape(HloInstruction* inst) const {
  return inst->shape();
}

Status BaseVisitor::Unimplemented(HloInstruction* inst) {
  return xla::Unimplemented("%s (%s) not implemented", inst->name().c_str(),
                            HloOpcodeString(inst->opcode()).c_str());
}

Status BaseVisitor::HandleElementwiseUnary(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  TF_ASSIGN_OR_RETURN(poplar::program::Program prog,
                      CreateUnaryElementwiseOp(
                          resources_, inst, GetOutputShape(inst), tensor_map));
  sequence.add(prog);
  return Status::OK();
}

Status BaseVisitor::HandleElementwiseBinary(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  TF_ASSIGN_OR_RETURN(poplar::program::Program prog,
                      CreateBinaryElementwiseOp(
                          resources_, inst, GetOutputShape(inst), tensor_map));
  sequence.add(prog);
  return Status::OK();
}

Status BaseVisitor::HandleCompare(HloInstruction* inst) {
  return HandleElementwiseBinary(inst);
}

Status BaseVisitor::HandleConvert(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  TF_ASSIGN_OR_RETURN(
      poplar::program::Program prog,
      CreateCastOp(resources_, inst, GetOutputShape(inst), tensor_map));
  sequence.add(prog);
  return Status::OK();
}

Status BaseVisitor::HandleCopy(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleClamp(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  TF_ASSIGN_OR_RETURN(poplar::program::Program prog,
                      CreateTernaryElementwiseOp(
                          resources_, inst, GetOutputShape(inst), tensor_map));
  sequence.add(prog);
  return Status::OK();
}

Status BaseVisitor::HandleSelect(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  TF_ASSIGN_OR_RETURN(poplar::program::Program prog,
                      CreateTernaryElementwiseOp(
                          resources_, inst, GetOutputShape(inst), tensor_map));
  sequence.add(prog);
  return Status::OK();
}

Status BaseVisitor::HandleTupleSelect(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  TF_ASSIGN_OR_RETURN(
      poplar::program::Program prog,
      CreateTupleSelectOp(resources_, inst, GetOutputShape(inst), tensor_map));
  sequence.add(prog);
  return Status::OK();
}

Status BaseVisitor::HandleConcatenate(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleBitcastConvert(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  TF_ASSIGN_OR_RETURN(
      ArgVectors inputs,
      FindInplaceOutputTensors(tensor_map, resources_, inst, sequence));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor out = inputs[0][0];

  TF_ASSIGN_OR_RETURN(poplar::Type type, PoplarDataType(inst->shape()));
  out = out.reinterpret(type);
  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));
  return Status::OK();
}

Status BaseVisitor::HandleDot(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleConvolution(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleAllReduce(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();

  auto reduction = inst->to_apply();
  auto reduction_root = reduction->root_instruction();

  if (reduction_root->opcode() != HloOpcode::kAdd) {
    return xla::FailedPrecondition(
        "Unsupported all-reduce reduction computation.");
  }

  for (auto& reduction_operand : reduction_root->operands()) {
    if (reduction_operand->opcode() != HloOpcode::kParameter) {
      return xla::FailedPrecondition(
          "Unsupported all-reduce reduction computation.");
    }
  }

  TF_ASSIGN_OR_RETURN(
      auto seq, CreateReplicatedAllReduce(resources_, inst,
                                          GetOutputShape(inst), tensor_map));

  sequence.add(seq);
  return Status::OK();
}

Status BaseVisitor::HandleRng(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  if (inst->operand_count() != 2) {
    LOG(FATAL) << "RNG must have two operands.";
  }
  for (auto* op : inst->operands()) {
    if (op->opcode() != HloOpcode::kConstant) {
      LOG(FATAL) << "RNG operand " << op->ToString() << " is not a constant.";
    }
  }
  poplar::program::Program prog;
  switch (inst->random_distribution()) {
    case RandomDistribution::RNG_NORMAL: {
      TF_ASSIGN_OR_RETURN(prog, RandomNormal(resources_, inst,
                                             GetOutputShape(inst), tensor_map));
      break;
    }
    case RandomDistribution::RNG_UNIFORM: {
      TF_ASSIGN_OR_RETURN(
          prog,
          RandomUniform(resources_, inst, GetOutputShape(inst), tensor_map));
      break;
    }
    default: {
      LOG(FATAL) << "Unsupported random distribution type "
                 << inst->random_distribution() << ".";
    }
  }
  sequence.add(prog);
  return Status::OK();
}

Status BaseVisitor::HandleReverse(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleSort(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleConstant(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();

  poplar::Graph& graph = GetGraph(resources_, inst);
  TF_ASSIGN_OR_RETURN(
      poplar::Tensor t,
      AddConstantTensor(graph, std::make_pair(inst, 0), GetOutputShape(inst),
                        inst->literal(), resources_, tensor_map));

  // If this constant is used inplace then we need to add a copy and use that
  // instead so the original constant value is always preserved.
  bool is_inplace_read_write = IsOutputModifiedInplace(inst);
  if (is_inplace_read_write && t.numElements() != 0) {
    VLOG(1) << "Constant tensor is read/write inplace, adding copy";
    poplar::program::Sequence prog;
    poplar::Tensor clone = poputil::duplicate(
        graph, t, prog, GetDebugName(inst) + ".clone",
        poplar::TensorCloneMethod::PRESERVE_ORDER_AND_ALIASES);

    sequence.add(prog);
    t = clone;
  }

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, t));
  return Status::OK();
}

Status BaseVisitor::HandleGetTupleElement(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  TF_ASSIGN_OR_RETURN(
      ArgVectors output_tensors,
      FindInplaceOutputTensors(tensor_map, resources_, inst, sequence, false));
  CHECK_EQ(output_tensors.size(), 1);
  CHECK_EQ(output_tensors[0].size(), CountShapes(inst->shape()));
  for (int64 i = 0; i < output_tensors[0].size(); i++) {
    poplar::Tensor out;
    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, i, output_tensors[0][i]));
  }
  return Status::OK();
}

Status BaseVisitor::HandleReduce(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleBitcast(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleBroadcast(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleReshape(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleTranspose(HloInstruction* inst) {
  return Unimplemented(inst);
}

namespace {
ArgVectors GetFusionInputs(CompilerResources& res, const HloInstruction* inst,
                           TensorMap& tensor_map,
                           poplar::program::Sequence& seq,
                           const bool expand_constants = true) {
  ArgVectors args;
  for (int64 i = 0; i < inst->operand_count(); i++) {
    ArgVector t =
        FindInstructionInputs(tensor_map, res, inst, i, seq, expand_constants);
    args.push_back(t);
  }
  return args;
}
}  // namespace

Status BaseVisitor::HandleFusion(HloInstruction* inst) {
  poplar::program::Program prog;
  HloComputation* comp = inst->fused_instructions_computation();

  if (IsArithmeticExpressionFusion(inst)) {
    ArgVectors args = GetFusionInputs(resources_, inst, tensor_map, sequence);
    ArithmeticExprVisitor arithmetic_visitor(resources_, args);
    TF_RETURN_IF_ERROR(comp->Accept(&arithmetic_visitor));
    prog = arithmetic_visitor.GetSequence();

    for (size_t i = 0; i < arithmetic_visitor.outputs().size(); i++) {
      TF_CHECK_OK(AddOutputTensor(tensor_map, inst, i,
                                  arithmetic_visitor.outputs()[i]));
    }
  } else if (IsPopOpsFusion(inst)) {
    // If is is a special fusion-type op
    VLOG(1) << "Processing " << inst->name()
            << " as Poplibs fusion: " << comp->name();
    auto end = comp->name().find('.');
    std::string name = comp->name().substr(8, end - 8);

    if (custom_call_map.count(name) == 1) {
      TF_ASSIGN_OR_RETURN(
          prog, custom_call_map.at(name)(resources_, inst, GetOutputShape(inst),
                                         tensor_map));
    } else {
      return xla::FailedPrecondition("Unrecognized special call op %s: %s",
                                     inst->name().c_str(), name.c_str());
    }
  } else {
    TF_ASSIGN_OR_RETURN(prog, CreateFusionOp(resources_, inst,
                                             GetOutputShape(inst), tensor_map));
  }

  sequence.add(prog);
  return Status::OK();
};

Status BaseVisitor::HandleCall(HloInstruction* inst) {
  HloComputation* comp = inst->to_apply();
  VLOG(1) << "Processing " << inst->name() << " : " << comp->name();
  TF_ASSIGN_OR_RETURN(
      poplar::program::Program prog,
      CreateCallOp(resources_, inst, GetOutputShape(inst), tensor_map));
  sequence.add(prog);
  return Status::OK();
}

Status BaseVisitor::HandleCustomCall(HloInstruction* inst) {
  TF_ASSIGN_OR_RETURN(
      poplar::program::Program prog,
      CreateCustomCallOp(resources_, inst, GetOutputShape(inst), tensor_map));
  sequence.add(prog);

  return Status::OK();
}

Status BaseVisitor::HandleSlice(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleDynamicSlice(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleDynamicUpdateSlice(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleTuple(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  TF_ASSIGN_OR_RETURN(
      ArgVectors inputs,
      FindInplaceOutputTensors(tensor_map, resources_, inst, sequence));
  CHECK_EQ(inputs.size(), inst->operand_count());
  uint64 n = 0;
  for (uint64 i = 0; i < inputs.size(); i++) {
    CHECK_EQ(inputs[i].size(), CountShapes(inst->operand(i)->shape()));
    for (uint64 j = 0; j < inputs[i].size(); j++) {
      TF_CHECK_OK(AddOutputTensor(tensor_map, inst, n, inputs[i][j]));
      n++;
    }
  }
  return Status::OK();
}

Status BaseVisitor::HandleReduceWindow(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleMap(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  TF_ASSIGN_OR_RETURN(bool simple_parallel,
                      IsParallelMap(inst, inst->to_apply()));
  if (simple_parallel) {
    TF_ASSIGN_OR_RETURN(
        poplar::program::Program prog,
        CreateParallelMap(resources_, inst, GetOutputShape(inst), tensor_map));
    sequence.add(prog);
    return Status::OK();
  }
  return Unimplemented(inst);
}

Status BaseVisitor::HandleSelectAndScatter(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleWhile(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleConditional(HloInstruction* inst) {
  TF_ASSIGN_OR_RETURN(
      poplar::program::Program prog,
      CreateConditionalOp(resources_, inst, GetOutputShape(inst), tensor_map));
  sequence.add(prog);

  return Status::OK();
}

Status BaseVisitor::HandleReal(HloInstruction* inst) {
  VLOG(1) << "Processing " << inst->name();
  TF_ASSIGN_OR_RETURN(
      poplar::Tensor in,
      FindInstructionInput(tensor_map, resources_, inst, 0, sequence));

  poplar::Tensor out = GetGraph(resources_, inst).clone(in);
  sequence.add(poplar::program::Copy(in, out));
  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return Status::OK();
}

Status BaseVisitor::HandlePad(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleReducePrecision(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleInfeed(HloInstruction* inst) {
  return xla::FailedPrecondition(
      "Unsupported use of infeed operation - it's only supported inside of "
      "loops.");
}

Status BaseVisitor::HandleOutfeed(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleSend(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleSendDone(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleRecv(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleRecvDone(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleBatchNormInference(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleBatchNormTraining(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleBatchNormGrad(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleFft(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleGather(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleAfterAll(HloInstruction* inst) {
  // TODO(shauryas) : figure out how to use this for something useful
  return Status::OK();
}

Status BaseVisitor::HandleIota(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleScatter(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleAllToAll(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleCollectivePermute(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleGetDimensionSize(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleAddDependency(HloInstruction* inst) {
  std::vector<std::string> dep_names;
  GetAllDepNames(inst->operand(1), dep_names);

  VLOG(1) << "Processing " << inst->name() << " on "
          << absl::StrJoin(dep_names, ",");
  TF_ASSIGN_OR_RETURN(
      ArgVectors inputs,
      FindInplaceOutputTensors(tensor_map, resources_, inst, sequence, false));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), CountShapes(inst->operand(0)->shape()));
  for (int64 idx = 0; idx < inputs[0].size(); idx++) {
    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, idx, inputs[0][idx]));
  }
  return Status::OK();
}

Status BaseVisitor::HandleReplicaId(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleTriangularSolve(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleCholesky(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandlePartitionId(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleRngGetAndUpdateState(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleCopyStart(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::HandleCopyDone(HloInstruction* inst) {
  return Unimplemented(inst);
}

Status BaseVisitor::Preprocess(HloInstruction* inst) {
  TF_ASSIGN_OR_RETURN(auto poplar_backend_config,
                      inst->backend_config<PoplarBackendConfig>());
  bool new_stochastic_rounding_enabled;
  switch (poplar_backend_config.stochastic_rounding()) {
    case NOT_SET:
      new_stochastic_rounding_enabled =
          resources_.global_floating_point_behaviour.esr();
      break;
    case FORCE_ON:
      new_stochastic_rounding_enabled = true;
      break;
    case FORCE_OFF:
      new_stochastic_rounding_enabled = false;
      break;
    default:
      return InvalidArgument(
          "Invalid value for PoplarBackendConfig.stochastic_rounding()");
  }
  if (new_stochastic_rounding_enabled != stochastic_rounding_enabled_) {
    poplar::setStochasticRounding(GetGraph(resources_, inst), sequence,
                                  new_stochastic_rounding_enabled,
                                  "Preprocess");
    stochastic_rounding_enabled_ = new_stochastic_rounding_enabled;
  }
  return Status::OK();
}

}  // namespace poplarplugin
}  // namespace xla
