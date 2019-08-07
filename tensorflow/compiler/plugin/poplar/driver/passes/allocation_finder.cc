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

#include "tensorflow/compiler/plugin/poplar/driver/passes/allocation_finder.h"
#include "tensorflow/compiler/plugin/poplar/driver/compiler_annotations.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/classification_predicates.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/hlo_poplar_instruction.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/remap_deduce.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/plugin/poplar/kernels/custom_kernels_util.h"

#include "tensorflow/compiler/xla/service/dfs_hlo_visitor_with_default.h"
#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/service/hlo_module.h"

#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"

#include "absl/container/flat_hash_set.h"
namespace xla {
namespace poplarplugin {

namespace {

// Find the index of a tensor after extracting it (or a tuple containing it)
// from a tuple. tuple_index is the index of one of the elements of the tuple,
// and original_index is the tensor position within the original tuple.
int64 ExtractFromTuple(const Shape& tuple, int64 tuple_index,
                       int64 original_index) {
  int64 index = original_index;
  for (int64 i = 0; i < tuple_index; i++) {
    index -= CountShapes(ShapeUtil::GetTupleElementShape(tuple, i));
  }
  int64 n = CountShapes(ShapeUtil::GetTupleElementShape(tuple, tuple_index));
  if (index < 0 || index >= n) {
    return -1;
  }
  return index;
}

class FindAllocatingInstructions : public DfsHloVisitorWithDefault {
 public:
  FindAllocatingInstructions() {}

  ~FindAllocatingInstructions() override = default;

  Status DefaultAction(HloInstruction* hlo_instruction) override {
    return Status::OK();
  }

  Status HandleConstant(HloInstruction* inst) override {
    allocating_instructions.push_back(std::make_pair(inst, 0));
    return Status::OK();
  }

  Status HandleRng(HloInstruction* inst) override {
    allocating_instructions.push_back(std::make_pair(inst, 0));
    return Status::OK();
  }

  Status HandleParameter(HloInstruction* inst) override {
    auto shapes = FlattenedXlaShape(inst->shape());
    for (unsigned int i = 0; i < shapes.size(); i++) {
      allocating_instructions.push_back(std::make_pair(inst, i));
    }
    return Status::OK();
  }

  Status HandleInfeed(HloInstruction* inst) override {
    HloInfeedInstruction* infeed = Cast<HloInfeedInstruction>(inst);
    auto shapes = FlattenedXlaShape(infeed->infeed_shape());
    for (unsigned int i = 0; i < shapes.size(); i++) {
      allocating_instructions.push_back(std::make_pair(inst, i));
    }
    return Status::OK();
  }

  Status HandleCustomCall(HloInstruction* inst) override {
    HloCustomCallInstruction* remap = DynCast<HloCustomCallInstruction>(inst);

    if (remap != nullptr) {
      auto shapes = FlattenedXlaShape(remap->shape());
      for (unsigned int i = 0; i < shapes.size(); i++) {
        allocating_instructions.push_back(std::make_pair(inst, i));
      }
    }
    return Status::OK();
  }

  Status HandleFusion(HloInstruction* inst) override {
    if (IsPopOpsFusion(inst, "wide_const")) {
      allocating_instructions.push_back(std::make_pair(inst, 0));
    }
    return Status::OK();
  }

  Status HandleReduceWindow(HloInstruction* inst) override {
    allocating_instructions.push_back(std::make_pair(inst, 0));
    return Status::OK();
  }

