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

#include "tensorflow/compiler/plugin/poplar/driver/tools/custom_ops/user_op_hlo.h"
#include "tensorflow/compiler/plugin/poplar/kernels/custom_kernels_util.h"
#include "tensorflow/compiler/plugin/poplar/kernels/poplibs_ops.pb.h"

#include "absl/strings/str_cat.h"

namespace xla {
namespace poplarplugin {

HloUserOpInstruction::HloUserOpInstruction(
    absl::Span<HloInstruction* const> inputs, const Shape& shape,
    const std::string& path, void* fn_ptr, void* metadata_fn_ptr,
    void* allocator_function_ptr, bool is_gradient, bool is_user_read_write)
    : HloPoplarInstruction(
          shape, inputs,
          GetPoplibsCustomOpTargetString(PoplibsOp::Poputil, PoplibsOp::UserOp),
          fn_ptr, metadata_fn_ptr, allocator_function_ptr, path, is_gradient),
      function_ptr_(fn_ptr),
      metadata_function_ptr_(metadata_fn_ptr),
      allocator_function_ptr_(allocator_function_ptr),
      gp_path(path),
      is_gradient_(is_gradient),
      is_user_read_write_(is_user_read_write) {
  set_custom_call_has_side_effect(true);
  num_inputs_ = inputs.size();

  // If there is a metadata function, call it to populate the metadata_ struct.
  if (metadata_function_ptr_ != nullptr) {
    void (*metadataSignature)(
        std::unordered_set<std::int64_t> & allocating_indices,
        std::unordered_map<std::int64_t, std::int64_t> & layout_dependencies,
        std::uint32_t & num_inplace, bool& is_elementwise,
        std::uint32_t num_inputs);

    metadataSignature =
        reinterpret_cast<decltype(metadataSignature)>(metadata_function_ptr_);

    metadataSignature(metadata_.allocating_indices_,
                      metadata_.layout_dependencies_, metadata_.num_inplace_,
                      metadata_.is_elementwise_, num_inputs_);
  }
}

absl::flat_hash_set<int64> HloUserOpInstruction::AllocatingIndices() const {
  absl::flat_hash_set<int64> set;
  for (std::int64_t i : metadata_.allocating_indices_) {
    set.insert({i});
  }
  return set;
}

absl::flat_hash_map<int64, int64> HloUserOpInstruction::LayoutDependencies()
    const {
  absl::flat_hash_map<int64, int64> map;

  for (auto& pair : metadata_.layout_dependencies_) {
    map[pair.first] = pair.second;
  }
  return map;
}

uint64 HloUserOpInstruction::NumberOfInplaceOperands() const {
  return metadata_.num_inplace_;
}

bool HloUserOpInstruction::IsPopOpsElementwise() const {
  return metadata_.is_elementwise_;
}

std::vector<string> HloUserOpInstruction::ExtraPoplarAttributesToStringImpl(
    const HloPrintOptions& options) const {
  std::stringstream ss;
  ss << function_ptr_;
  std::string function_ptr_address = ss.str();
  ss.clear();

  ss << metadata_function_ptr_;
  std::string metadata_ptr_address = ss.str();
  ss.clear();

  ss << allocator_function_ptr_;
  std::string allocator_ptr_address = ss.str();
  ss.clear();

  std::vector<string> attributes;
  attributes.push_back(absl::StrCat("function_ptr=", function_ptr_address));
  attributes.push_back(absl::StrCat("metadata_ptr=", metadata_ptr_address));
  attributes.push_back(absl::StrCat("allocator_ptr=", allocator_ptr_address));

  attributes.push_back(
      absl::StrCat("metadata_.is_elementwise_=", metadata_.is_elementwise_));
  attributes.push_back(
      absl::StrCat("metadata_.num_inplace_=", metadata_.num_inplace_));

  attributes.push_back(absl::StrCat("num_inputs_=", num_inputs_));
  attributes.push_back(absl::StrCat("gp_path=", gp_path));

  return attributes;
}

std::unique_ptr<HloInstruction> HloUserOpInstruction::CloneWithNewOperandsImpl(
    const Shape& shape, absl::Span<HloInstruction* const> new_operands,
    HloCloneContext*) const {
  return CreateUserOp(new_operands, shape, GetPath(), function_ptr_,
                      metadata_function_ptr_, allocator_function_ptr_,
                      is_gradient_, is_user_read_write_);
}

std::unique_ptr<HloInstruction> CreateUserOp(
    absl::Span<HloInstruction* const> inputs, const Shape& shape,
    const std::string& gp_path, void* function_ptr, void* metadata_function_ptr,
    void* allocator_function_ptr, bool is_gradient, bool is_user_read_write) {
  return absl::make_unique<HloUserOpInstruction>(
      inputs, shape, gp_path, function_ptr, metadata_function_ptr,
      allocator_function_ptr, is_gradient, is_user_read_write);
}

namespace {

static HloPoplarInstructionFactory user_op_factory(
    GetPoplibsCustomOpTargetString(PoplibsOp::Poputil, PoplibsOp::UserOp),
    [](HloCustomCallInstruction* call)
        -> StatusOr<std::unique_ptr<xla::HloInstruction>> {
      auto attribute_map = IPUCustomKernelsUtil::AttributeMap(call);

      TF_ASSIGN_OR_RETURN(uint64 operation_fn,
                          attribute_map.GetAttributeAsUInt64("operation_fn"));
      void* operation_fn_ptr = reinterpret_cast<void*>(operation_fn);

      TF_ASSIGN_OR_RETURN(
          uint64 metadata_function,
          attribute_map.GetAttributeAsUInt64("metadata_function"));
      void* metadata_function_ptr = reinterpret_cast<void*>(metadata_function);

      TF_ASSIGN_OR_RETURN(
          uint64 allocator_function,
          attribute_map.GetAttributeAsUInt64("allocator_function"));
      void* allocator_function_ptr =
          reinterpret_cast<void*>(allocator_function);

      TF_ASSIGN_OR_RETURN(std::string gp_path,
                          attribute_map.GetAttributeAsString("gp_path"));

      TF_ASSIGN_OR_RETURN(bool is_gradient,
                          attribute_map.GetAttributeAsBool("is_gradient"));

      TF_ASSIGN_OR_RETURN(
          bool is_user_read_write,
          attribute_map.GetAttributeAsBool("is_user_read_write"));

      return CreateUserOp(call->operands(), call->shape(), gp_path,
                          operation_fn_ptr, metadata_function_ptr,
                          allocator_function_ptr, is_gradient,
                          is_user_read_write);
    });
}  // namespace

}  // namespace poplarplugin
}  // namespace xla
