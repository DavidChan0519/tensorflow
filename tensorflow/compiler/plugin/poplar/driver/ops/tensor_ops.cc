#include <algorithm>

#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matcher_predicates.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/plugin/poplar/driver/vertex_templates.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/lib/core/errors.h"

#include <poplar/Engine.hpp>
#include <poplar/Graph.hpp>
#include <popops/Cast.hpp>
#include <popops/DynamicSlice.hpp>
#include <popops/Encoding.hpp>
#include <popops/Pad.hpp>
#include <poputil/TileMapping.hpp>

namespace xla {
namespace poplarplugin {
namespace {
bool AreAllDimensionsConstant(const HloDynamicIndexInstruction* inst) {
  for (int64 i = inst->first_index_operand_number(); i < inst->operand_count();
       i++) {
    if (!IsScalarIntegerConstant(inst->operand(i))) {
      return false;
    }
  }
  return true;
}

StatusOr<poplar::program::Program> ConstSliceUpdate(
    CompilerResources& res, const HloDynamicIndexInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::program::Sequence seq;

  TF_ASSIGN_OR_RETURN(ArgVectors inputs,
                      FindInplaceOutputTensors(tensor_map, res, inst, seq));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor input = inputs[0][0];

  TF_ASSIGN_OR_RETURN(poplar::Tensor update,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  std::vector<std::size_t> begin;
  for (int64 i = inst->first_index_operand_number(); i < inst->operand_count();
       i++) {
    TF_ASSIGN_OR_RETURN(int64 index, LiteralScalarToNativeType<int64>(
                                         inst->operand(i)->literal()));
    begin.push_back(index);
  }

  if (begin.size() != input.rank()) {
    return xla::FailedPrecondition("Invalid slice start.");
  }

  std::vector<std::size_t> end = begin;
  for (unsigned int i = 0; i < end.size(); i++) {
    end[i] += update.dim(i);
  }
  poplar::Tensor slice = input.slice(begin, end);
  seq.add(poplar::program::Copy(update, slice));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, input));

  return seq;
}

StatusOr<poplar::program::Program> DynamicSliceUpdate(
    CompilerResources& res, const HloDynamicIndexInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  TF_ASSIGN_OR_RETURN(ArgVectors inputs,
                      FindInplaceOutputTensors(tensor_map, res, inst, seq));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor input = inputs[0][0];

  TF_ASSIGN_OR_RETURN(poplar::Tensor update,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  TF_ASSIGN_OR_RETURN(poplar::Tensor indices,
                      FindInstructionInput(tensor_map, res, inst, 2, seq));

  auto first_index = inst->first_index_operand_number();

  bool multiple_indices = (indices.rank() == 0);

  std::vector<std::size_t> slice_dims;
  std::vector<std::size_t> slice_sizes;
  poplar::Tensor slice_indices;
  for (unsigned d = 0; d < inst->shape().dimensions_size(); d++) {
    poplar::Tensor t;
    if (multiple_indices) {
      TF_ASSIGN_OR_RETURN(
          t, FindInstructionInput(tensor_map, res, inst, first_index + d, seq));
      t = t.reshape({1});
    } else {
      t = indices.index({d}).reshape({1});
    }

    auto type = t.elementType();
    if (type == poplar::INT) {
      t = t.reinterpret(poplar::UNSIGNED_INT);
    }

    bool same_shape = inst->shape().dimensions(d) == update.shape()[d];
    unsigned int index;
    bool zero_index = t.getConstantValue(&index) && (index == 0);

    if (!(same_shape && zero_index)) {
      if (slice_dims.size() == 0) {
        slice_indices = t;
      } else {
        slice_indices = poplar::concat(slice_indices, t);
      }
      slice_dims.push_back(d);
      slice_sizes.push_back(update.shape()[d]);
    }
  }

  if (slice_dims.size() > 0) {
    popops::dynamicUpdate(graph, input, update, slice_indices, slice_dims,
                          slice_sizes, seq, GetDebugName(inst));
  } else {
    seq.add(poplar::program::Copy(update, input));
  }

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, input));

