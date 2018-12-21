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

#include "tensorflow/compiler/plugin/poplar/driver/allocation_finder.h"
#include "tensorflow/compiler/plugin/poplar/driver/compiler_annotations.h"
#include "tensorflow/compiler/plugin/poplar/driver/forward_allocation.h"

#include "tensorflow/compiler/xla/service/hlo_parser.h"
#include "tensorflow/compiler/xla/service/shape_inference.h"

#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/core/lib/core/status_test_util.h"

namespace xla {
namespace poplarplugin {
namespace {

using AllocationFinderTest = HloTestBase;

static Window GetConv1Window() {
  Window window;
  for (int i = 0; i < 2; ++i) {
    auto dim = window.add_dimensions();
    dim->set_size(3);
    dim->set_stride(1);
    dim->set_padding_low(1);
    dim->set_padding_high(1);
    dim->set_window_dilation(1);
    dim->set_base_dilation(1);
  }
  return window;
}

static Window GetConv2Window() {
  Window window;
  for (int i = 0; i < 2; ++i) {
    auto dim = window.add_dimensions();
    dim->set_size(3);
    dim->set_stride(2);
    dim->set_padding_low(1);
    dim->set_padding_high(1);
    dim->set_window_dilation(1);
    dim->set_base_dilation(1);
  }
  return window;
}

static ConvolutionDimensionNumbers GetConvDimensions() {
  ConvolutionDimensionNumbers dimension;
  dimension.set_input_batch_dimension(0);
  dimension.add_input_spatial_dimensions(1);
  dimension.add_input_spatial_dimensions(2);
  dimension.set_input_feature_dimension(3);

  dimension.set_output_batch_dimension(0);
  dimension.add_output_spatial_dimensions(1);
  dimension.add_output_spatial_dimensions(2);
  dimension.set_output_feature_dimension(3);

  dimension.add_kernel_spatial_dimensions(0);
  dimension.add_kernel_spatial_dimensions(1);
  dimension.set_kernel_input_feature_dimension(2);
  dimension.set_kernel_output_feature_dimension(3);
  return dimension;
}

// Check basic parameter matching
TEST_F(AllocationFinderTest, FindBasicTensorAllocations) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f16[1,16,16,2] parameter(0)
  p1 = f16[1,16,16,2] parameter(1)
  p2 = f16[3,3,2,4] parameter(2)

  add = f16[1,16,16,2] add(p0, p1)

  conv = f16[1,16,16,4] convolution(p0, p2), window={size=3x3 pad=1_1x1_1}, dim_labels=b01f_01io->b01f

  ROOT t = (f16[1,16,16,4], f16[1,16,16,2]) tuple(conv, add)
}

)";

  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(2);
  config.set_resource_update_to_input_index({0});
  auto module = ParseHloString(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* module0 = module.ValueOrDie().get();

  const auto* root = module0->entry_computation()->root_instruction();
  const auto* conv = root->operand(0);
  const auto* ip0 = conv->operand(0);
  const auto* ip2 = conv->operand(1);

  CompilerAnnotations annotations(module0);

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(module0).ValueOrDie());

  ASSERT_EQ(annotations.tensor_allocation_map.size(), 2);

  auto t = annotations.tensor_allocation_map.at(std::make_pair(ip0, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);
  EXPECT_EQ(t.backward_path[0], ip0);

  t = annotations.tensor_allocation_map.at(std::make_pair(ip2, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);
  EXPECT_EQ(t.backward_path[0], ip2);
}

// Check it goes through call sites
TEST_F(AllocationFinderTest, FindSubCompTensorAllocations) {
  Shape input_shape = ShapeUtil::MakeShape(F32, {1, 10, 10, 2});
  Shape weight_shape = ShapeUtil::MakeShape(F32, {3, 3, 2, 1});

  Shape conv_shape = ShapeInference::InferConvolveShape(
                         input_shape, weight_shape, /*feature_group_count=*/1,
                         GetConv1Window(), GetConvDimensions())
                         .ConsumeValueOrDie();

  /* Create convolution sub-computation */
  auto builder_sub = HloComputation::Builder(TestName());
  auto op0_sub = builder_sub.AddInstruction(
      HloInstruction::CreateParameter(0, input_shape, "input"));
  auto op1_sub = builder_sub.AddInstruction(
      HloInstruction::CreateParameter(1, weight_shape, "weights"));

  auto conv = builder_sub.AddInstruction(HloInstruction::CreateConvolve(
      conv_shape, op0_sub, op1_sub, /*feature_group_count=*/1, GetConv1Window(),
      GetConvDimensions(), DefaultPrecisionConfig(2)));

  auto computation_sub = builder_sub.Build();

  /* Create main computation */
  auto builder_main = HloComputation::Builder(TestName());
  auto op0 = builder_main.AddInstruction(
      HloInstruction::CreateParameter(0, input_shape, "op0"));
  auto op1 = builder_main.AddInstruction(
      HloInstruction::CreateParameter(1, input_shape, "op1"));
  auto op2 = builder_main.AddInstruction(
      HloInstruction::CreateParameter(2, weight_shape, "op2"));

  auto add = builder_main.AddInstruction(
      HloInstruction::CreateBinary(input_shape, HloOpcode::kAdd, op0, op1));

  auto call = builder_main.AddInstruction(HloInstruction::CreateCall(
      conv_shape, {op1, op2}, computation_sub.get()));

  builder_main.AddInstruction(HloInstruction::CreateTuple({add, call}));

  auto computation_main = builder_main.Build();

  auto hlo_module = CreateNewModule();
  hlo_module->AddEmbeddedComputation(std::move(computation_sub));
  hlo_module->AddEntryComputation(std::move(computation_main));

  CompilerAnnotations annotations(hlo_module.get());

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(hlo_module.get()).ValueOrDie());

  const HloInstruction* c_conv = conv;

  ASSERT_EQ(annotations.tensor_allocation_map.size(), 4);
  auto t = annotations.tensor_allocation_map.at(std::make_pair(op1, 0));
  EXPECT_EQ(t.tgt, c_conv);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 2);

