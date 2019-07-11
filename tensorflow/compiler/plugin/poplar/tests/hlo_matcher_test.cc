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

#include "tensorflow/compiler/plugin/poplar/driver/tools/hlo_matcher.h"
#include "tensorflow/compiler/plugin/poplar/driver/compiler_annotations.h"

#include "tensorflow/compiler/xla/service/hlo_parser.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"

namespace xla {
namespace poplarplugin {
namespace {

using HloMatcherTest = HloTestBase;

class TestMatcher : public HloMatcher {
 public:
  TestMatcher(const std::vector<HloMatcherPattern>& patterns,
              CompilerAnnotations& annotations, bool root_only,
              bool requires_unique_sharding = true,
              unsigned int look_through_depth = 0)
      : HloMatcher(patterns, annotations, root_only, requires_unique_sharding,
                   look_through_depth) {}

 private:
  bool HandleMatch(HloMatcherMatched& match,
                   const absl::optional<int64> sharding_device) override {
    auto pattern = patterns_[match.pattern_idx];
    OutlineExpressionFromComputation(match, pattern.GetType(), sharding_device);
    replace_count++;
    const int replaced_instructions =
        match.instruction_mapping.size() - pattern.GetInputs().size();
    match_count.push_back(replaced_instructions);
    return true;
  }

 public:
  int replace_count = 0;
  std::vector<unsigned int> match_pattern;
  std::vector<unsigned int> match_count;
};

TEST_F(HloMatcherTest, MatchTestSimpleReplacementTwice) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[10,10] parameter(0)
  p1 = f32[10,10] parameter(1)
  p2 = f32[10,10] parameter(2)
  add1 = f32[10,10] add(p0, p1)
  add2 = f32[10,10] add(add1, p2)
  ROOT root = (f32[10,10]) tuple(add2)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({1, 2}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());
  ASSERT_EQ(2, matcher.replace_count);
  EXPECT_EQ(6, hlo_module->entry_computation()->instruction_count());
}

TEST_F(HloMatcherTest, MatchTestExplicitInputs) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[10,10] parameter(0)
  p1 = f32[10,10] parameter(1)
  add1 = f32[10,10] add(p0, p0)
  add2 = f32[10,10] add(p0, p1)
  ROOT root = (f32[10,10], f32[10,10]) tuple(add1, add2)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({1, 2}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());
  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(5, hlo_module->entry_computation()->instruction_count());
}

TEST_F(HloMatcherTest, MatchTestTwoPatterns) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[10,10] parameter(0)
  p1 = f32[10,10] parameter(1)
  p2 = f32[10] parameter(2)
  b1 = f32[10,10] broadcast(p2), dimensions={1}
  add1 = f32[10,10] add(p0, p1), metadata={op_type="Add" op_name="long/add1"}
  add2 = f32[10,10] add(add1, b1), metadata={op_type="Add" op_name="long/add2"}
  ROOT root = (f32[10,10]) tuple(add2)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("add"),
      PatternMetaTarget(0),
      PatternInputs({2, 3}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({3, 1})},
        {HloOpcode::kBroadcast, NodeOperands({2})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    ),
    HloMatcherPattern(
      PatternType("add"),
      PatternMetaTarget(0),
      PatternInputs({1, 2}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);
  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());

  ASSERT_EQ(2, matcher.replace_count);
  EXPECT_EQ(6, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* call_inst = comp->root_instruction()->operand(0);
  EXPECT_EQ("add", call_inst->fused_instructions_computation()->name());

  EXPECT_EQ("long/add2", call_inst->metadata().op_name());
  EXPECT_EQ("long/add1", call_inst->operand(1)->metadata().op_name());
}

TEST_F(HloMatcherTest, MatchTestGraphWithPathsJoining) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[10,10] parameter(0)
  p1 = f32[10,10] parameter(1)
  p2 = f32[10] parameter(2)
  b1 = f32[10,10] broadcast(p2), dimensions={1},
       metadata={op_type="Broadcast" op_name="long/bc"},
       sharding={maximal device=1}
  sub1 = f32[10,10] subtract(p0, b1)
  add1 = f32[10,10] add(p1, b1)
  sub2 = f32[10,10] subtract(add1, sub1)
  ROOT root = (f32[10,10]) tuple(sub2)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("fuse"),
      PatternMetaTarget(1),
      PatternInputs({2, 3}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({3, 1})},
        {HloOpcode::kBroadcast, NodeOperands({2})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());
  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(8, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* call_inst = comp->root_instruction()->operand(0)->operand(0);
  EXPECT_EQ("fuse", call_inst->fused_instructions_computation()->name());

  EXPECT_EQ("long/bc", call_inst->metadata().op_name());
  EXPECT_TRUE(call_inst->has_sharding());
  EXPECT_EQ(1, call_inst->sharding().UniqueDevice());
}

TEST_F(HloMatcherTest, MatchTestGraphWithPathsJoiningOnMultipleMatchNode) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[10,10] parameter(0)
  p1 = f32[10,10] parameter(1)
  p2 = f32[10] parameter(2)
  b1 = f32[10,10] broadcast(p2), dimensions={1}
  add1 = f32[10,10] add(p0, b1)
  add2 = f32[10,10] add(p1, b1)
  sub1 = f32[10,10] subtract(add1, add2)
  ROOT root = (f32[10,10]) tuple(sub1)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({2, 3}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({3, 1})},
        {HloOpcode::kBroadcast, NodeOperands({2})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());
  EXPECT_EQ(2, matcher.replace_count);
  EXPECT_EQ(7, hlo_module->entry_computation()->instruction_count());
}

