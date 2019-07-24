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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_VISITORS_DEFERRED_ALLOCATION_VISITOR_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_VISITORS_DEFERRED_ALLOCATION_VISITOR_H_

#include "tensorflow/compiler/plugin/poplar/driver/passes/allocation_finder.h"
#include "tensorflow/compiler/plugin/poplar/driver/visitors/visitor_full.h"

namespace xla {
namespace poplarplugin {

struct CompilerResources;
/*
 * This visitor uses the deferred allocation info to allocate tuple allocation
 * targets when needed.
 * This is required for forward allocations where the target and the source both
 * come from the same input instruction.
 */
class DeferredAllocationVisitor : public FullVisitor {
 public:
  DeferredAllocationVisitor(CompilerResources& resources)
      : FullVisitor(resources) {}

  // GTEs are specialised:
  // * if the GTE input is deferred and:
  //   - this is the deferred allocation place then this calls the AllocateInput
  //   - otherwise it skips all the deferred allocations in the output.
  // * Otherwise it behaves like a GTE.
  Status HandleGetTupleElement(HloInstruction* inst) override;

  Status HandleInfeed(HloInstruction* inst) override;

  poplar::program::Sequence GetSequence() const override {
    poplar::program::Sequence seq;
    seq.add(merged_infeed_sequence);
    seq.add(sequence);
    return seq;
  }

 protected:
  // Allocates the input and calls the post processing function - this function
  // should be called by HandleParameter and HandleInfeed. If it's allocating a
  // deferred input then it also makes sure to set the outputs of all
  // instructions between the input tuple and inst to this allocation.
  Status AllocateInput(const HloInstruction* inst, const int64 flat_tuple_index,
                       const Shape& shape);

  // Called by AllocateInput when allocating an input for an infeed.
  StatusOr<poplar::Tensor> PostProcessInfeedAllocation(
      const HloInstruction* inst, const int64 flat_tuple_index,
      const Shape& shape, poplar::Tensor tensor);

  // Called by AllocateInput when allocating an input for a paramter.
  virtual StatusOr<poplar::Tensor> PostProcessParameterAllocation(
      const HloInstruction* inst, const int64 flat_tuple_index,
      const Shape& shape, poplar::Tensor tensor) = 0;

  // Returns true if the passed parameter can be deferred.
  bool CanDeferAllocation(const HloInstruction* inst,
                          const int64 flat_tuple_index);

  // Marks the passed parameter as deferred allocation.
  void DeferAllocation(const HloInstruction* inst,
                       const int64 flat_tuple_index);

 private:
  // Returns true if the passed parameter is in the deferred allocation path
  // between tensor source and its actual allocation.
  bool IsInDeferredAllocationPath(const HloInstruction* inst,
                                  const int64 flat_tuple_index);

  // Returns true if this is the deferred tensor allocation.
  bool IsDeferredAllocation(const HloInstruction* inst,
                            const int64 flat_tuple_index);

  // Stores all the tensors in the path between tensor source and its actual
  // allocation.
  absl::flat_hash_set<TensorSource> instructions_in_deferred_allocation_paths;
  // Stores the locations where deferred tensors are allocated.
  absl::flat_hash_set<TensorSource> deferred_allocation_sources;

  poplar::program::Sequence merged_infeed_sequence;
};

}  // namespace poplarplugin
}  // namespace xla

#endif