  t = annotations.tensor_allocation_map.at(std::make_pair(op2, 0));
  EXPECT_EQ(t.tgt, c_conv);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 2);

  t = annotations.tensor_allocation_map.at(std::make_pair(op0_sub, 0));
  EXPECT_EQ(t.tgt, c_conv);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);

  t = annotations.tensor_allocation_map.at(std::make_pair(op1_sub, 0));
  EXPECT_EQ(t.tgt, c_conv);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);
}

// Check it works for multiple valid destinations (perferred one first)
TEST_F(AllocationFinderTest, FindMultiCompTensorAllocations1) {
  Shape input_shape = ShapeUtil::MakeShape(F32, {1, 10, 10, 2});
  Shape weight_shape = ShapeUtil::MakeShape(F32, {3, 3, 2, 1});

  Shape conv1_shape = ShapeInference::InferConvolveShape(
                          input_shape, weight_shape, /*feature_group_count=*/1,
                          GetConv1Window(), GetConvDimensions())
                          .ConsumeValueOrDie();

  Shape conv2_shape = ShapeInference::InferConvolveShape(
                          input_shape, weight_shape, /*feature_group_count=*/1,
                          GetConv2Window(), GetConvDimensions())
                          .ConsumeValueOrDie();

  /* Create convolution sub-computation 1 */
  auto builder_sub1 = HloComputation::Builder(TestName());
  auto op0_sub1 = builder_sub1.AddInstruction(
      HloInstruction::CreateParameter(0, input_shape, "input"));
  auto op1_sub1 = builder_sub1.AddInstruction(
      HloInstruction::CreateParameter(1, weight_shape, "weights"));

  auto conv1 = builder_sub1.AddInstruction(HloInstruction::CreateConvolve(
      conv1_shape, op0_sub1, op1_sub1, /*feature_group_count=*/1,
      GetConv1Window(), GetConvDimensions(), DefaultPrecisionConfig(2)));

  auto computation_sub1 = builder_sub1.Build();

  /* Create convolution sub-computation 2 */
  auto builder_sub2 = HloComputation::Builder(TestName());
  auto op0_sub2 = builder_sub2.AddInstruction(
      HloInstruction::CreateParameter(0, input_shape, "input"));
  auto op1_sub2 = builder_sub2.AddInstruction(
      HloInstruction::CreateParameter(1, weight_shape, "weights"));

  auto conv2 = builder_sub2.AddInstruction(HloInstruction::CreateConvolve(
      conv2_shape, op0_sub2, op1_sub2, /*feature_group_count=*/1,
      GetConv1Window(), GetConvDimensions(), DefaultPrecisionConfig(2)));

  auto computation_sub2 = builder_sub2.Build();

  /* Create main computation */
  auto builder_main = HloComputation::Builder(TestName());
  auto op0 = builder_main.AddInstruction(
      HloInstruction::CreateParameter(0, input_shape, "op0"));
  auto op1 = builder_main.AddInstruction(
      HloInstruction::CreateParameter(1, input_shape, "op1"));
  auto op2 = builder_main.AddInstruction(
      HloInstruction::CreateParameter(2, weight_shape, "op2"));

  auto add = builder_main.AddInstruction(
      HloInstruction::CreateBinary(input_shape, HloOpcode::kAdd, op0, op1));

  auto call1 = builder_main.AddInstruction(HloInstruction::CreateCall(
      conv1_shape, {op1, op2}, computation_sub1.get()));

  auto call2 = builder_main.AddInstruction(HloInstruction::CreateCall(
      conv2_shape, {op1, op2}, computation_sub2.get()));

  builder_main.AddInstruction(HloInstruction::CreateTuple({add, call1, call2}));

  auto computation_main = builder_main.Build();

  auto hlo_module = CreateNewModule();
  hlo_module->AddEmbeddedComputation(std::move(computation_sub1));
  hlo_module->AddEmbeddedComputation(std::move(computation_sub2));
  hlo_module->AddEntryComputation(std::move(computation_main));

  CompilerAnnotations annotations(hlo_module.get());
  annotations.classification_map[conv1] = ConvClassificationType::FORWARD;
  annotations.classification_map[conv2] =
      ConvClassificationType::BACKPROP_INPUT;

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(hlo_module.get()).ValueOrDie());

  const HloInstruction* c_conv1 = conv1;
  const HloInstruction* c_conv2 = conv2;

  ASSERT_EQ(annotations.tensor_allocation_map.size(), 6);
  auto t = annotations.tensor_allocation_map.at(std::make_pair(op1, 0));
  EXPECT_EQ(t.tgt, c_conv1);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 2);

  t = annotations.tensor_allocation_map.at(std::make_pair(op2, 0));
  EXPECT_EQ(t.tgt, c_conv1);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 2);

  t = annotations.tensor_allocation_map.at(std::make_pair(op0_sub1, 0));
  EXPECT_EQ(t.tgt, c_conv1);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);

  t = annotations.tensor_allocation_map.at(std::make_pair(op1_sub1, 0));
  EXPECT_EQ(t.tgt, c_conv1);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);

  t = annotations.tensor_allocation_map.at(std::make_pair(op0_sub2, 0));
  EXPECT_EQ(t.tgt, c_conv2);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);

  t = annotations.tensor_allocation_map.at(std::make_pair(op1_sub2, 0));
  EXPECT_EQ(t.tgt, c_conv2);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);
}

