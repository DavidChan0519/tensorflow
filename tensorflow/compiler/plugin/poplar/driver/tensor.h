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
#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TENSOR_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_TENSOR_H_

#include "tensorflow/compiler/plugin/poplar/driver/passes/allocation_finder.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/poplar_util.h"

#include "tensorflow/compiler/xla/literal_util.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"
#include "tensorflow/core/platform/types.h"

#include <poplar/TensorCloneMethod.hpp>
#include <popops/DynamicSlice.hpp>

namespace poplar {
class Tensor;
class Graph;
class Type;
}  // namespace poplar

namespace xla {
namespace poplarplugin {

struct CompilerResources;

StatusOr<poplar::Type> PoplarDataType(const xla::PrimitiveType& element_type);

StatusOr<poplar::Type> PoplarDataType(const xla::Shape& shape);

std::vector<size_t> PoplarShapeFromXlaShape(const xla::Shape& xla_shape);

xla::Shape XlaShapeFromPoplarShape(PrimitiveType element_type,
                                   const std::vector<size_t>& poplar_shape);

poplar::Tensor ConvertToDeviceLayout(const Shape& shape,
                                     const poplar::Tensor& tensor);

poplar::Tensor ConvertFromDeviceLayout(const Shape& shape,
                                       const poplar::Tensor& tensor);

bool PoplarShapeMatchesXLAShape(const poplar::Tensor& tensor,
                                const xla::Shape& shape);

// Concatenate all tensors into a single tensor.
poplar::Tensor FlattenAndConcatenateTensors(
    const std::vector<poplar::Tensor>& tensors);

StatusOr<poplar::Tensor> SliceTensor(
    poplar::Tensor tensor_to_slice,
    const HloInstruction::InstructionVector& slices, int64 slice_index);

// Slice tensor into tensors with shapes like the tensors.
std::vector<poplar::Tensor> SliceTensorIntoTensorsLike(
    poplar::Tensor tensor_to_slice,
    const std::vector<poplar::Tensor>& like_tensors);

StatusOr<poplar::Tensor> AddDynamicSliceTensor(
    poplar::Graph& graph, const std::string& debug_name,
    const xla::Shape& shape_xla, const xla::Shape& slice_shape_xla);

StatusOr<poplar::Tensor> AddDynamicUpdateSliceTensor(
    poplar::Graph& graph, const std::string& debug_name,
    const xla::Shape& input_shape_xla, const xla::Shape& update_shape_xla);

StatusOr<poplar::Tensor> AddDynamicSliceTensor(
    poplar::Graph& graph, const std::string& debug_name,
    const xla::Shape& shape_xla, const xla::Shape& slice_shape_xla,
    poplar::Tensor& physical_layout);

StatusOr<poplar::Tensor> AddScatterTensor(poplar::Graph& graph,
                                          const std::string& debug_name,
                                          const xla::Shape& shape_xla,
                                          const xla::Shape& slice_shape_xla);

StatusOr<poplar::Tensor> AddGatherTensor(poplar::Graph& graph,
                                         const std::string& debug_name,
                                         const xla::Shape& shape_xla,
                                         std::vector<std::size_t> slice_sizes,
                                         std::vector<unsigned> start_index_map);

StatusOr<poplar::Tensor> AddPlainTensor(poplar::Graph& graph,
                                        const std::string& debug_name,
                                        const xla::Shape& shape,
                                        CompilerResources& resources,
                                        bool offset = true);

StatusOr<poplar::Tensor> AddNormScaleTensor(
    poplar::Graph& graph, const std::string& debug_name,
    const HloInstruction* layout, uint64 layout_output_idx,
    const unsigned feature_dimension,
    std::vector<const HloInstruction*> forward_path,
    const TensorMap& tensor_map);

StatusOr<poplar::Tensor> AddNormOffsetTensor(
    poplar::Graph& graph, const std::string& debug_name,
    const HloInstruction* layout, uint64 layout_output_idx,
    const unsigned feature_dimension,
    std::vector<const HloInstruction*> forward_path,
    const TensorMap& tensor_map);

StatusOr<poplar::Tensor> CreateIndicesTensor(
    poplar::Graph& graph, const popops::SlicePlan& plan,
    const xla::Shape& xla_indices_shape, const std::string& name);

// Returns true if the given tensor source has a special layout allocation
// target.
bool HasTensorAllocationTarget(const TensorSource& src,
                               const CompilerResources& resources);

StatusOr<poplar::Tensor> AddTensorForTarget(poplar::Graph& graph,
                                            const TensorTarget& tensor_target,
                                            const xla::Shape& shape,
                                            CompilerResources& resources,
                                            const TensorMap& tensor_map,
                                            const std::string& debug_name);

StatusOr<poplar::Tensor> AddTensor(poplar::Graph& graph,
                                   const TensorSource& src,
                                   const xla::Shape& shape,
                                   CompilerResources& resources,
                                   const TensorMap& tensor_map);

StatusOr<poplar::Tensor> AddConstantTensor(poplar::Graph& graph,
                                           const TensorSource& src,
                                           const xla::Shape& shape,
                                           const xla::Literal& literal,
                                           CompilerResources& resources,
                                           const TensorMap& tensor_map);

// Creates a constant tensor.
StatusOr<poplar::Tensor> CreateConstantTensor(poplar::Graph& graph,
                                              const xla::Literal& literal,
                                              const xla::Shape& shape,
                                              const poplar::Type& poplar_type,
                                              const std::string& name);

// Sets a value of a tensor to a constant.
Status SetInitialTensorValue(poplar::Graph& graph, poplar::Tensor& tensor,
                             const xla::Literal& literal);

template <typename T>
poplar::Tensor TileTensor(const T& multiples, const poplar::Tensor& in);

StatusOr<poplar::Tensor> PadTensor(const PaddingConfig& cfg,
                                   const poplar::Tensor& in,
                                   const poplar::Tensor& pad);

StatusOr<poplar::Tensor> ReverseTensor(const poplar::Tensor& in,
                                       const std::vector<int64>& dimensions);

StatusOr<poplar::Tensor> BroadcastTensor(
    const poplar::Tensor& in, const xla::Shape& out,
    const std::vector<int64>& dimensions = {});

Status AddOutputTensor(TensorMap& map, const HloInstruction* inst, int64 n,
                       const poplar::Tensor& tensor);

/* This returns a vector of all poplar tensors which are outputs of the inst
 * operand index `input` in range [range.first, range.second).
 */
ArgVector FindInstructionInputsInRange(TensorMap& map, CompilerResources& res,
                                       const HloInstruction* inst, int64 input,
                                       std::pair<int64, int64> range,
                                       poplar::program::Sequence& seq,
                                       bool expand_constants = true);

/* This returns the single poplar tensor which is the non-tuple input to the
 * input to the instruction
 */
StatusOr<poplar::Tensor> FindInstructionInput(
    TensorMap& map, CompilerResources& res, const HloInstruction* inst,
    int64 input, poplar::program::Sequence& seq, bool expand_constants = true);

/* This returns a vector of all poplar tensors which are part of the tuple
 * or non-tuple on the input to the instruction
 */
ArgVector FindInstructionInputs(TensorMap& map, CompilerResources& res,
                                const HloInstruction* inst, int64 input,
                                poplar::program::Sequence& seq,
                                bool expand_constants = true);

bool AreInplaceOutputTensorsWritable(TensorMap& map,
                                     const HloInstruction* inst);

/* Sometimes an inplace op cannot be performed because the input/output tensor
 * is not parallel writable or because further analysis has shown that the op
 * can no longer be in place. If that's the case, this function will add an
 * extra tensor copy and use that tensor as the input/output tensor.
 *
 * The ArgVector contains only those inputs which are listed as inplace inputs
 * by HloInstructionDescription.
 */
StatusOr<ArgVectors> FindInplaceOutputTensors(
    TensorMap& map, CompilerResources& res, const HloInstruction* inst,
    poplar::program::Sequence& seq, bool expand_constants = true,
    bool always_preserve_aliases = false);

/* This returns a vector of poplar tensors which are all of the outputs from
 * the given instruction
 */
OutVector FindInstructionOutputs(const TensorMap& map,
                                 const HloInstruction* inst);

/* This returns a vector of poplar tensors which are all of the outputs from
 * the given instruction - any wide constants are expanded - TODO T5364
 */
OutVector FindExpandedInstructionOutputs(TensorMap& map, CompilerResources& res,
                                         const HloInstruction* inst,
                                         poplar::program::Sequence& seq);

/* Generate a JSON struture describing the tensor mappings
 */
std::string GetTensorMappingJson(const std::string& module_name,
                                 const poplar::Graph& graph,
                                 const TensorMaps& tensor_map);

}  // namespace poplarplugin
}  // namespace xla

#endif