  return seq;
}

StatusOr<poplar::program::Program> ConstSlice(
    CompilerResources& res, const HloDynamicIndexInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  TF_ASSIGN_OR_RETURN(poplar::Tensor input,
                      FindInstructionInput(tensor_map, res, inst, 0, seq));

  std::vector<std::size_t> begin;
  for (int64 i = inst->first_index_operand_number(); i < inst->operand_count();
       i++) {
    TF_ASSIGN_OR_RETURN(int64 index, LiteralScalarToNativeType<int64>(
                                         inst->operand(i)->literal()));
    begin.push_back(index);
  }

  if (begin.size() != input.rank()) {
    return xla::FailedPrecondition("Invalid slice start.");
  }

  std::vector<std::size_t> end = begin;
  for (unsigned int i = 0; i < end.size(); i++) {
    end[i] += output_shape.dimensions(i);
  }

  poplar::Tensor slice = input.slice(begin, end);
  poplar::Tensor out = graph.clone(slice, GetDebugName(inst));

  seq.add(poplar::program::Copy(slice, out));
  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}

StatusOr<poplar::program::Program> DynamicSlice(
    CompilerResources& res, const HloDynamicIndexInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  TF_ASSIGN_OR_RETURN(poplar::Tensor input,
                      FindInstructionInput(tensor_map, res, inst, 0, seq));

  TF_ASSIGN_OR_RETURN(poplar::Tensor indices,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  auto first_index = inst->first_index_operand_number();

  bool multiple_indices = (indices.rank() == 0);

  auto& inst_slice_sizes = inst->dynamic_slice_sizes();
  std::vector<std::size_t> slice_dims;
  std::vector<std::size_t> slice_sizes;
  poplar::Tensor slice_indices;
  for (unsigned d = 0; d < inst->shape().dimensions_size(); d++) {
    poplar::Tensor t;
    if (multiple_indices) {
      TF_ASSIGN_OR_RETURN(
          t, FindInstructionInput(tensor_map, res, inst, first_index + d, seq));
      t = t.reshape({1});
    } else {
      t = indices.index({d}).reshape({1});
    }

    auto type = t.elementType();
    if (type == poplar::INT) {
      t = t.reinterpret(poplar::UNSIGNED_INT);
    }

    bool same_shape = inst_slice_sizes[d] == input.shape()[d];
    unsigned int index;

    bool zero_index = t.getConstantValue(&index) && (index == 0);

    if (!(same_shape && zero_index)) {
      if (slice_dims.size() == 0) {
        slice_indices = t;
      } else {
        slice_indices = poplar::concat(slice_indices, t, 0);
      }
      slice_dims.push_back(d);
      slice_sizes.push_back(inst_slice_sizes[d]);
    }
  }

  // Add the dynamic slice operations to `seq`. This automatically
  // creates the required compute set.
  poplar::Tensor out;

  if (slice_dims.size() > 0) {
    out = popops::dynamicSlice(graph, input, slice_indices, slice_dims,
                               slice_sizes, seq, GetDebugName(inst));
  } else {
    poplar::Tensor copy = graph.clone(input);
    seq.add(poplar::program::Copy(input, copy));
    out = copy;
  }

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}
}  // namespace

StatusOr<poplar::program::Program> CreateDynamicSliceUpdateOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  auto* dynamic_inst = Cast<HloDynamicIndexInstruction>(inst);
  // See if we know the slice dimensions at the compile time.
  if (AreAllDimensionsConstant(dynamic_inst)) {
    VLOG(1) << "Processing " << inst->name() << " as a const slice update.";
    return ConstSliceUpdate(res, dynamic_inst, output_shape, tensor_map);
  } else {
    return DynamicSliceUpdate(res, dynamic_inst, output_shape, tensor_map);
  }
}

StatusOr<poplar::program::Program> CreateDynamicSliceOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  auto* dynamic_inst = Cast<HloDynamicIndexInstruction>(inst);
  // See if we know the slice dimensions at the compile time.
  if (AreAllDimensionsConstant(dynamic_inst)) {
    VLOG(1) << "Processing " << inst->name() << " as a const slice.";
    return ConstSlice(res, dynamic_inst, output_shape, tensor_map);
  } else {
    return DynamicSlice(res, dynamic_inst, output_shape, tensor_map);
  }
}