// Check it works for multiple valid destinations (perferred one second)
TEST_F(AllocationFinderTest, FindMultiCompTensorAllocations2) {
  Shape input_shape = ShapeUtil::MakeShape(F32, {1, 10, 10, 2});
  Shape weight_shape = ShapeUtil::MakeShape(F32, {3, 3, 2, 1});

  Shape conv1_shape = ShapeInference::InferConvolveShape(
                          input_shape, weight_shape, /*feature_group_count=*/1,
                          GetConv1Window(), GetConvDimensions())
                          .ConsumeValueOrDie();

  Shape conv2_shape = ShapeInference::InferConvolveShape(
                          input_shape, weight_shape, /*feature_group_count=*/1,
                          GetConv2Window(), GetConvDimensions())
                          .ConsumeValueOrDie();

  /* Create convolution sub-computation 1 */
  auto builder_sub1 = HloComputation::Builder(TestName());
  auto op0_sub1 = builder_sub1.AddInstruction(
      HloInstruction::CreateParameter(0, input_shape, "input"));
  auto op1_sub1 = builder_sub1.AddInstruction(
      HloInstruction::CreateParameter(1, weight_shape, "weights"));

  auto conv1 = builder_sub1.AddInstruction(HloInstruction::CreateConvolve(
      conv1_shape, op0_sub1, op1_sub1, /*feature_group_count=*/1,
      GetConv1Window(), GetConvDimensions(), DefaultPrecisionConfig(2)));

  auto computation_sub1 = builder_sub1.Build();

  /* Create convolution sub-computation 2 */
  auto builder_sub2 = HloComputation::Builder(TestName());
  auto op0_sub2 = builder_sub2.AddInstruction(
      HloInstruction::CreateParameter(0, input_shape, "input"));
  auto op1_sub2 = builder_sub2.AddInstruction(
      HloInstruction::CreateParameter(1, weight_shape, "weights"));

  auto conv2 = builder_sub2.AddInstruction(HloInstruction::CreateConvolve(
      conv2_shape, op0_sub2, op1_sub2, /*feature_group_count=*/1,
      GetConv1Window(), GetConvDimensions(), DefaultPrecisionConfig(2)));

  auto computation_sub2 = builder_sub2.Build();

  /* Create main computation */
  auto builder_main = HloComputation::Builder(TestName());
  auto op0 = builder_main.AddInstruction(
      HloInstruction::CreateParameter(0, input_shape, "op0"));
  auto op1 = builder_main.AddInstruction(
      HloInstruction::CreateParameter(1, input_shape, "op1"));
  auto op2 = builder_main.AddInstruction(
      HloInstruction::CreateParameter(2, weight_shape, "op2"));

  auto add = builder_main.AddInstruction(
      HloInstruction::CreateBinary(input_shape, HloOpcode::kAdd, op0, op1));

  auto call1 = builder_main.AddInstruction(HloInstruction::CreateCall(
      conv1_shape, {op1, op2}, computation_sub1.get()));

  auto call2 = builder_main.AddInstruction(HloInstruction::CreateCall(
      conv2_shape, {op1, op2}, computation_sub2.get()));

  builder_main.AddInstruction(HloInstruction::CreateTuple({add, call1, call2}));

  auto computation_main = builder_main.Build();

  auto hlo_module = CreateNewModule();
  hlo_module->AddEmbeddedComputation(std::move(computation_sub1));
  hlo_module->AddEmbeddedComputation(std::move(computation_sub2));
  hlo_module->AddEntryComputation(std::move(computation_main));

  CompilerAnnotations annotations(hlo_module.get());
  annotations.classification_map[conv1] =
      ConvClassificationType::BACKPROP_INPUT;
  annotations.classification_map[conv2] = ConvClassificationType::FORWARD;

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(hlo_module.get()).ValueOrDie());

  const HloInstruction* c_conv1 = conv1;
  const HloInstruction* c_conv2 = conv2;

  ASSERT_EQ(annotations.tensor_allocation_map.size(), 6);

  auto t = annotations.tensor_allocation_map.at(std::make_pair(op1, 0));
  EXPECT_EQ(t.tgt, c_conv2);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 2);

  t = annotations.tensor_allocation_map.at(std::make_pair(op2, 0));
  EXPECT_EQ(t.tgt, c_conv2);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 2);

  t = annotations.tensor_allocation_map.at(std::make_pair(op0_sub1, 0));
  EXPECT_EQ(t.tgt, c_conv1);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);

  t = annotations.tensor_allocation_map.at(std::make_pair(op1_sub1, 0));
  EXPECT_EQ(t.tgt, c_conv1);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);

  t = annotations.tensor_allocation_map.at(std::make_pair(op0_sub2, 0));
  EXPECT_EQ(t.tgt, c_conv2);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);

  t = annotations.tensor_allocation_map.at(std::make_pair(op1_sub2, 0));
  EXPECT_EQ(t.tgt, c_conv2);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);
}