TEST_F(HloMatcherTest, MatchTestGraphWithMatchedByNonRemovedNodes) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[10,10] parameter(0)
  p1 = f32[10,10] parameter(1)
  p2 = f32[10] parameter(2)
  b1 = f32[10,10] broadcast(p2), dimensions={1}
  sub1 = f32[10,10] subtract(p0, p1)
  add1 = f32[10,10] add(p1, b1)
  sub2 = f32[10,10] subtract(add1, sub1)
  ROOT root = (f32[10,10]) tuple(sub2)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({3, 2, 4}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kSubtract, NodeOperands({1, 3})},
        {HloOpcode::kAdd, NodeOperands({4, 2})},
        {HloOpcode::kBroadcast, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());
  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(2, matcher.match_count[0]);
  EXPECT_EQ(7, hlo_module->entry_computation()->instruction_count());
}

TEST_F(HloMatcherTest, OutlineWithInstructionsNotRemoved) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[10] parameter(0)
  p1 = f32[] constant(1.0)
  b1 = f32[10] broadcast(p1), dimensions={}
  sub1 = f32[10] subtract(p0, b1)
  add1 = f32[10] add(p0, b1)
  sub2 = f32[10] subtract(add1, sub1)
  ROOT root = (f32[10]) tuple(sub2)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("abc"),
      PatternMetaTarget(0),
      PatternInputs({3}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kSubtract, NodeOperands({3, 1})},
        {HloOpcode::kBroadcast, NodeOperands({2})},
        {HloOpcode::kConstant, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());
  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(7, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* call_inst = comp->root_instruction()->operand(0)->operand(1);
  EXPECT_EQ("abc", call_inst->fused_instructions_computation()->name());
}

TEST_F(HloMatcherTest, LookThroughAssociativeOps) {
  const unsigned int look_through_depth = 2;
  Shape shape = ShapeUtil::MakeShape(F32, {});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  auto c1 = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(10.f)));
  auto sub = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kSubtract, i1, c1));
  auto add = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i2, sub));
  builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, add, c1));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("abc"),
      PatternMetaTarget(0),
      PatternInputs({3, 2}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloOpcode::kSubtract, NodeOperands({3, 2})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true, look_through_depth);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(1, matcher.replace_count);
  EXPECT_EQ(5, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* root = comp->root_instruction();
  // Expect that root is add now
  EXPECT_EQ(root, add);

  // Expect that operand 1 of add has changed to a call
  EXPECT_EQ(add->operand(1)->opcode(), HloOpcode::kFusion);
  auto* call_inst = comp->root_instruction()->operand(1);
  // Expect the name
  EXPECT_EQ("abc", call_inst->fused_instructions_computation()->name());
  // Expect the parameters
  EXPECT_EQ(call_inst->operand(0), i1);
  EXPECT_EQ(call_inst->operand(1), c1);
  // Expect the call body
  auto* call_root =
      call_inst->fused_instructions_computation()->root_instruction();
  EXPECT_EQ(call_root->opcode(), HloOpcode::kAdd);
  EXPECT_EQ(call_root->operand(1)->opcode(), HloOpcode::kParameter);
  EXPECT_EQ(call_root->operand(1)->parameter_number(), 1);
  auto* call_sub = call_root->operand(0);
  EXPECT_EQ(call_sub->opcode(), HloOpcode::kSubtract);
  EXPECT_EQ(call_sub->operand(0)->opcode(), HloOpcode::kParameter);
  EXPECT_EQ(call_sub->operand(0)->parameter_number(), 0);
  EXPECT_EQ(call_sub->operand(1)->opcode(), HloOpcode::kParameter);
  EXPECT_EQ(call_sub->operand(1)->parameter_number(), 1);
}

TEST_F(HloMatcherTest, LookThroughAssociativeOpsParameter) {
  const unsigned int look_through_depth = 2;
  Shape shape = ShapeUtil::MakeShape(F32, {});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  auto c1 = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(10.f)));
  auto sub = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kSubtract, i1, c1));
  auto add = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i2, sub));
  builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, add, c1));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("abc"),
      PatternMetaTarget(0),
      PatternInputs({2, 1}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloOpcode::kSubtract, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true, look_through_depth);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(1, matcher.replace_count);
  EXPECT_EQ(6, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* root = comp->root_instruction();
  // Expect that root is add now
  EXPECT_EQ(root, add);

  // Expect that operand 1 of add has changed to a call
  EXPECT_EQ(add->operand(1)->opcode(), HloOpcode::kFusion);
  auto* call_inst = comp->root_instruction()->operand(1);
  // Expect the name
  EXPECT_EQ("abc", call_inst->fused_instructions_computation()->name());
  // Expect the parameters
  EXPECT_EQ(call_inst->operand(0), c1);
  EXPECT_EQ(call_inst->operand(1), sub);
  // Expect the call body
  auto* call_root =
      call_inst->fused_instructions_computation()->root_instruction();
  EXPECT_EQ(call_root->opcode(), HloOpcode::kAdd);
  EXPECT_EQ(call_root->operand(0)->opcode(), HloOpcode::kParameter);
  EXPECT_EQ(call_root->operand(0)->parameter_number(), 1);
  EXPECT_EQ(call_root->operand(1)->opcode(), HloOpcode::kParameter);
  EXPECT_EQ(call_root->operand(1)->parameter_number(), 0);
}