StatusOr<poplar::program::Program> CreateWideConstant(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::program::Sequence seq;

  poplar::Graph& graph = GetGraph(res, inst);

  const HloInstruction* root =
      inst->fused_instructions_computation()->root_instruction();

  const HloInstruction* constant = root->operand(0);
  CHECK_EQ(constant->opcode(), HloOpcode::kConstant);
  const Literal& constant_literal = constant->literal();

  // Allocate the constant first.
  TF_ASSIGN_OR_RETURN(
      poplar::Tensor constant_tensor,
      AddConstantTensor(graph, std::make_pair(constant, 0), constant->shape(),
                        constant_literal, res, tensor_map));

  // Broadcast the tensor to the right shape.
  TF_ASSIGN_OR_RETURN(poplar::Tensor out,
                      BroadcastTensor(constant_tensor, output_shape, {}));
  // For wide constants, check if they have an allocation target, if so then
  // allocate the tensor with that target and copy the constant to that layout.
  TensorSource src = std::make_pair(inst, 0);
  if (HasTensorAllocationTarget(src, res)) {
    // Doing this copy rather than allocating a big constant and calling
    // setInitialValue is a trade off between having a large tensor always live
    // and a copy + a scalar constant always being live.
    TF_ASSIGN_OR_RETURN(poplar::Tensor layout,
                        AddTensor(graph, src, output_shape, res, tensor_map));
    seq.add(poplar::program::Copy(out, layout));
    out = layout;
  }
  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}

StatusOr<poplar::program::Program> CreateIota(CompilerResources& res,
                                              const HloInstruction* inst,
                                              const xla::Shape& output_shape,
                                              TensorMap& tensor_map) {
  poplar::program::Sequence seq;

  poplar::Graph& graph = GetGraph(res, inst);

  auto iota_inst = DynCast<HloIotaInstruction>(inst);
  const auto iota_dimension = iota_inst->iota_dimension();

  // Get iota length.
  const int64 iota_length = output_shape.dimensions(iota_dimension);
  switch (output_shape.element_type()) {
    case S64: {
      if (!convert_scalar<int32>(iota_length)) {
        return xla::UnimplementedStrCat(
            "Iota - trying to create an iota of length ", iota_length,
            " but only 31-bit integer lengths are supported for signed types.");
      }
    }
    case U64: {
      if (!convert_scalar<uint32>(iota_length)) {
        return xla::UnimplementedStrCat(
            "Iota - trying to create an iota of length ", iota_length,
            " but only 32-bit integer lengths are supported for unsigned "
            "types.");
      }
    }
    default:
      break;
  }

  // Get the iota shape.
  const bool is_signed = ShapeUtil::ElementIsSigned(output_shape);
  auto iota_xla_type = is_signed ? S32 : U32;
  auto iota_shape = ShapeUtil::MakeShape(iota_xla_type, {iota_length});

  auto name = GetDebugName(inst);

  // Create a tensor which stores the iota.
  TF_ASSIGN_OR_RETURN(
      poplar::Tensor iota_tensor,
      AddPlainTensor(graph, name + "/InitialIotaTensor", iota_shape, res));
  // Do the Iota.
  if (is_signed) {
    popops::iota(graph, iota_tensor, 0, seq, name + "/IotaSigned");
  } else {
    popops::iota(graph, iota_tensor, 0U, seq, name + "/IotaUnsigned");
  }
  // Cast it to the right type if the types don't match.
  TF_ASSIGN_OR_RETURN(poplar::Type iota_type, PoplarDataType(iota_shape));
  TF_ASSIGN_OR_RETURN(poplar::Type output_type, PoplarDataType(output_shape));
  poplar::Tensor casted = iota_type != output_type
                              ? popops::cast(graph, iota_tensor, output_type,
                                             seq, name + "/IotaCast")
                              : iota_tensor;

  // Broadcast it to the right shape given the iota dimension.
  TF_ASSIGN_OR_RETURN(poplar::Tensor out,
                      BroadcastTensor(casted, output_shape, {iota_dimension}));
  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}

StatusOr<poplar::program::Program> CreateZeroPadOp(CompilerResources& res,
                                                   const HloInstruction* inst,
                                                   const xla::Shape& output,
                                                   TensorMap& tensor_map) {
  poplar::program::Sequence seq;

  poplar::Graph& graph = GetGraph(res, inst);

  const HloInstruction* root =
      inst->fused_instructions_computation()->root_instruction();
  const PaddingConfig& cfg(root->padding_config());
  TF_ASSIGN_OR_RETURN(poplar::Tensor out,
                      FindInstructionInput(tensor_map, res, inst, 0, seq));

  std::vector<std::ptrdiff_t> paddingLower;
  std::vector<std::ptrdiff_t> paddingUpper;
  for (auto& d : cfg.dimensions()) {
    paddingLower.push_back(d.edge_padding_low());
    paddingUpper.push_back(d.edge_padding_high());
  }
  out = popops::pad(graph, out, paddingLower, paddingUpper);

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));
  return seq;
}

}  // namespace poplarplugin
}  // namespace xla