// Check it works for constants
TEST_F(AllocationFinderTest, FindConstantTensorAllocations) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f16[1,16,16,2] parameter(0)
  p1 = f16[1,16,16,2] parameter(1)
  p2 = f16[1,1,2,4] constant(f16[1,1,2,4]{{{{1,0,0,0},{1,0,0,0}}}})

  add = f16[1,16,16,2] add(p0, p1)

  conv = f16[1,16,16,4] convolution(p0, p2), window={size=1x1}, dim_labels=b01f_01io->b01f

  ROOT t = (f16[1,16,16,4], f16[1,16,16,2]) tuple(conv, add)
}

)";

  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(2);
  config.set_resource_update_to_input_index({0});
  auto module = ParseHloString(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* module0 = module.ValueOrDie().get();

  const auto* root = module0->entry_computation()->root_instruction();
  const auto* conv = root->operand(0);
  const auto* ip0 = conv->operand(0);
  const auto* ip2 = conv->operand(1);

  CompilerAnnotations annotations(module0);

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(module0).ValueOrDie());

  ASSERT_EQ(annotations.tensor_allocation_map.size(), 2);

  auto t = annotations.tensor_allocation_map.at(std::make_pair(ip0, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);

  t = annotations.tensor_allocation_map.at(std::make_pair(ip2, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);
}

// Check it goes through Tuple/Detuple pairs
TEST_F(AllocationFinderTest, CanTraverseTuples) {
  auto hlo_module = CreateNewModule();

  Shape lhs_shape = ShapeUtil::MakeShape(F32, {2});
  Shape rhs_shape = ShapeUtil::MakeShape(F32, {2, 2});
  Shape tuple_shape = ShapeUtil::MakeTupleShape({lhs_shape, rhs_shape});

  auto b = HloComputation::Builder(TestName());
  auto in =
      b.AddInstruction(HloInstruction::CreateParameter(0, lhs_shape, "in"));
  auto w =
      b.AddInstruction(HloInstruction::CreateParameter(1, rhs_shape, "weight"));

  auto tuple = b.AddInstruction(HloInstruction::CreateTuple({in, w}));

  auto in1 = b.AddInstruction(
      HloInstruction::CreateGetTupleElement(lhs_shape, tuple, 0));
  auto w1 = b.AddInstruction(
      HloInstruction::CreateGetTupleElement(rhs_shape, tuple, 1));

  DotDimensionNumbers dot_dnums;
  dot_dnums.add_lhs_contracting_dimensions(1);
  dot_dnums.add_rhs_contracting_dimensions(0);
  auto dot_inst = b.AddInstruction(HloInstruction::CreateDot(
      lhs_shape, in1, w1, dot_dnums, DefaultPrecisionConfig(2)));

  hlo_module->AddEntryComputation(b.Build());

  CompilerAnnotations annotations(hlo_module.get());

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(hlo_module.get()).ValueOrDie());

  const HloInstruction* dot = dot_inst;

  ASSERT_EQ(annotations.tensor_allocation_map.size(), 2);

  auto t = annotations.tensor_allocation_map.at(std::make_pair(in, 0));
  EXPECT_EQ(t.tgt, dot);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 3);

  t = annotations.tensor_allocation_map.at(std::make_pair(w, 0));
  EXPECT_EQ(t.tgt, dot);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 3);
}