  std::vector<TensorSource> allocating_instructions;
};

}  // namespace

// This function traverses the tensor flow (!) from the source to the tensor
// target, and marks all those tensors as having a layout.
TensorsWithLayouts GetAllLayoutsInPath(const TensorSource& source,
                                       const TensorTarget& tensor_target) {
  TensorsWithLayouts ops_with_layout;
  ops_with_layout.insert(source);

  const HloInstruction* parent = source.first;
  int64 tuple_index = source.second;
  for (auto& user : tensor_target.backward_path) {
    switch (user->opcode()) {
      case HloOpcode::kTuple: {
        tuple_index = InsertIntoTuple(user->shape(),
                                      user->operand_index(parent), tuple_index);
        break;
      }
      case HloOpcode::kGetTupleElement: {
        tuple_index =
            ExtractFromTuple(parent->shape(), user->tuple_index(), tuple_index);
        break;
      }
      default: { break; }
    }
    ops_with_layout.insert(std::make_pair(user, tuple_index));
    parent = user;
  }
  return ops_with_layout;
}

void AllocationFinder::AddTensorTarget(const TensorSource& source,
                                       const TensorTarget& tensor_target) {
  tensor_allocation_map.insert(std::make_pair(source, tensor_target));
  auto ops_with_layout = GetAllLayoutsInPath(source, tensor_target);
  absl::c_copy(ops_with_layout,
               std::inserter(tensors_with_layout, tensors_with_layout.end()));
}

bool AllocationFinder::CompareTargets(const TensorTarget& a,
                                      const TensorTarget& b) {
  return IsForward(a.tgt, annotations) && !IsForward(b.tgt, annotations);
}

void AllocationFinder::FindConsumers(const TensorSource& src,
                                     const HloInstruction* tgt, int64 index) {
  path.emplace_back(tgt);
  for (auto user : tgt->users()) {
    if (visited.count(user) == 0) {
      visited.insert(user);
      int64 op_index = user->operand_index(tgt);
      switch (user->opcode()) {
        case HloOpcode::kConvolution: {
          auto t = TensorTarget(user, op_index, path);
          auto i = tensor_allocation_map.find(src);
          if (i != tensor_allocation_map.end() &&
              CompareTargets(t, i->second)) {
            tensor_allocation_map.erase(src);
          }
          AddTensorTarget(src, t);
          break;
        }
        case HloOpcode::kDot: {
          auto t = TensorTarget(user, op_index, path);
          auto i = tensor_allocation_map.find(src);
          if (i != tensor_allocation_map.end() &&
              CompareTargets(t, i->second)) {
            tensor_allocation_map.erase(src);
          }
          AddTensorTarget(src, t);
          break;
        }
        case HloOpcode::kDynamicSlice: {
          if (op_index == 0) {
            auto t = TensorTarget(user, op_index, path);
            auto i = tensor_allocation_map.find(src);
            if (i != tensor_allocation_map.end()) {
              tensor_allocation_map.erase(src);
            }
            AddTensorTarget(src, t);
          }
          break;
        }
        case HloOpcode::kDynamicUpdateSlice: {
          if (op_index == 0 || op_index == 1) {
            auto t = TensorTarget(user, op_index, path);
            auto i = tensor_allocation_map.find(src);
            if (i != tensor_allocation_map.end()) {
              tensor_allocation_map.erase(src);
            }
            AddTensorTarget(src, t);
          }
          break;
        }
        case HloOpcode::kScatter: {
          if (op_index == 0 || op_index == 2) {
            auto t = TensorTarget(user, op_index, path);
            auto i = tensor_allocation_map.find(src);
            if (i != tensor_allocation_map.end()) {
              tensor_allocation_map.erase(src);
            }
            AddTensorTarget(src, t);
          }
          break;
        }
        case HloOpcode::kGather: {
          if (op_index == 0) {
            auto t = TensorTarget(user, op_index, path);
            auto i = tensor_allocation_map.find(src);
            if (i != tensor_allocation_map.end()) {
              tensor_allocation_map.erase(src);
            }
            tensor_allocation_map.insert(std::make_pair(src, t));
          }
          break;
        }
        case HloOpcode::kCall: {
          // This also handles repeat loops which are represented as a Call
          // operation.
          HloComputation* comp = user->to_apply();
          HloInstruction* param = comp->parameter_instruction(op_index);
          FindConsumers(src, param, index);
          break;
        }
        case HloOpcode::kFusion: {
          HloComputation* comp = user->fused_instructions_computation();
          if (IsPopOpsFusion(user)) {
            auto end = comp->name().find('.');
            std::string name = comp->name().substr(8, end - 8);
            if (name == "depthwise_conv") {
              auto t = TensorTarget(user, op_index, path);
              auto i = tensor_allocation_map.find(src);
              if (i != tensor_allocation_map.end()) {
                tensor_allocation_map.erase(src);
              }
              AddTensorTarget(src, t);
            }
          }
          break;
        }
        case HloOpcode::kCustomCall: {
          if (IsPoplibsHloCustomOp(user)) {
            auto poplar_inst = Cast<HloPoplarInstruction>(user);
            auto allocating_indexes = poplar_inst->AllocatingIndices();

            if (allocating_indexes.count(op_index)) {
              auto t = TensorTarget(user, op_index, path);
              auto i = tensor_allocation_map.find(src);
              if (i != tensor_allocation_map.end() &&
                  CompareTargets(t, i->second)) {
                tensor_allocation_map.erase(src);
              }
              AddTensorTarget(src, t);
            }
          } else {
            auto shapes = FlattenedXlaShape(src.first->shape());
            if (ShapeUtil::Equal(shapes[src.second], user->shape())) {
              FindConsumers(src, user, index);
            }
          }
          break;
        }
        case HloOpcode::kWhile: {
          HloComputation* comp = user->while_body();
          HloInstruction* param = comp->parameter_instruction(op_index);
          FindConsumers(src, param, index);
          break;
        }
        case HloOpcode::kTuple: {
          int64 new_index = InsertIntoTuple(user->shape(), op_index, index);
          FindConsumers(src, user, new_index);
          break;
        }
        case HloOpcode::kGetTupleElement: {
          int64 tuple_index = user->tuple_index();
          int64 new_index = ExtractFromTuple(tgt->shape(), tuple_index, index);
          if (new_index != -1) {
            FindConsumers(src, user, new_index);
          }
          break;
        }
        case HloOpcode::kReshape: {
          FindConsumers(src, user, index);
          break;
        }
        case HloOpcode::kTranspose: {
          FindConsumers(src, user, index);
          break;
        }
        case HloOpcode::kConvert: {
          FindConsumers(src, user, index);
          break;
        }
        default: {
          auto shapes = FlattenedXlaShape(src.first->shape());
          if (ShapeUtil::Equal(shapes[src.second], user->shape())) {
            FindConsumers(src, user, index);
          }
          break;
        }
      }
    }
  }
  path.pop_back();
  return;
}

StatusOr<bool> AllocationFinder::Run(HloModule* module) {
  FindAllocatingInstructions finder;

  for (const auto& comp : module->computations()) {
    if (!IsPopOpsFusion(comp)) {
      TF_RETURN_IF_ERROR(comp->Accept(&finder));
    }
  }

  for (auto inst : finder.allocating_instructions) {
    visited.clear();
    FindConsumers(inst, inst.first, inst.second);
  }

  return true;
}

AllocationFinder::AllocationFinder(CompilerAnnotations& annotations)
    : annotations(annotations),
      tensor_allocation_map(annotations.tensor_allocation_map),
      tensors_with_layout(annotations.tensors_with_layout) {}

}  // namespace poplarplugin
}  // namespace xla
