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

#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_executor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/data_initializer.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"

#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_computation.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/lib/core/errors.h"

#include <poplar/Graph.hpp>
#include <popops/DynamicSlice.hpp>
#include <popops/ElementWise.hpp>

namespace pe = popops::expr;

namespace xla {
namespace poplarplugin {

StatusOr<poplar::program::Program> CreateInfeed(CompilerResources& res,
                                                const HloInstruction* inst,
                                                int64 tuple_index,
                                                const xla::Shape& shape,
                                                poplar::Tensor tensor) {
  poplar::program::Sequence seq;
  const HloInfeedInstruction* infeed = Cast<HloInfeedInstruction>(inst);

  poplar::Graph& graph = GetGraph(res, inst);

  // Parse the infeed config to find out how much data to prefetch if at all.
  xla::poplarplugin::PoplarFeedConfig infeed_config;
  infeed_config.ParseFromString(infeed->infeed_config());

  // The amount of data the user has specified to be prefetched on each host
  // sync.
  size_t io_batch_size = std::max<size_t>(1, infeed_config.io_batch_size());

  // A functor wrapper to either use synthetic data or copy from the host,
  // depending on the global synthetic flags.
  auto init_synthetic_or_copy = [&](poplar::program::Sequence& seq,
                                    const Shape& data_shape,
                                    poplar::Tensor& tensor_to_update) {
    if (!UseSyntheticData()) {
      auto fifo = graph.addHostToDeviceFIFO(
          GetInfeedCopyHandle(infeed->name(), tuple_index),
          tensor_to_update.elementType(), tensor_to_update.numElements());
      seq.add(poplar::program::Copy(fifo, tensor_to_update, false));
    } else if (UseSyntheticData() && UseSyntheticDataInitializer()) {
      // Initialize the tensor with a synthetic initalizer.
      auto& initializer = DataInitializer::GetSyntheticDataInitializer();
      TF_ASSIGN_OR_RETURN(auto literal, initializer.GetData(data_shape));
      TF_RETURN_IF_ERROR(
          SetInitialTensorValue(graph, tensor_to_update, literal));
    }
    // If neither case then we want synthetic data but don't want it initalized
    // so we just return the empty tensor unchanged.
    return Status::OK();
  };

  if (io_batch_size != 1) {
    // Extend the old shape to add a new dimension for the batches of memory.
    std::vector<size_t> new_shape = tensor.shape();
    new_shape.insert(new_shape.begin(), io_batch_size);

    std::vector<poplar::Tensor> cloned_tensors(io_batch_size);
    for (int i = 0; i < io_batch_size; ++i) {
      if (res.always_rearrange_copies_on_host) {
        // When rearranging on the host, it is better to keep the layout of the
        // slices in the output tensor layout, in order to minimise on-device
        // rearrangement.
        cloned_tensors[i] = graph.clone(tensor);
      } else {
        // When rearranging on the device, it is better to rearrange after the
        // dynamic slice, so that the rearrangement only takes place on the
        // slice, not the whole incoming pegged_memory buffer.
        cloned_tensors[i] =
            graph.addVariable(tensor.elementType(), tensor.shape(),
                              poplar::VariableMappingMethod::LINEAR);
      }
    }
    // Concatenate all the cloned tensors then reshape to make sure we are
    // in the shape [io_batch_size][original_shape].
    poplar::Tensor pegged_memory =
        poplar::concat(cloned_tensors).reshape(new_shape);

    // A counter for tracking the number of entries in the buffer
    poplar::Tensor counter = graph.addVariable(
        poplar::UNSIGNED_INT, {},
        GetDebugName(inst) + "/InfeedCtr/" + std::to_string(tuple_index));
    // Map counter to the next tile.
    MappingHelper::MapTensorLinearly(res.linear_mapping_state, graph, counter);
    res.zeroed_tensors.push_back(counter);

    // The body for copying from host and zeroing the counter.
    poplar::program::Sequence true_body;

    // If we are using synthetic data, init pegged_memory with it otherwise host
    // copy. Either way we will have a tensor with some number of prefetched
    // batches and we will dynamic slice the actual batch from that. This is to
    // ensure that we can benchmark synthetic vs non-synthetic without changing
    // the graph too much.
    TF_RETURN_IF_ERROR(init_synthetic_or_copy(
        true_body,
        XlaShapeFromPoplarShape(shape.element_type(), pegged_memory.shape()),
        pegged_memory));

    // The NOP body.
    poplar::program::Sequence false_body;

    // Predicate for fetching the next batch
    poplar::Tensor predicate = popops::map(
        graph, pe::Equal(pe::_1, pe::Const(0)), {counter}, seq,
        GetDebugName(inst) + "/InfeedCtrCmp/" + std::to_string(tuple_index));

    // The main body which contains the control flow for copy from host and
    // the dynamic slice.
    seq.add(poplar::program::If(predicate, true_body, false_body));

    // Use dynamic slice to extract the slices from the buffer
    poplar::Tensor slice = popops::dynamicSlice(
        graph, pegged_memory, counter.reshape({1}), {0}, {1}, seq,
        GetDebugName(inst) + "/Slice/" + std::to_string(tuple_index));
    seq.add(poplar::program::Copy(slice, tensor));

    // Increment the counter by one.
    popops::mapInPlace(
        graph, pe::Rem(pe::Add(pe::_1, pe::Const(1)), pe::Const(io_batch_size)),
        {counter}, seq,
        GetDebugName(inst) + "/InfeedCtrInc/" + std::to_string(tuple_index));

  } else {
    // Just an normal copy from host->tensor or init tensor with synthetic data.
    TF_RETURN_IF_ERROR(init_synthetic_or_copy(seq, shape, tensor));
  }
  return seq;
}

StatusOr<poplar::program::Program> CreateOutfeed(CompilerResources& res,
                                                 const HloInstruction* inst,
                                                 TensorMap& tensor_map) {
  if (res.annotations.outfeed_infos.size()) {
    return InvalidArgument("Only one IPUOutfeedQueue supported per graph.");
  }

  poplar::program::Sequence seq;
  poplar::Graph& graph = GetGraph(res, inst);

  const HloOutfeedInstruction* outfeed = Cast<HloOutfeedInstruction>(inst);
  xla::poplarplugin::PoplarFeedConfig outfeed_config;
  outfeed_config.ParseFromString(outfeed->outfeed_config());

  size_t io_batch_size = std::max<size_t>(1, outfeed_config.io_batch_size());

  // Check that the replication factor matches.
  if (res.replication_factor != outfeed_config.replication_factor()) {
    return xla::FailedPrecondition(
        "Current program has been created with replication_factor %d, however "
        "the IPUOutfeedQueue has been configured with replication_factor %d. "
        "Either reduce the number of IPUs in your TensorFlow device, or set "
        "the `replication_factor` to %d when creating IPUOutfeedQueue.",
        res.replication_factor, outfeed_config.replication_factor(),
        res.replication_factor);
  }

  if (UseSyntheticData()) {
    return seq;
  }

  HloInstruction* operand = outfeed->operands()[0];
  const Shape& shape = operand->shape();
  if (ShapeUtil::IsNestedTuple(shape)) {
    return InvalidArgument("Nested tuple shapes are not supported for outfeed");
  }

  const bool expand_constants = true;
  ArgVector input_tensors =
      FindInstructionInputs(tensor_map, res, inst, 0, seq, expand_constants);

  for (unsigned i = 0; i < input_tensors.size(); ++i) {
    poplar::Tensor& in = input_tensors[i];

    if (io_batch_size == 1) {
      // Simply copy to the stream
      auto fifo =
          graph.addDeviceToHostFIFO(GetOutfeedCopyHandle(inst->name(), i),
                                    in.elementType(), in.numElements());

      seq.add(poplar::program::Copy(in, fifo, false));
    } else {
      // Batch multiple writes, and then write as a block

      // Extend the old shape to add a new dimension for the batches of memory
      std::vector<size_t> new_shape = in.shape();
      new_shape.insert(new_shape.begin(), io_batch_size);

      std::vector<poplar::Tensor> cloned_tensors(io_batch_size);
      for (int i = 0; i < io_batch_size; ++i) {
        if (res.always_rearrange_copies_on_host) {
          // When rearranging on the host it is better to have the slices of the
          // buffer laid out in the same form as the 'in' tensor so that there
          // is no cost of rearrangement.
          cloned_tensors[i] = graph.clone(in);
        } else {
          // When the data is rearranged on the device, it is beter to have the
          // slices arranged in the standard order of the host buffer, and then
          // to have the rearragement done only once, during the dynamicUpdate.
          cloned_tensors[i] =
              graph.addVariable(in.elementType(), in.shape(),
                                poplar::VariableMappingMethod::LINEAR);
        }
      }
      poplar::Tensor batched =
          poplar::concat(cloned_tensors).reshape(new_shape);

      //  A counter for counting slots
      poplar::Tensor counter = graph.addVariable(
          poplar::UNSIGNED_INT, {},
          GetDebugName(inst) + "/OutfeedCtr/" + std::to_string(i));
      // Map counter to the next tile.
      MappingHelper::MapTensorLinearly(res.linear_mapping_state, graph,
                                       counter);
      res.zeroed_tensors.push_back(counter);

      // Use dynamic slice update to put the slices into the buffer
      popops::dynamicUpdate(graph, batched, in.expand({0}),
                            counter.reshape({1}), {0}, {1}, seq,
                            GetDebugName(inst) + "/Slice" + std::to_string(i));

      // Increment the counter by one.
      popops::mapInPlace(
          graph,
          pe::Rem(pe::Add(pe::_1, pe::Const(1)), pe::Const(io_batch_size)),
          {counter}, seq,
          GetDebugName(inst) + "/OutfeedCtrInc/" + std::to_string(i));

      // The body for copying to host and zeroing the counter.
      poplar::program::Sequence true_body;

      // Copy the data to the host
      if (!UseSyntheticData()) {
        auto fifo = graph.addDeviceToHostFIFO(
            GetOutfeedCopyHandle(outfeed->name(), i), batched.elementType(),
            batched.numElements());
        true_body.add(poplar::program::Copy(batched, fifo, false));
      }

      // The NOP body.
      poplar::program::Sequence false_body;

      // Check the counter doesn't equal
      poplar::Tensor predicate = popops::map(
          graph, pe::Equal(pe::_1, pe::Const(0)), {counter}, seq,
          GetDebugName(inst) + "/OutfeedCtrCmp/" + std::to_string(i));

      // The main body which contains the control flow for copy from host and
      // the dynamic slice.
      seq.add(poplar::program::If(predicate, true_body, false_body));
    }
  }

  FeedInfo info(outfeed->name(), outfeed_config,
                outfeed->operands()[0]->shape());
  res.annotations.outfeed_infos.push_back(info);
  return seq;
}

}  // namespace poplarplugin
}  // namespace xla
