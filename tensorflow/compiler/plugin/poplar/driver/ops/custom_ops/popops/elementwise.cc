/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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
#include "tensorflow/compiler/plugin/poplar/driver/ops/custom_ops/poplar_ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/custom_ops/popops/expression_helpers.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"

#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/conv_util.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/core/lib/core/errors.h"

#include <poplar/Graph.hpp>
#include <popops/Cast.hpp>
#include <popops/ElementWise.hpp>

namespace xla {
namespace poplarplugin {

class UnaryElementwiseOp : public PoplarOpDef {
  StatusOr<poplar::program::Program> Creator(poplar::Graph& graph,
                                             CompilerResources& res,
                                             const HloInstruction* inst,
                                             const xla::Shape& output_shape,
                                             TensorMap& tensor_map) override {
    poplar::program::Sequence seq;
    TF_ASSIGN_OR_RETURN(
        auto expression_inputs,
        helper::GetElementwiseInputs(res, inst, tensor_map, seq));
    auto input_tensors =
        helper::GetTensorsFromExpressionInputs(expression_inputs);

    auto operation = helper::GetElementwiseOp(inst);
    TF_ASSIGN_OR_RETURN(popops::expr::UnaryOpType op, LookupUnaryFn(operation));
    auto expr = popops::expr::UnaryOp(op, *expression_inputs[0].expr);

    poplar::Tensor out;
    const bool is_inplace =
        AreInplaceOutputTensorsWritable(tensor_map, res, inst);
    if (is_inplace) {
      popops::mapInPlace(graph, expr, input_tensors, seq, GetDebugName(inst));
      out = input_tensors[0];
    } else {
      out = popops::map(graph, expr, input_tensors, seq, GetDebugName(inst));
    }

    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

    return seq;
  }
};

REGISTER_POPLAR_OP(UnaryOp, UnaryElementwiseOp);

class BinaryElementwiseOp : public PoplarOpDef {
  StatusOr<poplar::program::Program> Creator(poplar::Graph& graph,
                                             CompilerResources& res,
                                             const HloInstruction* inst,
                                             const xla::Shape& output_shape,
                                             TensorMap& tensor_map) override {
    poplar::program::Sequence seq;
    TF_ASSIGN_OR_RETURN(
        auto expression_inputs,
        helper::GetElementwiseInputs(res, inst, tensor_map, seq));
    auto input_tensors =
        helper::GetTensorsFromExpressionInputs(expression_inputs);

    auto operation = helper::GetElementwiseOp(inst);
    TF_ASSIGN_OR_RETURN(popops::expr::BinaryOpType op,
                        LookupBinaryFn(operation));
    auto expr = popops::expr::BinaryOp(op, *expression_inputs[0].expr,
                                       *expression_inputs[1].expr);

    poplar::Tensor out;
    const bool is_inplace =
        AreInplaceOutputTensorsWritable(tensor_map, res, inst);
    if (is_inplace) {
      switch (operation->opcode()) {
        case HloOpcode::kAdd:
        case HloOpcode::kSubtract: {
          // Specialize for add and subtract when all inputs are tensors.
          const bool all_inputs_tensors = input_tensors.size() == 2;
          if (all_inputs_tensors) {
            ScaledInplaceConstantOrTensor(
                graph, input_tensors[0], input_tensors[1], 1.0f, seq,
                operation->opcode(), GetDebugName(inst));
            break;
          }
          // Fall through if we can't specialize.
        }
        default: {
          popops::mapInPlace(graph, expr, input_tensors, seq,
                             GetDebugName(inst));
          break;
        }
      }
      out = input_tensors[0];
    } else {
      out = popops::map(graph, expr, input_tensors, seq, GetDebugName(inst));
    }

    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));
    return seq;
  }
};

REGISTER_POPLAR_OP(Implicit_binary_inplace, BinaryElementwiseOp);
REGISTER_POPLAR_OP(Implicit_binary, BinaryElementwiseOp);

class TernaryElementwiseOp : public PoplarOpDef {
  StatusOr<poplar::program::Program> Creator(poplar::Graph& graph,
                                             CompilerResources& res,
                                             const HloInstruction* inst,
                                             const xla::Shape& output_shape,
                                             TensorMap& tensor_map) override {
    // Non of the ternary ops currently support in-placing.
    const bool is_inplace =
        AreInplaceOutputTensorsWritable(tensor_map, res, inst);
    CHECK(!is_inplace);

    poplar::program::Sequence seq;
    TF_ASSIGN_OR_RETURN(
        auto expression_inputs,
        helper::GetElementwiseInputs(res, inst, tensor_map, seq));
    auto input_tensors =
        helper::GetTensorsFromExpressionInputs(expression_inputs);

    // Get the actual ternary operation.
    auto operation = helper::GetElementwiseOp(inst);

    // Create the expression depending on the operation.
    std::unique_ptr<popops::expr::TernaryOp> expr;
    switch (operation->opcode()) {
      case HloOpcode::kClamp: {
        expr = absl::make_unique<popops::expr::TernaryOp>(
            popops::expr::TernaryOpType::CLAMP, *expression_inputs[1].expr,
            *expression_inputs[0].expr, *expression_inputs[2].expr);
        break;
      }
      case HloOpcode::kSelect: {
        expr = absl::make_unique<popops::expr::TernaryOp>(
            popops::expr::TernaryOpType::SELECT, *expression_inputs[1].expr,
            *expression_inputs[2].expr, *expression_inputs[0].expr);
        break;
      }
      default: {
        return xla::FailedPrecondition(
            "Trying to process %s as a ternary operation.",
            operation->ToString().c_str());
      }
    }

    poplar::Tensor out =
        popops::map(graph, *expr, input_tensors, seq, GetDebugName(inst));

    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));
    return seq;
  }
};

REGISTER_POPLAR_OP(Implicit_ternary_inplace, TernaryElementwiseOp);
REGISTER_POPLAR_OP(Implicit_ternary, TernaryElementwiseOp);

}  // namespace poplarplugin
}  // namespace xla