TEST_F(HloMatcherTest, LookThroughAssociativeOpsLongerChain) {
  const unsigned int look_through_depth = 6;
  Shape shape = ShapeUtil::MakeShape(F32, {});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  auto c1 = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(10.f)));
  auto sub = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kSubtract, i1, c1));
  auto mul1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, sub));
  auto mul2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul1));
  auto mul3 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul2));
  auto mul4 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul3));
  auto mul5 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul4));
  auto mul6 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul5));
  builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, mul6, c1));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("abc"),
      PatternMetaTarget(0),
      PatternInputs({3, 2}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kMultiply, NodeOperands({1, 2})},
        {HloOpcode::kSubtract, NodeOperands({3, 2})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true, look_through_depth);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(1, matcher.replace_count);
  EXPECT_EQ(10, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* root = comp->root_instruction();
  // Expect that root is mul1 now
  EXPECT_EQ(root, mul1);

  // Expect that operand 1 of mul1 has changed to a call
  EXPECT_EQ(mul1->operand(1)->opcode(), HloOpcode::kFusion);
  auto* call_inst = comp->root_instruction()->operand(1);
  // Expect the name
  EXPECT_EQ("abc", call_inst->fused_instructions_computation()->name());
  // Expect the parameters
  EXPECT_EQ(call_inst->operand(0), i1);
  EXPECT_EQ(call_inst->operand(1), c1);
  // Expect the call body
  auto* call_root =
      call_inst->fused_instructions_computation()->root_instruction();
  EXPECT_EQ(call_root->opcode(), HloOpcode::kMultiply);
  EXPECT_EQ(call_root->operand(1)->opcode(), HloOpcode::kParameter);
  EXPECT_EQ(call_root->operand(1)->parameter_number(), 1);
  auto* call_sub = call_root->operand(0);
  EXPECT_EQ(call_sub->opcode(), HloOpcode::kSubtract);
  EXPECT_EQ(call_sub->operand(0)->opcode(), HloOpcode::kParameter);
  EXPECT_EQ(call_sub->operand(0)->parameter_number(), 0);
  EXPECT_EQ(call_sub->operand(1)->opcode(), HloOpcode::kParameter);
  EXPECT_EQ(call_sub->operand(1)->parameter_number(), 1);
}

TEST_F(HloMatcherTest, LookThroughAssociativeOpsChainTooLong) {
  const unsigned int look_through_depth = 5;
  Shape shape = ShapeUtil::MakeShape(F32, {});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  auto c1 = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(10.f)));
  auto sub = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kSubtract, i1, c1));
  auto mul1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, sub));
  auto mul2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul1));
  auto mul3 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul2));
  auto mul4 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul3));
  auto mul5 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul4));
  auto mul6 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul5));
  builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, mul6, c1));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("abc"),
      PatternMetaTarget(0),
      PatternInputs({2, 1}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kMultiply, NodeOperands({1, 2})},
        {HloOpcode::kSubtract, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true, look_through_depth);

  EXPECT_FALSE(matcher.Run(hlo_module.get()).ValueOrDie());
}

TEST_F(HloMatcherTest, LookThroughAssociativeOpsPartialInChainUsed) {
  const unsigned int look_through_depth = 6;
  Shape shape = ShapeUtil::MakeShape(F32, {});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  auto c1 = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(10.f)));
  auto sub = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kSubtract, i1, c1));
  auto mul1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, sub));
  auto mul2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul1));
  auto mul3 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul2));
  auto mul4 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul3));
  auto mul5 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul4));
  auto mul6 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, mul5));
  auto mul7 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, mul6, c1));
  builder.AddInstruction(HloInstruction::CreateTuple({mul3, mul7}));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("abc"),
      PatternMetaTarget(0),
      PatternInputs({2, 1}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kMultiply, NodeOperands({1, 2})},
        {HloOpcode::kSubtract, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true, look_through_depth);

  EXPECT_FALSE(matcher.Run(hlo_module.get()).ValueOrDie());
}

