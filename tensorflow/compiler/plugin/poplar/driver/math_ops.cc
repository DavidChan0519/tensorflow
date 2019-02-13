#include <algorithm>

#include "tensorflow/compiler/plugin/poplar/driver/classification_predicates.h"
#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/util.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/util/bcast.h"

#include <poplar/Engine.hpp>
#include <poplar/Graph.hpp>

#include <poplin/MatMul.hpp>
#include <popnn/NonLinearity.hpp>
#include <popops/Cast.hpp>
#include <popops/ElementWise.hpp>
#include <popops/ScaledAdd.hpp>

#include "absl/strings/str_cat.h"

using ::absl::StrCat;

namespace xla {
namespace poplarplugin {

static const std::string a_conn("a");
static const std::string b_conn("b");
static const std::string c_conn("c");
static const std::string out_conn("out");

#define POPLAR_OPCODE(O, N) \
  case HloOpcode::O:        \
    return std::string(N)
#define UNUSED_OPCODE(O) \
  case HloOpcode::O:     \
    break;

StatusOr<popops::expr::UnaryOpType> LookupUnaryFn(const HloInstruction* inst) {
  HloOpcode opcode = inst->opcode();
  switch (opcode) {
    case HloOpcode::kAbs:
      return popops::expr::UnaryOpType::ABSOLUTE;
    case HloOpcode::kCeil:
      return popops::expr::UnaryOpType::CEIL;
    case HloOpcode::kClz:
      return popops::expr::UnaryOpType::COUNT_LEADING_ZEROS;
    case HloOpcode::kCos:
      return popops::expr::UnaryOpType::COS;
    case HloOpcode::kExp:
      return popops::expr::UnaryOpType::EXPONENT;
    case HloOpcode::kExpm1:
      return popops::expr::UnaryOpType::EXPONENT_MINUS_ONE;
    case HloOpcode::kFloor:
      return popops::expr::UnaryOpType::FLOOR;
    case HloOpcode::kLog:
      return popops::expr::UnaryOpType::LOGARITHM;
    case HloOpcode::kLog1p:
      return popops::expr::UnaryOpType::LOGARITHM_ONE_PLUS;
    case HloOpcode::kNegate:
      return popops::expr::UnaryOpType::NEGATE;
    case HloOpcode::kRoundNearestAfz:
      return popops::expr::UnaryOpType::ROUND;
    case HloOpcode::kSign:
      return popops::expr::UnaryOpType::SIGNUM;
    case HloOpcode::kSin:
      return popops::expr::UnaryOpType::SIN;
    case HloOpcode::kTanh:
      return popops::expr::UnaryOpType::TANH;
    case HloOpcode::kIsFinite:
      return popops::expr::UnaryOpType::IS_FINITE;
    default:
      break;
  }

  if (opcode == HloOpcode::kNot) {
    if (inst->shape().element_type() == PRED) {
      return popops::expr::UnaryOpType::LOGICAL_NOT;
    } else {
      return popops::expr::UnaryOpType::BITWISE_NOT;
    }
  }

  return tensorflow::errors::Unknown(
      StrCat("[Poplar] Invalid opcode lookup ", HloOpcodeString(opcode)));
}

StatusOr<popops::expr::BinaryOpType> LookupBinaryFn(
    const HloInstruction* inst) {
  HloOpcode opcode = inst->opcode();
  switch (opcode) {
    case HloOpcode::kAdd:
      return popops::expr::BinaryOpType::ADD;
    case HloOpcode::kAtan2:
      return popops::expr::BinaryOpType::ATAN2;
    case HloOpcode::kDivide:
      return popops::expr::BinaryOpType::DIVIDE;
    case HloOpcode::kEq:
      return popops::expr::BinaryOpType::EQUAL;
    case HloOpcode::kGt:
      return popops::expr::BinaryOpType::GREATER_THAN;
    case HloOpcode::kGe:
      return popops::expr::BinaryOpType::GREATER_THAN_EQUAL;
    case HloOpcode::kLt:
      return popops::expr::BinaryOpType::LESS_THAN;
    case HloOpcode::kLe:
      return popops::expr::BinaryOpType::LESS_THAN_EQUAL;
    case HloOpcode::kMaximum:
      return popops::expr::BinaryOpType::MAXIMUM;
    case HloOpcode::kMinimum:
      return popops::expr::BinaryOpType::MINIMUM;
    case HloOpcode::kMultiply:
      return popops::expr::BinaryOpType::MULTIPLY;
    case HloOpcode::kNe:
      return popops::expr::BinaryOpType::NOT_EQUAL;
    case HloOpcode::kPower:
      return popops::expr::BinaryOpType::POWER;
    case HloOpcode::kRemainder:
      return popops::expr::BinaryOpType::REMAINDER;
    case HloOpcode::kShiftLeft:
      return popops::expr::BinaryOpType::SHIFT_LEFT;
    case HloOpcode::kShiftRightArithmetic:
      return popops::expr::BinaryOpType::SHIFT_RIGHT_SIGN_EXTEND;
    case HloOpcode::kShiftRightLogical:
      return popops::expr::BinaryOpType::SHIFT_RIGHT;
    case HloOpcode::kSubtract:
      return popops::expr::BinaryOpType::SUBTRACT;
    default:
      break;
  }

  if (opcode == HloOpcode::kAnd) {
    if (inst->shape().element_type() == PRED) {
      return popops::expr::BinaryOpType::LOGICAL_AND;
    } else {
      return popops::expr::BinaryOpType::BITWISE_AND;
    }
  }

  if (opcode == HloOpcode::kOr) {
    if (inst->shape().element_type() == PRED) {
      return popops::expr::BinaryOpType::LOGICAL_OR;
    } else {
      return popops::expr::BinaryOpType::BITWISE_OR;
    }
  }

  return tensorflow::errors::Unknown(
      StrCat("[Poplar] Invalid opcode lookup ", HloOpcodeString(opcode)));
}

static std::string GetMatMulPass(const HloInstruction* inst,
                                 const CompilerAnnotations& annotations) {
  if (IsForward(inst, annotations)) {
    return "TRAINING_FWD";
  }
  if (IsBackpropInput(inst, annotations)) {
    return "TRAINING_BWD";
  }
  if (IsBackpropFilter(inst, annotations)) {
    return "TRAINING_WU";
  }
  return "INFERENCE_FWD";
}

StatusOr<poplar::program::Program> CreateUnaryElementwiseOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  poplar::Tensor in;
  TF_ASSIGN_OR_RETURN(in, FindInstructionInput(tensor_map, res, inst, 0, seq));