// Check it can start from tuple subshapes
TEST_F(AllocationFinderTest, CanStartOnTuples) {
  auto hlo_module = CreateNewModule();

  Shape lhs_shape = ShapeUtil::MakeShape(F32, {2});
  Shape rhs_shape = ShapeUtil::MakeShape(F32, {2, 2});
  Shape tuple_shape = ShapeUtil::MakeTupleShape({lhs_shape, rhs_shape});

  auto b = HloComputation::Builder(TestName());
  auto in = b.AddInstruction(
      HloInstruction::CreateParameter(0, tuple_shape, "tuple"));

  auto in1 =
      b.AddInstruction(HloInstruction::CreateGetTupleElement(lhs_shape, in, 0));
  auto w1 =
      b.AddInstruction(HloInstruction::CreateGetTupleElement(rhs_shape, in, 1));

  DotDimensionNumbers dot_dnums;
  dot_dnums.add_lhs_contracting_dimensions(1);
  dot_dnums.add_rhs_contracting_dimensions(0);
  auto dot_inst = b.AddInstruction(HloInstruction::CreateDot(
      lhs_shape, in1, w1, dot_dnums, DefaultPrecisionConfig(2)));

  hlo_module->AddEntryComputation(b.Build());

  CompilerAnnotations annotations(hlo_module.get());

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(hlo_module.get()).ValueOrDie());

  const HloInstruction* dot = dot_inst;

  ASSERT_EQ(annotations.tensor_allocation_map.size(), 2);

  auto t = annotations.tensor_allocation_map.at(std::make_pair(in, 0));
  EXPECT_EQ(t.tgt, dot);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 2);

  t = annotations.tensor_allocation_map.at(std::make_pair(in, 1));
  EXPECT_EQ(t.tgt, dot);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 2);
}

// Check it goes through while instructions
TEST_F(AllocationFinderTest, FindWhileTensorAllocations) {
  auto hlo_module = CreateNewModule();

  Shape counter_shape = ShapeUtil::MakeShape(S32, {});
  Shape input_shape = ShapeUtil::MakeShape(F32, {2});
  Shape weight_shape = ShapeUtil::MakeShape(F32, {2, 2});
  Shape tuple_shape =
      ShapeUtil::MakeTupleShape({counter_shape, input_shape, weight_shape});

  const HloInstruction* dot_inst;
  const HloInstruction* body_param;

  /* Create while condition */
  HloComputation* comp_cond;
  {
    auto builder_cond = HloComputation::Builder(TestName());
    auto tuple = builder_cond.AddInstruction(
        HloInstruction::CreateParameter(0, tuple_shape, "cond_tuple"));
    auto limit = builder_cond.AddInstruction(
        HloInstruction::CreateConstant(LiteralUtil::CreateR0<int32>(10)));
    auto c = builder_cond.AddInstruction(HloInstruction::CreateGetTupleElement(
        ShapeUtil::MakeShape(S32, {}), tuple, 0));
    builder_cond.AddInstruction(HloInstruction::CreateBinary(
        ShapeUtil::MakeShape(PRED, {}), HloOpcode::kLt, c, limit));

    comp_cond = hlo_module->AddEmbeddedComputation(builder_cond.Build());
  }

  /* Create while body */
  HloComputation* comp_body;
  {
    auto builder_body = HloComputation::Builder(TestName());
    auto tuple = builder_body.AddInstruction(
        HloInstruction::CreateParameter(0, tuple_shape, "body_tuple"));
    auto c = builder_body.AddInstruction(
        HloInstruction::CreateGetTupleElement(counter_shape, tuple, 0));
    auto in = builder_body.AddInstruction(
        HloInstruction::CreateGetTupleElement(input_shape, tuple, 1));
    auto w = builder_body.AddInstruction(
        HloInstruction::CreateGetTupleElement(weight_shape, tuple, 2));
    auto one = builder_body.AddInstruction(
        HloInstruction::CreateConstant(LiteralUtil::CreateR0<int32>(1)));
    auto new_c = builder_body.AddInstruction(
        HloInstruction::CreateBinary(c->shape(), HloOpcode::kAdd, c, one));

    DotDimensionNumbers dot_dnums;
    dot_dnums.add_lhs_contracting_dimensions(1);
    dot_dnums.add_rhs_contracting_dimensions(0);
    auto new_in = builder_body.AddInstruction(HloInstruction::CreateDot(
        input_shape, in, w, dot_dnums, DefaultPrecisionConfig(2)));

    dot_inst = new_in;
    body_param = tuple;

    builder_body.AddInstruction(
        HloInstruction::CreateTuple({new_c, new_in, w}));

    comp_body = hlo_module->AddEmbeddedComputation(builder_body.Build());
  }

  /* Create main computation */
  auto builder_main = HloComputation::Builder(TestName());
  auto c = builder_main.AddInstruction(
      HloInstruction::CreateParameter(0, counter_shape, "counter"));
  auto in = builder_main.AddInstruction(
      HloInstruction::CreateParameter(1, input_shape, "in"));
  auto w = builder_main.AddInstruction(
      HloInstruction::CreateParameter(2, weight_shape, "weight"));

  auto init =
      builder_main.AddInstruction(HloInstruction::CreateTuple({c, in, w}));

  auto main = builder_main.AddInstruction(
      HloInstruction::CreateWhile(tuple_shape, comp_cond, comp_body, init));

  builder_main.AddInstruction(HloInstruction::CreateTuple({main}));

  hlo_module->AddEntryComputation(builder_main.Build());

  CompilerAnnotations annotations(hlo_module.get());

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(hlo_module.get()).ValueOrDie());

  ASSERT_EQ(annotations.tensor_allocation_map.size(), 4);

  auto t = annotations.tensor_allocation_map.at(std::make_pair(in, 0));
  EXPECT_EQ(t.tgt, dot_inst);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 4);

  t = annotations.tensor_allocation_map.at(std::make_pair(w, 0));
  EXPECT_EQ(t.tgt, dot_inst);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 4);

  t = annotations.tensor_allocation_map.at(std::make_pair(body_param, 1));
  EXPECT_EQ(t.tgt, dot_inst);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 2);

  t = annotations.tensor_allocation_map.at(std::make_pair(body_param, 2));
  EXPECT_EQ(t.tgt, dot_inst);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 2);
}