TEST_F(HloMatcherTest, LookThroughAssociativeOpsDifferentAssociativitySets) {
  const unsigned int look_through_depth = 2;
  Shape shape = ShapeUtil::MakeShape(F32, {});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  auto c1 = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(10.f)));
  auto sub = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kSubtract, i1, c1));
  auto add = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i2, sub));
  auto mul = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kMultiply, i2, add));
  builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, mul, c1));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("abc"),
      PatternMetaTarget(0),
      PatternInputs({2, 1}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloOpcode::kSubtract, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true, look_through_depth);

  EXPECT_FALSE(matcher.Run(hlo_module.get()).ValueOrDie());
}

TEST_F(HloMatcherTest, LookThroughAssociativeOpsRootNonAssociative) {
  const unsigned int look_through_depth = 5;
  Shape shape = ShapeUtil::MakeShape(F32, {});
  Shape shape2 = ShapeUtil::MakeShape(F32, {2});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  auto c1 = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(10.f)));
  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, c1));
  auto add2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, add1, i2));
  builder.AddInstruction(HloInstruction::CreateBroadcast(shape2, add2, {}));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("abc"),
      PatternMetaTarget(0),
      PatternInputs({2}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kBroadcast, NodeOperands({1})},
        {HloOpcode::kAdd, NodeOperands({2, 3})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloOpcode::kConstant, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true, look_through_depth);

  EXPECT_FALSE(matcher.Run(hlo_module.get()).ValueOrDie());
}

TEST_F(HloMatcherTest, PatternNoOutputs) {
  try {
    // clang-format off
    std::vector<HloMatcherPattern> patterns = {
      HloMatcherPattern(
        PatternType("abc"),
        PatternMetaTarget(0),
        PatternInputs({2}),
        PatternOutputs({}),
        Pattern({
          {HloOpcode::kBroadcast, NodeOperands({1})},
          {HloOpcode::kAdd, NodeOperands({2, 3})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloOpcode::kConstant, NodeOperands({})}
        })
      )
    };
    // clang-format on
    FAIL() << "Expected invalid_argument throw.";
  } catch (const std::invalid_argument& ia) {
    EXPECT_EQ(std::string(ia.what()),
              "[Pattern abc] Pattern has no outputs, at least one required.");
  } catch (...) {
    FAIL() << "Expected invalid_argument throw.";
  }
}