  popops::expr::UnaryOpType op;
  TF_ASSIGN_OR_RETURN(op, LookupUnaryFn(inst));

  poplar::Tensor out = popops::map(graph, op, in, seq, GetDebugName(inst));

  TF_ASSIGN_OR_RETURN(out, BroadcastTensor(out, output_shape));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}

StatusOr<poplar::program::Program> CreateBinaryElementwiseOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  poplar::Tensor in0;
  TF_ASSIGN_OR_RETURN(
      in0, FindInstructionInput(tensor_map, res, inst, 0, seq, false));

  poplar::Tensor in1;
  TF_ASSIGN_OR_RETURN(
      in1, FindInstructionInput(tensor_map, res, inst, 1, seq, false));

  if (res.annotations.inplace_instructions.count(inst) &&
      (in0.shape() == in1.shape())) {
    TF_ASSIGN_OR_RETURN(
        ArgVectors inputs,
        GetInplaceOutputTensors(tensor_map, res, inst, seq, false));
    CHECK_EQ(inputs.size(), 1);
    CHECK_EQ(inputs[0].size(), 1);
    poplar::Tensor in0 = inputs[0][0];

    // Call the inplace op
    switch (inst->opcode()) {
      case HloOpcode::kAdd: {
        popops::scaledAddTo(graph, in0, in1, 1.0f, seq, GetDebugName(inst));
        break;
      }
      case HloOpcode::kSubtract: {
        popops::scaledSubtractFrom(graph, in0, in1, 1.0f, seq,
                                   GetDebugName(inst));
        break;
      }
      default: {
        popops::expr::BinaryOpType op;
        TF_ASSIGN_OR_RETURN(op, LookupBinaryFn(inst));
        popops::mapInPlace(graph, op, in0, in1, seq, GetDebugName(inst));
        break;
      }
    }

    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, in0));

    return seq;

  } else {
    if (in0.shape() != in1.shape()) {
      auto shape0_optional = convert_array<tensorflow::BCast::Vec>(in0.shape());
      auto shape1_optional = convert_array<tensorflow::BCast::Vec>(in1.shape());
      if (!shape0_optional || !shape1_optional) {
        return xla::FailedPrecondition(
            "ExpressionOutliner - cannot cast input shape.");
      }
      tensorflow::BCast::Vec shape0 = *shape0_optional;
      tensorflow::BCast::Vec shape1 = *shape1_optional;

      tensorflow::BCast bcast(shape0, shape1);
      if (!bcast.IsValid()) {
        return xla::FailedPrecondition("Incompatible broadcast on %s",
                                       inst->name().c_str());
      }

      auto bcast_x_shape_optional =
          convert_array<std::vector<size_t>>(bcast.x_reshape());
      auto bcast_y_shape_optional =
          convert_array<std::vector<size_t>>(bcast.y_reshape());
      if (!bcast_x_shape_optional || !bcast_y_shape_optional) {
        return xla::FailedPrecondition(
            "ExpressionOutliner - cannot cast broadcast shape.");
      }
      std::vector<size_t> bcast_x_shape = *bcast_x_shape_optional;
      std::vector<size_t> bcast_y_shape = *bcast_y_shape_optional;
      in0 = in0.reshape(bcast_x_shape);
      in1 = in1.reshape(bcast_y_shape);

      in0 = TileTensor(bcast.x_bcast(), in0);
      in1 = TileTensor(bcast.y_bcast(), in1);
    }

    poplar::Tensor out;

    if (inst->opcode() == HloOpcode::kXor) {
      if (inst->shape().element_type() == PRED) {
        poplar::Tensor or_out =
            popops::map(graph, popops::expr::BinaryOpType::LOGICAL_OR, in0, in1,
                        seq, GetDebugName(inst));
        poplar::Tensor and_out =
            popops::map(graph, popops::expr::BinaryOpType::LOGICAL_AND, in0,
                        in1, seq, GetDebugName(inst));
        poplar::Tensor not_out =
            popops::map(graph, popops::expr::UnaryOpType::LOGICAL_NOT, and_out,
                        seq, GetDebugName(inst));
        out = popops::map(graph, popops::expr::BinaryOpType::LOGICAL_AND,
                          or_out, not_out, seq, GetDebugName(inst));
      } else {
        poplar::Tensor or_out =
            popops::map(graph, popops::expr::BinaryOpType::BITWISE_OR, in0, in1,
                        seq, GetDebugName(inst));
        poplar::Tensor and_out =
            popops::map(graph, popops::expr::BinaryOpType::BITWISE_AND, in0,
                        in1, seq, GetDebugName(inst));
        poplar::Tensor not_out =
            popops::map(graph, popops::expr::UnaryOpType::BITWISE_NOT, and_out,
                        seq, GetDebugName(inst));
        out = popops::map(graph, popops::expr::BinaryOpType::BITWISE_AND,
                          or_out, not_out, seq, GetDebugName(inst));
      }
    } else {
      popops::expr::BinaryOpType op;
      TF_ASSIGN_OR_RETURN(op, LookupBinaryFn(inst));

      out = popops::map(graph, op, in0, in1, seq, GetDebugName(inst));
    }

    // Occasionally, due to an interplay of implicit broadcasting and
    // arithmetic re-arrangement, the output of an op is larger than the inputs
    // generate
    if (ShapeUtil::ElementsIn(output_shape) != out.numElements()) {
      TF_ASSIGN_OR_RETURN(out, BroadcastTensor(out, output_shape));
    }

    out = out.reshape(PoplarShapeFromXlaShape(output_shape));

    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

    return seq;
  }
}