// Check basic parameter matching
TEST_F(AllocationFinderTest, TraverseDimShuffleAndReshapeAllocations) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f16[1,16,16,2] parameter(0)
  p1 = f16[3,3,4,2] parameter(1)

  p1_t = f16[3,3,2,4] transpose(p1), dimensions={2,3}

  conv = f16[1,16,16,4] convolution(p0, p1_t), window={size=3x3 pad=1_1x1_1}, dim_labels=b01f_01io->b01f

  ROOT t = (f16[1,16,16,4]) tuple(conv)
}

)";

  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(0);
  config.set_resource_update_to_input_index({0});
  auto module = ParseHloString(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* module0 = module.ValueOrDie().get();

  const auto* root = module0->entry_computation()->root_instruction();
  const auto* conv = root->operand(0);
  const auto* ip0 = conv->operand(0);
  const auto* trans = conv->operand(1);
  const auto* ip1 = trans->operand(0);

  CompilerAnnotations annotations(module0);

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(module0).ValueOrDie());

  ASSERT_EQ(annotations.tensor_allocation_map.size(), 2);

  auto t = annotations.tensor_allocation_map.at(std::make_pair(ip0, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 0ll);
  EXPECT_EQ(t.backward_path.size(), 1);
  EXPECT_EQ(t.backward_path[0], ip0);

  t = annotations.tensor_allocation_map.at(std::make_pair(ip1, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 1ll);
  EXPECT_EQ(t.backward_path.size(), 2);
  EXPECT_EQ(t.backward_path[0], ip1);
  EXPECT_EQ(t.backward_path[1], trans);
}

// Check it goes through call sites
TEST_F(AllocationFinderTest, FindDoesntTraceThroughInvalidCalls) {
  Shape input_shape = ShapeUtil::MakeShape(F32, {1, 10, 10, 2});
  Shape half_shape = ShapeUtil::MakeShape(F32, {1, 10, 10, 1});
  Shape weight_shape = ShapeUtil::MakeShape(F32, {3, 3, 2, 1});

  Shape conv_shape = ShapeInference::InferConvolveShape(
                         input_shape, weight_shape, /*feature_group_count=*/1,
                         GetConv1Window(), GetConvDimensions())
                         .ConsumeValueOrDie();

  /* Create sub-computation which contains an unacceptable op */
  auto builder_sub = HloComputation::Builder(TestName());
  HloInstruction* op0_sub = builder_sub.AddInstruction(
      HloInstruction::CreateParameter(0, input_shape, "input"));
  HloInstruction* op1_sub = builder_sub.AddInstruction(
      HloInstruction::CreateConstant(Literal::CreateFromShape(half_shape)));
  HloInstruction* op2_sub = builder_sub.AddInstruction(
      HloInstruction::CreateConcatenate(input_shape, {op0_sub, op1_sub}, 3));
  auto computation_sub = builder_sub.Build();

  /* Create main computation */
  auto builder_main = HloComputation::Builder(TestName());
  HloInstruction* op0 = builder_main.AddInstruction(
      HloInstruction::CreateParameter(0, half_shape, "op0"));
  HloInstruction* op1 = builder_main.AddInstruction(
      HloInstruction::CreateParameter(1, weight_shape, "op1"));
  HloInstruction* call = builder_main.AddInstruction(
      HloInstruction::CreateCall(input_shape, {op0}, computation_sub.get()));
  HloInstruction* conv =
      builder_main.AddInstruction(HloInstruction::CreateConvolve(
          conv_shape, call, op1, /*feature_group_count=*/1, GetConv1Window(),
          GetConvDimensions(), DefaultPrecisionConfig(2)));

  builder_main.AddInstruction(HloInstruction::CreateTuple({conv}));

  auto computation_main = builder_main.Build();

  auto hlo_module = CreateNewModule();
  hlo_module->AddEmbeddedComputation(std::move(computation_sub));
  hlo_module->AddEntryComputation(std::move(computation_main));

  CompilerAnnotations annotations(hlo_module.get());

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(hlo_module.get()).ValueOrDie());

  ASSERT_EQ(annotations.tensor_allocation_map.size(), 1);
  auto t1 = annotations.tensor_allocation_map.at(std::make_pair(op1, 0));
  EXPECT_EQ(t1.tgt, conv);
  EXPECT_EQ(t1.input_index, 1ll);
  EXPECT_EQ(t1.backward_path.size(), 1);
}