TEST_F(HloMatcherTest, PatternDuplicateParams) {
  try {
    // clang-format off
    std::vector<HloMatcherPattern> patterns = {
      HloMatcherPattern(
        PatternType("abc"),
        PatternMetaTarget(0),
        PatternInputs({1, 1}),
        PatternOutputs({0}),
        Pattern({
          {HloOpcode::kAdd, NodeOperands({1, 2})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
        })
      )
    };
    // clang-format on
    FAIL() << "Expected invalid_argument throw.";
  } catch (const std::invalid_argument& ia) {
    EXPECT_EQ(std::string(ia.what()),
              "[Pattern abc] Input with label 1 already defined. Pattern "
              "inputs need to be unique.");
  } catch (...) {
    FAIL() << "Expected invalid_argument throw.";
  }
}

TEST_F(HloMatcherTest, PatternDuplicateOutput) {
  try {
    // clang-format off
    std::vector<HloMatcherPattern> patterns = {
      HloMatcherPattern(
        PatternType("abc"),
        PatternMetaTarget(0),
        PatternInputs({2, 3}),
        PatternOutputs({0, 0}),
        Pattern({
          {HloOpcode::kAdd, NodeOperands({2, 1})},
          {HloOpcode::kAdd, NodeOperands({3, 3})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
        })
      )
    };
    // clang-format on
    FAIL() << "Expected invalid_argument throw.";
  } catch (const std::invalid_argument& ia) {
    EXPECT_EQ(std::string(ia.what()),
              "[Pattern abc] Output with label 0 already defined. Pattern "
              "outputs need to be unique.");
  } catch (...) {
    FAIL() << "Expected invalid_argument throw.";
  }
}

TEST_F(HloMatcherTest, PatternDisconnected) {
  try {
    // clang-format off
    std::vector<HloMatcherPattern> patterns = {
      HloMatcherPattern(
        PatternType("abc"),
        PatternMetaTarget(0),
        PatternInputs({2, 3}),
        PatternOutputs({0}),
        Pattern({
          {HloOpcode::kAdd, NodeOperands({2, 1})},
          {HloOpcode::kAdd, NodeOperands({2, 2})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
        })
      )
    };
    // clang-format on
    FAIL() << "Expected invalid_argument throw.";
  } catch (const std::invalid_argument& ia) {
    EXPECT_EQ(std::string(ia.what()),
              "[Pattern abc] Node with label 3 is disconnected from the graph. "
              "The graph needs to be connected.");
  } catch (...) {
    FAIL() << "Expected invalid_argument throw.";
  }
}

TEST_F(HloMatcherTest, PatternInvalidParamLabel) {
  try {
    // clang-format off
    std::vector<HloMatcherPattern> patterns = {
      HloMatcherPattern(
        PatternType("abc"),
        PatternMetaTarget(0),
        PatternInputs({2, 4}),
        PatternOutputs({0}),
        Pattern({
          {HloOpcode::kAdd, NodeOperands({2, 1})},
          {HloOpcode::kAdd, NodeOperands({3, 3})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
        })
      )
    };
    // clang-format on
    FAIL() << "Expected invalid_argument throw.";
  } catch (const std::invalid_argument& ia) {
    EXPECT_EQ(
        std::string(ia.what()),
        "[Pattern abc] Input with label 4 does not exist in the pattern.");
  } catch (...) {
    FAIL() << "Expected invalid_argument throw.";
  }
}

TEST_F(HloMatcherTest, PatternInvalidOutputLabel) {
  try {
    // clang-format off
    std::vector<HloMatcherPattern> patterns = {
      HloMatcherPattern(
        PatternType("abc"),
        PatternMetaTarget(0),
        PatternInputs({2, 3}),
        PatternOutputs({4}),
        Pattern({
          {HloOpcode::kAdd, NodeOperands({2, 1})},
          {HloOpcode::kAdd, NodeOperands({3, 3})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
        })
      )
    };
    // clang-format on
    FAIL() << "Expected invalid_argument throw.";
  } catch (const std::invalid_argument& ia) {
    EXPECT_EQ(
        std::string(ia.what()),
        "[Pattern abc] Output with label 4 does not exist in the pattern.");
  } catch (...) {
    FAIL() << "Expected invalid_argument throw.";
  }
}

TEST_F(HloMatcherTest, PatternInvalidPatternLabel) {
  try {
    // clang-format off
    std::vector<HloMatcherPattern> patterns = {
      HloMatcherPattern(
        PatternType("abc"),
        PatternMetaTarget(0),
        PatternInputs({2, 3}),
        PatternOutputs({0}),
        Pattern({
          {HloOpcode::kAdd, NodeOperands({2, 1})},
          {HloOpcode::kAdd, NodeOperands({3, 4})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
        })
      )
    };
    // clang-format on
    FAIL() << "Expected invalid_argument throw.";
  } catch (const std::invalid_argument& ia) {
    EXPECT_EQ(
        std::string(ia.what()),
        "[Pattern abc] Unknown node 4 which was not defined in the pattern.");
  } catch (...) {
    FAIL() << "Expected invalid_argument throw.";
  }
}

TEST_F(HloMatcherTest, PatternInvalidInplaceInput) {
  try {
    // clang-format off
    std::vector<HloMatcherPattern> patterns = {
      HloMatcherPattern(
        PatternType("abc"),
        PatternMetaTarget(0),
        PatternInputs({1, 2}),
        PatternInplaceInputs({0}),
        PatternOutputs({0}),
        Pattern({
          {HloOpcode::kAdd, NodeOperands({2, 1})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
        })
      )
    };
    // clang-format on
    FAIL() << "Expected invalid_argument throw.";
  } catch (const std::invalid_argument& ia) {
    EXPECT_EQ(
        std::string(ia.what()),
        "[Pattern abc] Inplace input with label 0 is not an inplace input to "
        "the pattern. Inplace inputs need to be inputs to the pattern.");
  } catch (...) {
    FAIL() << "Expected invalid_argument throw.";
  }
}

TEST_F(HloMatcherTest, TwoDisconnectedGraphs) {
  try {
    // clang-format off
    std::vector<HloMatcherPattern> patterns = {
      HloMatcherPattern(
        PatternType("abc"),
        PatternMetaTarget(0),
        PatternInputs({2, 3, 4, 5}),
        PatternOutputs({0, 1}),
        Pattern({
          {HloOpcode::kAdd, NodeOperands({2, 3})},
          {HloOpcode::kAdd, NodeOperands({4, 5})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
        })
      )
    };
    // clang-format on
    FAIL() << "Expected invalid_argument throw.";
  } catch (const std::invalid_argument& ia) {
    EXPECT_TRUE(std::string(ia.what()).find("Node with label"));
  } catch (...) {
    FAIL() << "Expected invalid_argument throw.";
  }
}

TEST_F(HloMatcherTest, InputWithInputs) {
  try {
    // clang-format off
    std::vector<HloMatcherPattern> patterns = {
      HloMatcherPattern(
        PatternType("test"),
        PatternMetaTarget(0),
        PatternInputs({1, 2, 3}),
        PatternOutputs({0}),
        Pattern({
          {HloOpcode::kAdd, NodeOperands({1, 2})},
          {HloOpcode::kAdd, NodeOperands({2, 3})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
          {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
        })
      )
    };
    // clang-format on
    FAIL() << "Expected invalid_argument throw.";
  } catch (const std::invalid_argument& ia) {
    EXPECT_EQ(std::string(ia.what()),
              "[Pattern test] Input with label 1 has an input - this is "
              "currently not supported.");
  } catch (...) {
    FAIL() << "Expected invalid_argument throw.";
  }
}

TEST_F(HloMatcherTest, MatchTestMultipleOutputs) {
  Shape shape = ShapeUtil::MakeShape(F32, {10, 10});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  auto i3 =
      builder.AddInstruction(HloInstruction::CreateParameter(2, shape, "in3"));
  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, i2));
  auto add2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i3, i2));

  builder.AddInstruction(HloInstruction::CreateTuple({add2, add1}));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({2, 3, 4}),
      PatternOutputs({0, 1}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({2, 3})},
        {HloOpcode::kAdd, NodeOperands({4, 3})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(1, matcher.replace_count);
  auto entry_computation = hlo_module->entry_computation();
  EXPECT_EQ(7, entry_computation->instruction_count());
  auto root = entry_computation->root_instruction();
  CHECK_EQ(root->operand(0)->opcode(), HloOpcode::kGetTupleElement);
  CHECK_EQ(root->operand(0)->tuple_index(), 1);
  CHECK_EQ(root->operand(1)->opcode(), HloOpcode::kGetTupleElement);
  CHECK_EQ(root->operand(1)->tuple_index(), 0);
}

TEST_F(HloMatcherTest, MatchTestMultipleOutputsMultipleMatches) {
  Shape shape = ShapeUtil::MakeShape(F32, {10, 10});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  auto i3 =
      builder.AddInstruction(HloInstruction::CreateParameter(2, shape, "in3"));
  auto i4 =
      builder.AddInstruction(HloInstruction::CreateParameter(3, shape, "in4"));
  auto i5 =
      builder.AddInstruction(HloInstruction::CreateParameter(4, shape, "in5"));
  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, i2));
  auto add2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i3, i2));
  auto add3 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i4, i2));
  auto add4 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i5, i2));

  builder.AddInstruction(HloInstruction::CreateTuple({add3, add2, add1, add4}));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({2, 3, 4}),
      PatternOutputs({0, 1}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({2, 3})},
        {HloOpcode::kAdd, NodeOperands({4, 3})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(2, matcher.replace_count);
  auto entry_computation = hlo_module->entry_computation();
  EXPECT_EQ(12, entry_computation->instruction_count());
  auto root = entry_computation->root_instruction();
  CHECK_EQ(root->operand(0)->opcode(), HloOpcode::kGetTupleElement);
  CHECK_EQ(root->operand(1)->opcode(), HloOpcode::kGetTupleElement);
  CHECK_EQ(root->operand(2)->opcode(), HloOpcode::kGetTupleElement);
  CHECK_EQ(root->operand(3)->opcode(), HloOpcode::kGetTupleElement);
}