namespace {
template <typename T>
Status DoScaledInplaceConstantOrTensor(poplar::Graph& graph,
                                       poplar::Tensor& lhs, poplar::Tensor& rhs,
                                       T scale, poplar::program::Sequence& prog,
                                       const HloOpcode op_type,
                                       const std::string& name) {
  // Call the inplace op
  switch (op_type) {
    case HloOpcode::kAdd: {
      popops::scaledAddTo(graph, lhs, rhs, scale, prog, name);
      break;
    }
    case HloOpcode::kSubtract: {
      popops::scaledSubtractFrom(graph, lhs, rhs, scale, prog, name);
      break;
    }
    default: {
      return xla::FailedPrecondition("Unsupported scaled inplace op: %s",
                                     name.c_str());
    }
  }
  return Status::OK();
}
}  // namespace

Status ScaledInplaceConstantOrTensor(poplar::Graph& graph, poplar::Tensor& lhs,
                                     poplar::Tensor& rhs, const double scale,
                                     poplar::program::Sequence& prog,
                                     const HloOpcode op_type,
                                     const std::string& name) {
  return DoScaledInplaceConstantOrTensor(graph, lhs, rhs, scale, prog, op_type,
                                         name);
}

Status ScaledInplaceConstantOrTensor(poplar::Graph& graph, poplar::Tensor& lhs,
                                     poplar::Tensor& rhs, poplar::Tensor& scale,
                                     poplar::program::Sequence& prog,
                                     const HloOpcode op_type,
                                     const std::string& name) {
  return DoScaledInplaceConstantOrTensor(graph, lhs, rhs, scale, prog, op_type,
                                         name);
}

