#include <algorithm>

#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/util.h"
#include "tensorflow/compiler/plugin/poplar/driver/vertex_templates.h"
#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/lib/core/errors.h"

#include <poplar/Engine.hpp>
#include <poplar/Graph.hpp>
#include <popops/DynamicSlice.hpp>
#include <popops/Pad.hpp>

namespace xla {
namespace poplarplugin {

StatusOr<poplar::program::Program> CreateSliceUpdateOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  poplar::Tensor input;
  TF_ASSIGN_OR_RETURN(input,
                      FindInstructionInput(tensor_map, res, inst, 0, seq));
  poplar::Tensor update;
  TF_ASSIGN_OR_RETURN(update,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  const HloInstruction* root = inst->to_apply()->root_instruction();

  std::vector<int64> begin;
  if (root->operand(2)->opcode() == HloOpcode::kConstant) {
    TF_ASSIGN_OR_RETURN(
        begin, LiteralVectorToInt64Vector(root->operand(2)->literal()));
  } else {
    const HloInstruction* bcast = root->operand(2);
    const HloInstruction* constant = bcast->operand(0);
    TF_ASSIGN_OR_RETURN(begin, WideConstToInt64Vector(bcast, constant));
  }

  if (begin.size() != input.rank()) {
    return xla::FailedPrecondition("Invalid update slice start");
  }

  poplar::Tensor copy;

  if (!input.isParallelWriteable()) {
    TF_ASSIGN_OR_RETURN(
        copy, AddTensor(graph, std::make_pair(inst, 0),
                        XlaShapeFromPoplarShape(output_shape.element_type(),
                                                input.shape()),
                        res, tensor_map));
    seq.add(poplar::program::Copy(input, copy));
    input = copy;
  } else {
    copy = graph.clone(input);
    seq.add(poplar::program::Copy(input, copy));
  }

  std::vector<std::size_t> s_begin =
      convert_array<std::vector<std::size_t>>(begin);
  std::vector<std::size_t> s_end = s_begin;
  for (unsigned int i = 0; i < s_end.size(); i++) {
    s_end[i] += update.dim(i);
  }
  poplar::Tensor slice = copy.slice(s_begin, s_end);
  seq.add(poplar::program::Copy(update, slice));

  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, copy));

  return seq;
}

StatusOr<poplar::program::Program> CreateSliceOp(CompilerResources& res,
                                                 const HloInstruction* inst,
                                                 const xla::Shape& output_shape,
                                                 TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  poplar::Tensor input;
  TF_ASSIGN_OR_RETURN(input,
                      FindInstructionInput(tensor_map, res, inst, 0, seq));

  const HloInstruction* root = inst->to_apply()->root_instruction();

  std::vector<int64> begin;
  if (root->operand(1)->opcode() == HloOpcode::kConstant) {
    TF_ASSIGN_OR_RETURN(
        begin, LiteralVectorToInt64Vector(root->operand(1)->literal()));
  } else {
    const HloInstruction* bcast = root->operand(1);
    const HloInstruction* constant = bcast->operand(0);
    TF_ASSIGN_OR_RETURN(begin, WideConstToInt64Vector(bcast, constant));
  }

  if (begin.size() != input.rank()) {
    return xla::FailedPrecondition("Invalid update slice start");
  }

  std::vector<std::size_t> s_begin =
      convert_array<std::vector<std::size_t>>(begin);
  std::vector<std::size_t> s_end = s_begin;
  for (unsigned int i = 0; i < s_end.size(); i++) {
    s_end[i] += output_shape.dimensions(i);
  }

  poplar::Tensor slice = input.slice(s_begin, s_end);
  poplar::Tensor out = graph.clone(slice, GetDebugName(inst));

  seq.add(poplar::program::Copy(slice, out));
  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}

StatusOr<poplar::program::Program> CreateDynamicSliceUpdateOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  TF_ASSIGN_OR_RETURN(ArgVectors inputs,
                      GetInplaceOutputTensors(tensor_map, res, inst, seq));
  CHECK_EQ(inputs.size(), 1);
  CHECK_EQ(inputs[0].size(), 1);
  poplar::Tensor input = inputs[0][0];

  poplar::Tensor update;
  TF_ASSIGN_OR_RETURN(update,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  poplar::Tensor indices;
  TF_ASSIGN_OR_RETURN(indices,
                      FindInstructionInput(tensor_map, res, inst, 2, seq));

  auto type = indices.elementType();
  if (type == poplar::INT) {
    indices = indices.reinterpret(poplar::UNSIGNED_INT);
  }

  std::vector<std::size_t> slice_dims;
  std::vector<std::size_t> slice_sizes;
  poplar::Tensor slice_indices;
  for (unsigned d = 0; d < inst->shape().dimensions_size(); d++) {
    auto t = indices.index({d}).reshape({1});
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

StatusOr<poplar::program::Program> CreateDynamicSliceOp(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::Graph& graph = GetGraph(res, inst);

  poplar::program::Sequence seq;

  poplar::Tensor input;
  TF_ASSIGN_OR_RETURN(input,
                      FindInstructionInput(tensor_map, res, inst, 0, seq));

  poplar::Tensor indices;
  TF_ASSIGN_OR_RETURN(indices,
                      FindInstructionInput(tensor_map, res, inst, 1, seq));

  auto type = indices.elementType();
  if (type == poplar::INT) {
    indices = indices.reinterpret(poplar::UNSIGNED_INT);
  }

  std::vector<std::size_t> slice_dims;
  std::vector<std::size_t> slice_sizes;
  poplar::Tensor slice_indices;
  for (unsigned d = 0; d < inst->shape().dimensions_size(); d++) {
    auto t = indices.index({d}).reshape({1});
    bool same_shape = inst->shape().dimensions(d) == input.shape()[d];
    unsigned int index;
    bool zero_index = t.getConstantValue(&index) && (index == 0);

    if (!(same_shape && zero_index)) {
      if (slice_dims.size() == 0) {
        slice_indices = t;
      } else {
        slice_indices = poplar::concat(slice_indices, t, 0);
      }
      slice_dims.push_back(d);
      slice_sizes.push_back(inst->shape().dimensions(d));
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

StatusOr<poplar::program::Program> CreateWideConstant(
    CompilerResources& res, const HloInstruction* inst,
    const xla::Shape& output_shape, TensorMap& tensor_map) {
  poplar::program::Sequence seq;

  poplar::Graph& graph = GetGraph(res, inst);

  const HloInstruction* root = inst->to_apply()->root_instruction();
  poplar::Tensor out;
  TF_ASSIGN_OR_RETURN(
      out, AddConstantTensor(graph, std::make_pair(inst, 0), inst->shape(),
                             root->operand(0)->literal(), res, tensor_map));
  TF_CHECK_OK(AddOutputTensor(tensor_map, inst, 0, out));

  return seq;
}

StatusOr<poplar::program::Program> CreateZeroPadOp(CompilerResources& res,
                                                   const HloInstruction* inst,
                                                   const xla::Shape& output,
                                                   TensorMap& tensor_map) {
  poplar::program::Sequence seq;

  poplar::Graph& graph = GetGraph(res, inst);

  const HloInstruction* root = inst->to_apply()->root_instruction();
  const PaddingConfig& cfg(root->padding_config());
  poplar::Tensor out;
  TF_ASSIGN_OR_RETURN(out, FindInstructionInput(tensor_map, res, inst, 0, seq));

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