TEST_F(HloMatcherTest, TestShardingSame) {
  Shape shape = ShapeUtil::MakeShape(F32, {10, 10});

  auto builder = HloComputation::Builder(TestName());

  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  i1->set_device_sharding(0);

  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  i2->set_device_sharding(0);

  auto i3 =
      builder.AddInstruction(HloInstruction::CreateParameter(2, shape, "in3"));
  i3->set_device_sharding(0);

  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, i2));
  add1->set_device_sharding(0);

  auto add2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, add1, i3));
  add2->set_device_sharding(0);

  auto tuple = builder.AddInstruction(HloInstruction::CreateTuple({add2}));
  tuple->set_device_sharding(0);

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({1, 2}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());

  ASSERT_EQ(2, matcher.replace_count);
  EXPECT_EQ(6, hlo_module->entry_computation()->instruction_count());
}

TEST_F(HloMatcherTest, TestShardingDifferentDontIgnoreSharding) {
  Shape shape = ShapeUtil::MakeShape(F32, {10, 10});

  auto builder = HloComputation::Builder(TestName());

  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  i1->set_device_sharding(0);

  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  i2->set_device_sharding(0);

  auto i3 =
      builder.AddInstruction(HloInstruction::CreateParameter(2, shape, "in3"));
  i3->set_device_sharding(0);

  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, i2));
  add1->set_device_sharding(0);

  auto add2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, add1, i3));
  add2->set_device_sharding(1);

  auto tuple = builder.AddInstruction(HloInstruction::CreateTuple({add2}));
  tuple->set_device_sharding(0);

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({2, 3, 4}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloOpcode::kAdd, NodeOperands({3, 4})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true);

  EXPECT_FALSE(matcher.Run(hlo_module.get()).ValueOrDie());
}

TEST_F(HloMatcherTest, TestShardingDifferentIgnoreSharding) {
  Shape shape = ShapeUtil::MakeShape(F32, {10, 10});

  auto builder = HloComputation::Builder(TestName());

  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  i1->set_device_sharding(0);

  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  i2->set_device_sharding(0);

  auto i3 =
      builder.AddInstruction(HloInstruction::CreateParameter(2, shape, "in3"));
  i3->set_device_sharding(0);

  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, i2));
  add1->set_device_sharding(0);

  auto add2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, add1, i3));
  add2->set_device_sharding(1);

  auto tuple = builder.AddInstruction(HloInstruction::CreateTuple({add2}));
  tuple->set_device_sharding(0);

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({2, 3, 4}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloOpcode::kAdd, NodeOperands({3, 4})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, false);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());

  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(5, hlo_module->entry_computation()->instruction_count());
}