StatusOr<poplar::program::Program> CreateScaledInplace(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;
  TF_ASSIGN_OR_RETURN(
      ArgVectors inputs,
      GetInplaceOutputTensors(tensor_map, res, inst, seq, false));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor in0 = inputs[0][0];

  poplar::Tensor in1;
  TF_ASSIGN_OR_RETURN(
      in1, FindInstructionInput(tensor_map, res, inst, 1, seq, false));

  const auto* root_inst =
      inst->fused_instructions_computation()->root_instruction();

  if (inst->operand_count() == 2) {
    const auto* const_inst = root_inst->operand(1)->operand(1)->operand(0);
    CHECK_EQ(const_inst->opcode(), HloOpcode::kConstant);
    // Get the scalar multiplier
    TF_ASSIGN_OR_RETURN(
        double scale, LiteralScalarToNativeType<double>(const_inst->literal()));

    TF_CHECK_OK(ScaledInplaceConstantOrTensor(
        graph, in0, in1, scale, seq, root_inst->opcode(), GetDebugName(inst)));
  } else if (inst->operand_count() == 3) {
    TF_ASSIGN_OR_RETURN(
        poplar::Tensor scale,
        FindInstructionInput(tensor_map, res, inst, 2, seq, false));
    TF_CHECK_OK(ScaledInplaceConstantOrTensor(
        graph, in0, in1, scale, seq, root_inst->opcode(), GetDebugName(inst)));
  } else {
    return xla::FailedPrecondition("Unsupported use of scaled inplace op: %s",
                                   root_inst->name().c_str());
  }

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, in0));
  return seq;
}