TEST_F(AllocationFinderTest, BiasAdd1) {
  std::string hlo = R"(
HloModule top

_pop_op_biasadd {
  arg_0 = f16[1,16,16,4] parameter(0)
  arg_1 = f16[4] parameter(1)
  bcast = f16[1,16,16,4] broadcast(arg_1), dimensions={3}
  ROOT %add = f16[1,16,16,4] add(arg_0, bcast)
}

ENTRY c1 {
  p0 = f16[1,16,16,2] parameter(0)
  p1 = f16[3,3,2,4] parameter(1)
  p2 = f16[4] parameter(2)

  conv = f16[1,16,16,4] convolution(p0, p1), window={size=3x3 pad=1_1x1_1}, dim_labels=b01f_01io->b01f
  call = f16[1,16,16,64] call(conv, p2), to_apply=_pop_op_biasadd

  ROOT t = (f16[1,16,16,4]) tuple(%call)
}

)";

  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(2);
  config.set_resource_update_to_input_index({0});
  auto module = ParseHloString(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* module0 = module.ValueOrDie().get();

  const auto* root = module0->entry_computation()->root_instruction();
  const auto* call = root->operand(0);
  const auto* conv = call->operand(0);
  const auto* ip0 = conv->operand(0);
  const auto* ip1 = conv->operand(1);
  const auto* ip2 = call->operand(1);

  CompilerAnnotations annotations(module0);

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(module0).ValueOrDie());

  // Will have both of the convolution parameters
  ASSERT_EQ(annotations.tensor_allocation_map.size(), 2);

  auto t = annotations.tensor_allocation_map.at(std::make_pair(ip0, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 0);

  t = annotations.tensor_allocation_map.at(std::make_pair(ip1, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 1);

  ForwardAllocation fwd_finder(annotations);
  EXPECT_TRUE(fwd_finder.Run(module0).ValueOrDie());

  // We have added one new entry for the bias add
  ASSERT_EQ(annotations.tensor_allocation_map.size(), 3);

  t = annotations.tensor_allocation_map.at(std::make_pair(ip2, 0));
  EXPECT_EQ(t.tgt, call);
  EXPECT_EQ(t.input_index, 1);
  EXPECT_EQ(t.layout, conv);
}

TEST_F(AllocationFinderTest, BiasAddAndMultiply) {
  std::string hlo = R"(
HloModule top

_pop_op_biasadd {
  arg_0 = f16[1,16,16,4] parameter(0)
  arg_1 = f16[4] parameter(1)
  bcast = f16[1,16,16,4] broadcast(arg_1), dimensions={3}
  ROOT %add = f16[1,16,16,4] add(arg_0, bcast)
}

_pop_op_biasadd.1 {
  arg_0 = f16[1,16,16,4] parameter(0)
  arg_1 = f16[4] parameter(1)
  bcast = f16[1,16,16,4] broadcast(arg_1), dimensions={3}
  ROOT %add = f16[1,16,16,4] add(arg_0, bcast)
}

ENTRY c1 {
  p0 = f16[1,16,16,2] parameter(0)
  p1 = f16[3,3,2,4] parameter(1)
  p2 = f16[4] parameter(2)
  p3 = f16[4] parameter(3)

  conv = f16[1,16,16,4] convolution(p0, p1), window={size=3x3 pad=1_1x1_1}, dim_labels=b01f_01io->b01f
  call = f16[1,16,16,64] call(conv, p2), to_apply=_pop_op_biasadd
  call.1 = f16[1,16,16,64] call(call, p3), to_apply=_pop_op_biasadd.1

  ROOT t = (f16[1,16,16,4]) tuple(call.1)
}

)";

  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(2);
  config.set_resource_update_to_input_index({0});
  auto module = ParseHloString(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* module0 = module.ValueOrDie().get();

  const auto* root = module0->entry_computation()->root_instruction();
  const auto& call1 = root->operand(0);
  const auto* call = call1->operand(0);
  const auto* conv = call->operand(0);
  const auto* ip0 = conv->operand(0);
  const auto* ip1 = conv->operand(1);
  const auto* ip2 = call->operand(1);
  const auto* ip3 = call1->operand(1);

  CompilerAnnotations annotations(module0);

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(module0).ValueOrDie());

  // Will have both of the convolution parameters
  ASSERT_EQ(annotations.tensor_allocation_map.size(), 2);

  auto t = annotations.tensor_allocation_map.at(std::make_pair(ip0, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 0);

  t = annotations.tensor_allocation_map.at(std::make_pair(ip1, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 1);

  ForwardAllocation fwd_finder(annotations);
  EXPECT_TRUE(fwd_finder.Run(module0).ValueOrDie());

  // We have added two new entries to the map for the 2 bias add ops
  ASSERT_EQ(annotations.tensor_allocation_map.size(), 4);

  t = annotations.tensor_allocation_map.at(std::make_pair(ip2, 0));
  EXPECT_EQ(t.tgt, call);
  EXPECT_EQ(t.input_index, 1);
  EXPECT_EQ(t.layout, conv);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 0);

  t = annotations.tensor_allocation_map.at(std::make_pair(ip3, 0));
  EXPECT_EQ(t.tgt, call1);
  EXPECT_EQ(t.input_index, 1);
  EXPECT_EQ(t.layout, conv);
  EXPECT_EQ(t.forward_path.size(), 1);
  EXPECT_EQ(t.forward_path[0], call);
  EXPECT_EQ(t.backward_path.size(), 0);
}

TEST_F(AllocationFinderTest, BiasAddWithPath) {
  std::string hlo = R"(
HloModule top

_pop_op_biasadd {
  %arg_0 = f16[1,16,16,4] parameter(0)
  %arg_1 = f16[4] parameter(1)
  bcast = f16[1,16,16,4] broadcast(arg_1), dimensions={3}
  ROOT %add = f16[1,16,16,4] add(arg_0, bcast)
}

ENTRY c1 {
  p0 = f16[1,16,16,2] parameter(0)
  p1 = f16[3,3,2,4] parameter(1)
  p2 = f16[2,2] parameter(2)

  p2_r = f16[4] reshape(p2)

  conv = f16[1,16,16,4] convolution(p0, p1), window={size=3x3 pad=1_1x1_1}, dim_labels=b01f_01io->b01f
  call = f16[1,16,16,64] call(conv, p2_r), to_apply=_pop_op_biasadd

  ROOT t = (f16[1,16,16,4]) tuple(call)
}

)";

  auto config = GetModuleConfigForTest();
  config.set_resource_input_count(2);
  config.set_resource_update_to_input_index({0});
  auto module = ParseHloString(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* module0 = module.ValueOrDie().get();

  const auto* root = module0->entry_computation()->root_instruction();
  const auto* call = root->operand(0);
  const auto* conv = call->operand(0);
  const auto* ip0 = conv->operand(0);
  const auto* ip1 = conv->operand(1);
  const auto* reshape = call->operand(1);
  const auto* ip2 = reshape->operand(0);

  CompilerAnnotations annotations(module0);

  AllocationFinder finder(annotations);
  EXPECT_TRUE(finder.Run(module0).ValueOrDie());

  // Will have both of the convolution parameters
  ASSERT_EQ(annotations.tensor_allocation_map.size(), 2);

  auto t = annotations.tensor_allocation_map.at(std::make_pair(ip0, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 0);

  t = annotations.tensor_allocation_map.at(std::make_pair(ip1, 0));
  EXPECT_EQ(t.tgt, conv);
  EXPECT_EQ(t.input_index, 1);

  ForwardAllocation fwd_finder(annotations);
  EXPECT_TRUE(fwd_finder.Run(module0).ValueOrDie());

  // We have added one new entry for the bias add
  ASSERT_EQ(annotations.tensor_allocation_map.size(), 3);

  t = annotations.tensor_allocation_map.at(std::make_pair(ip2, 0));
  EXPECT_EQ(t.tgt, call);
  EXPECT_EQ(t.input_index, 1);
  EXPECT_EQ(t.layout, conv);
  EXPECT_EQ(t.forward_path.size(), 0);
  EXPECT_EQ(t.backward_path.size(), 1);
  EXPECT_EQ(t.backward_path[0], reshape);
}

// TODO:
// - can forward path traverse TUPLEs
// - can forward path traverse in-place ops
// - can forward path traverse elementwise ops
// - is forward path rejected when going through non-layout preserving inputs
// - can forward and backward paths start on TUPLE Parameters
// - can forward and backward paths start on TUPLE and non-TUPLE InFeeds

}  // namespace
}  // namespace poplarplugin
}  // namespace xla