TEST_F(HloMatcherTest, TestShardingIncomplete) {
  // In this test we provide incomplete sharding and check that it
  // gets applied to the whole pattern.
  Shape shape = ShapeUtil::MakeShape(F32, {10, 10});

  auto builder = HloComputation::Builder(TestName());

  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  i1->set_device_sharding(0);

  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  i2->set_device_sharding(0);

  auto i3 =
      builder.AddInstruction(HloInstruction::CreateParameter(2, shape, "in3"));
  i3->set_device_sharding(0);

  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, i2));
  add1->set_device_sharding(0);

  builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, add1, i3));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({2, 3, 4}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloOpcode::kAdd, NodeOperands({3, 4})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  auto entry = hlo_module->entry_computation();
  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(4, entry->instruction_count());
  EXPECT_EQ(0, *entry->root_instruction()->sharding_unique_device());
}

TEST_F(HloMatcherTest, TestShardingInputDifferentShard) {
  // In this test we provide incomplete sharding and inputs on different shards.
  Shape shape = ShapeUtil::MakeShape(F32, {10, 10});

  auto builder = HloComputation::Builder(TestName());

  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  i1->set_device_sharding(1);

  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  i2->set_device_sharding(2);

  auto i3 =
      builder.AddInstruction(HloInstruction::CreateParameter(2, shape, "in3"));
  i3->set_device_sharding(3);

  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, i2));
  add1->set_device_sharding(0);

  builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, add1, i3));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({2, 3, 4}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloOpcode::kAdd, NodeOperands({3, 4})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  auto entry = hlo_module->entry_computation();
  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(4, entry->instruction_count());
  EXPECT_EQ(0, *entry->root_instruction()->sharding_unique_device());
}

TEST_F(HloMatcherTest, TestShardingIgnoreConstSharding) {
  // In this test we provide incomplete sharding and inputs on different shards.
  Shape shape = ShapeUtil::MakeShape(F32, {});

  auto builder = HloComputation::Builder(TestName());

  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  i1->set_device_sharding(1);

  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  i2->set_device_sharding(2);

  auto c1 = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(1)));
  c1->set_device_sharding(3);

  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, i2));
  add1->set_device_sharding(0);

  builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, add1, c1));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({3, 4}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloOpcode::kAdd, NodeOperands({3, 4})},
        {HloOpcode::kConstant, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  auto entry = hlo_module->entry_computation();
  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(3, entry->instruction_count());
  EXPECT_EQ(0, *entry->root_instruction()->sharding_unique_device());
}

TEST_F(HloMatcherTest, TestShardingIgnoreWideConstSharding) {
  // In this test we provide incomplete sharding and inputs on different shards.
  Shape shape = ShapeUtil::MakeShape(F32, {10});

  auto builder = HloComputation::Builder(TestName());

  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  i1->set_device_sharding(1);

  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  i2->set_device_sharding(2);

  auto c1 = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::CreateR0<float>(1)));
  c1->set_device_sharding(3);
  auto b1 =
      builder.AddInstruction(HloInstruction::CreateBroadcast(shape, c1, {}));
  b1->set_device_sharding(4);

  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, i2));
  add1->set_device_sharding(0);

  builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, add1, b1));

  auto computation = builder.Build();

  auto hlo_module = CreateNewVerifiedModule();
  hlo_module->AddEntryComputation(std::move(computation));

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({4, 5}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloOpcode::kAdd, NodeOperands({4, 5})},
        {HloOpcode::kBroadcast, NodeOperands({3})},
        {HloOpcode::kConstant, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, false, true);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  auto entry = hlo_module->entry_computation();
  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(3, entry->instruction_count());
  EXPECT_EQ(0, *entry->root_instruction()->sharding_unique_device());
}

TEST_F(HloMatcherTest, MatchWithTokenDependencyAfter) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[10,10] parameter(0)
  p1 = f32[10,10] parameter(1)
  add1 = f32[10,10] add(p0, p1)
  after-all = token[] after-all(add1)
  add-dep = f32[10,10] add-dependency(p0, after-all)
  ROOT sub1 = f32[10,10] subtract(add-dep, p1)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
  std::vector<HloMatcherPattern> patterns = {
    HloMatcherPattern(
      PatternType("test"),
      PatternMetaTarget(0),
      PatternInputs({1, 2}),
      PatternOutputs({0}),
      Pattern({
        {HloOpcode::kAdd, NodeOperands({1, 2})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
        {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
      })
    )
  };
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());
  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(6, hlo_module->entry_computation()->instruction_count());

  // after-all is after the fused add
  auto* root = hlo_module->entry_computation()->root_instruction();
  auto* add_dep = root->operand(0);
  EXPECT_EQ(add_dep->opcode(), HloOpcode::kAddDependency);
  EXPECT_EQ(add_dep->operand(1)->opcode(), HloOpcode::kAfterAll);
  EXPECT_EQ(add_dep->operand(1)->operand(0)->opcode(), HloOpcode::kFusion);
}