StatusOr<poplar::program::Program> CreateMatMulForDotOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  CHECK_EQ(inst->opcode(), HloOpcode::kDot);
  poplar::Tensor in0;
  TF_ASSIGN_OR_RETURN(in0, FindInstructionInput(tensor_map, res, inst, 0, seq));

  poplar::Tensor in1;
  TF_ASSIGN_OR_RETURN(in1, FindInstructionInput(tensor_map, res, inst, 1, seq));

  poplar::Tensor out;

  if (in0.rank() > 2 || in1.rank() > 2) {
    return xla::FailedPrecondition("Unsupported Dot operation on %s",
                                   inst->name().c_str());
  }

  const DotDimensionNumbers& dot_dims = inst->dot_dimension_numbers();
  if (dot_dims.lhs_contracting_dimensions_size() != 1 ||
      dot_dims.rhs_contracting_dimensions_size() != 1) {
    return xla::FailedPrecondition(
        "Unsupported Dot with multiple contracting dimensions on %s",
        inst->name().c_str());
  }

  int64 lhs_reduction_dimension = dot_dims.lhs_contracting_dimensions(0);
  int64 rhs_reduction_dimension = dot_dims.rhs_contracting_dimensions(0);

  if (in0.rank() == 1) {
    in0 = in0.reshape({1, in0.dim(0)});
    // force the reduction dimension due to reshaping
    lhs_reduction_dimension = 1;
  }

  if (in1.rank() == 1) {
    in1 = in1.reshape({in1.dim(0), 1});
    // force the reduction dimension due to reshaping
    rhs_reduction_dimension = 0;
  }

  if (lhs_reduction_dimension != 1) {
    in0 = in0.transpose();
  }

  if (rhs_reduction_dimension != 0) {
    in1 = in1.transpose();
  }

  poplar::OptionFlags opts;
  opts.set("fullyConnectedPass", GetMatMulPass(inst, res.annotations));

  out = poplin::matMul(graph, in0, in1, seq, GetDebugName(inst), opts,
                       &res.dot_cache);

  out = out.reshape(PoplarShapeFromXlaShape(output_shape));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}

StatusOr<poplar::program::Program> CreateMatMulBiasAddOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence prog;

  TF_ASSIGN_OR_RETURN(
      ArgVectors inputs,
      GetInplaceOutputTensors(tensor_map, res, inst, prog, false));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor in = inputs[0][0];

  poplar::Tensor bias;
  TF_ASSIGN_OR_RETURN(
      bias, FindInstructionInput(tensor_map, res, inst, 1, prog, false));

  poplin::addBias(graph, in, bias, prog, GetDebugName(inst));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, in));
  return prog;
}

StatusOr<poplar::program::Program> CreateSelectOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  poplar::Tensor pred;
  TF_ASSIGN_OR_RETURN(
      pred, FindInstructionInput(tensor_map, res, inst, 0, seq, false));

  ArgVector in0 = FindInstructionInputs(tensor_map, res, inst, 1, seq, false);
  ArgVector in1 = FindInstructionInputs(tensor_map, res, inst, 2, seq, false);

  if (in0.size() != in1.size()) {
    return xla::FailedPrecondition("Mismatching tuple sizes on %s",
                                   inst->name().c_str());
  }

  for (unsigned int i = 0; i < in0.size(); i++) {
    poplar::Tensor p = pred;
    poplar::Tensor i0 = in0[i];
    poplar::Tensor i1 = in1[i];

    if (p.numElements() == 1) {
      p = p.reshape({1});
      p = p.broadcast(i0.numElements(), 0);
      p = p.reshape(i0.shape());
    }

    poplar::Tensor out = popops::map(graph, popops::expr::TernaryOpType::SELECT,
                                     i0, i1, p, seq, GetDebugName(inst));

    TF_CHECK_OK(AddOutputTensor(tensor_map, inst, i, out));
  }

  return seq;
}

StatusOr<poplar::program::Program> CreateCastOp(CompilerResources& res,
                                                const HloInstruction* inst,
                                                const xla::Shape& output_shape,
                                                TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  poplar::Tensor in;
  TF_ASSIGN_OR_RETURN(in, FindInstructionInput(tensor_map, res, inst, 0, seq));

  poplar::Type poplar_type;
  TF_ASSIGN_OR_RETURN(poplar_type, PoplarDataType(output_shape));

  poplar::Tensor out =
      popops::cast(graph, in, poplar_type, seq, GetDebugName(inst));

  TF_ASSIGN_OR_RETURN(out, BroadcastTensor(out, output_shape));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}

StatusOr<poplar::program::Program> CreateClampOp(CompilerResources& res,
                                                 const HloInstruction* inst,
                                                 const xla::Shape& output_shape,
                                                 TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  poplar::Tensor min;
  TF_ASSIGN_OR_RETURN(
      min, FindInstructionInput(tensor_map, res, inst, 0, seq, false));
  if (!PoplarShapeMatchesXLAShape(min, output_shape)) {
    TF_ASSIGN_OR_RETURN(min, BroadcastTensor(min, output_shape));
  }

  poplar::Tensor arg;
  TF_ASSIGN_OR_RETURN(
      arg, FindInstructionInput(tensor_map, res, inst, 1, seq, false));
  if (!PoplarShapeMatchesXLAShape(arg, output_shape)) {
    TF_ASSIGN_OR_RETURN(arg, BroadcastTensor(arg, output_shape));
  }

  poplar::Tensor max;
  TF_ASSIGN_OR_RETURN(
      max, FindInstructionInput(tensor_map, res, inst, 2, seq, false));
  if (!PoplarShapeMatchesXLAShape(max, output_shape)) {
    TF_ASSIGN_OR_RETURN(max, BroadcastTensor(max, output_shape));
  }

  poplar::Tensor out = popops::map(graph, popops::expr::TernaryOpType::CLAMP,
                                   arg, min, max, seq, GetDebugName(inst));

  TF_ASSIGN_OR_RETURN(out, BroadcastTensor(out, output_shape));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}

StatusOr<poplar::program::Program> CreateReluOp(CompilerResources& res,
                                                const HloInstruction* inst,
                                                const xla::Shape& output_shape,
                                                TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;
  TF_ASSIGN_OR_RETURN(ArgVectors inputs,
                      GetInplaceOutputTensors(tensor_map, res, inst, seq));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor t = inputs[0][0];
  popnn::reluInPlace(graph, t, seq, GetDebugName(inst));

  TF_ASSIGN_OR_RETURN(t, BroadcastTensor(t, output_shape));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, t));

  return seq;
}

StatusOr<poplar::program::Program> CreateReluGradOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  poplar::Tensor out;
  TF_ASSIGN_OR_RETURN(out, FindInstructionInput(tensor_map, res, inst, 0, seq));

  poplar::Tensor outgrad;
  TF_ASSIGN_OR_RETURN(outgrad,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  poplar::Tensor t =
      popnn::nonLinearityInputGradient(graph, popnn::NonLinearityType::RELU,
                                       out, outgrad, seq, GetDebugName(inst));

  TF_ASSIGN_OR_RETURN(t, BroadcastTensor(t, output_shape));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, t));

  return seq;
}

StatusOr<poplar::program::Program> CreateSigmoidOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;
  TF_ASSIGN_OR_RETURN(ArgVectors inputs,
                      GetInplaceOutputTensors(tensor_map, res, inst, seq));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor t = inputs[0][0];

  popnn::sigmoidInPlace(graph, t, seq, GetDebugName(inst));

  TF_ASSIGN_OR_RETURN(t, BroadcastTensor(t, output_shape));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, t));

  return seq;
}

StatusOr<poplar::program::Program> CreateSigmoidGradOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  poplar::Tensor out;
  TF_ASSIGN_OR_RETURN(out, FindInstructionInput(tensor_map, res, inst, 0, seq));

  poplar::Tensor outgrad;
  TF_ASSIGN_OR_RETURN(outgrad,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  poplar::Tensor t =
      popnn::nonLinearityInputGradient(graph, popnn::NonLinearityType::SIGMOID,
                                       out, outgrad, seq, GetDebugName(inst));

  TF_ASSIGN_OR_RETURN(t, BroadcastTensor(t, output_shape));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, t));

  return seq;
}

}  // namespace poplarplugin
}  // namespace xla