TEST_F(HloMatcherTest, MatchWithTokenDependencyBefore) {
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[10,10] parameter(0)
  p1 = f32[10,10] parameter(1)
  sub1 = f32[10,10] subtract(p0, p1)
  after-all = token[] after-all(sub1)
  add-dep = f32[10,10] add-dependency(p0, after-all)
  ROOT add1 = f32[10,10] add(add-dep, sub1)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
std::vector<HloMatcherPattern> patterns = {
  HloMatcherPattern(
    PatternType("test"),
    PatternMetaTarget(0),
    PatternInputs({1, 2}),
    PatternOutputs({0}),
    Pattern({
      {HloOpcode::kAdd, NodeOperands({1, 2})},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
    })
  )
};
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());

  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(6, hlo_module->entry_computation()->instruction_count());

  // after-all is after the fused add
  auto* root = hlo_module->entry_computation()->root_instruction();
  EXPECT_EQ(root->opcode(), HloOpcode::kFusion);
  EXPECT_EQ(root->operand(0)->opcode(), HloOpcode::kAddDependency);
}

TEST_F(HloMatcherTest, MatchWithTokenDependencyOnExternalEdge) {
  // The 'add' in the fusion is dependent on the sine operator outside of
  // it
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[1] parameter(0)
  p1 = f32[1] parameter(1)
  p2 = f32[1] parameter(2)
  s0 = f32[1] sine(p2)
  aa = token[] after-all(s0)

  m1 = f32[1] multiply(p0, p1)
  ad = f32[1] add-dependency(p2, aa)
  m2 = f32[1] add(m1, ad)

  ROOT o = (f32[1]) tuple(m2, s0)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
std::vector<HloMatcherPattern> patterns = {
  HloMatcherPattern(
    PatternType("muladd"),
    PatternMetaTarget(0),
    PatternInputs({2, 3, 4}),
    PatternOutputs({0}),
    Pattern({
      {HloOpcode::kAdd, NodeOperands({1, 2})},
      {HloOpcode::kMultiply, NodeOperands({3, 4})},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
    })
  )
};
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());

  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(8, hlo_module->entry_computation()->instruction_count());

  // after-all is after the fused add
  auto* root = hlo_module->entry_computation()->root_instruction();
  auto* fusion = root->operand(0);
  EXPECT_EQ(fusion->opcode(), HloOpcode::kFusion);
  EXPECT_EQ(fusion->operand(0)->opcode(), HloOpcode::kAddDependency);
}

TEST_F(HloMatcherTest, MatchWithTokenDependencyOnInternalEdge) {
  // The 'add' in the fusion is dependent on the sine operator outside of
  // it
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[1] parameter(0)
  p1 = f32[1] parameter(1)
  p2 = f32[1] parameter(2)
  s0 = f32[1] sine(p2)
  aa = token[] after-all(s0)

  m1 = f32[1] multiply(p0, p1)
  ad = f32[1] add-dependency(m1, aa)
  m2 = f32[1] add(ad, p2)

  ROOT o = (f32[1]) tuple(m2, s0)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
std::vector<HloMatcherPattern> patterns = {
  HloMatcherPattern(
    PatternType("muladd"),
    PatternMetaTarget(0),
    PatternInputs({2, 3, 4}),
    PatternOutputs({0}),
    Pattern({
      {HloOpcode::kAdd, NodeOperands({1, 2})},
      {HloOpcode::kMultiply, NodeOperands({3, 4})},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
    })
  )
};
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());

  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(8, hlo_module->entry_computation()->instruction_count());
}

TEST_F(HloMatcherTest, MatchWithTokenIsDependencyFromInternalEdge) {
  // The multiply part of the fusion is a dependency of another inst
  std::string hlo = R"(
HloModule top

ENTRY c1 {
  p0 = f32[1] parameter(0)
  p1 = f32[1] parameter(1)
  p2 = f32[1] parameter(2)

  m1 = f32[1] multiply(p0, p1)
  m2 = f32[1] add(m1, p2)
  aa = token[] after-all(m1)

  ad = f32[1] add-dependency(p1, aa)
  s0 = f32[1] sine(ad)
  ROOT o = (f32[1]) tuple(m2, s0)
 }
)";

  auto config = GetModuleConfigForTest();
  auto module = ParseAndReturnVerifiedModule(hlo, config);
  EXPECT_TRUE(module.ok());
  auto* hlo_module = module.ValueOrDie().get();

  // clang-format off
std::vector<HloMatcherPattern> patterns = {
  HloMatcherPattern(
    PatternType("muladd"),
    PatternMetaTarget(0),
    PatternInputs({2, 3, 4}),
    PatternOutputs({0}),
    Pattern({
      {HloOpcode::kAdd, NodeOperands({1, 2})},
      {HloOpcode::kMultiply, NodeOperands({3, 4})},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})},
      {HloMatcherOpcode::kAnyOpcode, NodeOperands({})}
    })
  )
};
  // clang-format on

  CompilerAnnotations annotations(hlo_module);
  TestMatcher matcher(patterns, annotations, false);

  EXPECT_TRUE(matcher.Run(hlo_module).ValueOrDie());
  ASSERT_EQ(1, matcher.replace_count);
  EXPECT_EQ(8, hlo_module->entry_computation()->instruction_count());
}

}  // namespace
}  // namespace poplarplugin
}  // namespace xla
