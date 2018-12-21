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

#include "tensorflow/compiler/plugin/poplar/driver/hlo_matcher.h"
#include "tensorflow/compiler/plugin/poplar/driver/compiler_annotations.h"

#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"

namespace xla {
namespace poplarplugin {
namespace {

using HloMatcherTest = HloTestBase;

class TestMatcher : public HloMatcher {
 public:
  TestMatcher(const std::vector<HloMatcherPattern>& patterns,
              CompilerAnnotations& annotations, const char* name,
              const char index, bool root_only,
              unsigned int look_through_depth = 0)
      : HloMatcher(patterns, annotations, root_only, look_through_depth),
        match_index(index),
        match_name(name) {}

 private:
  unsigned ReplaceNodes() override {
    unsigned int replacement_count = 0;
    for (int pattern = 0; pattern < matches_.size(); pattern++) {
      for (HloMatcherMatched& match : matches_[pattern]) {
        if (match.ok) {
          replace_count++;
          match_pattern.push_back(pattern);
          match_count.push_back(match.instructions.size());
          const OutlinedInfo outlined_info =
              OutlineExpressionFromComputation(match, match_name, match_index);
          replacement_count += MarkReplacedInstructions(outlined_info);
        }
      }
    }
    return replacement_count;
  }

 public:
  int replace_count = 0;
  char match_index = 0;
  const char* match_name = nullptr;
  std::vector<unsigned int> match_pattern;
  std::vector<unsigned int> match_count;
};

TEST_F(HloMatcherTest, MatchTestSimpleReplacementTwice) {
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
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, add1, i3));

  builder.AddInstruction(HloInstruction::CreateTuple({add2}));

  auto computation = builder.Build();

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kAdd, true, 0, nullptr, {1, 2}},
       {HloOpcode::kParameter, false, 0, nullptr, {}},
       {HloOpcode::kParameter, false, 1, nullptr, {}}}};
  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "test", 0, false);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(2, matcher.replace_count);
  EXPECT_EQ(6, hlo_module->entry_computation()->instruction_count());
}

TEST_F(HloMatcherTest, MatchTestExplicitInputs) {
  Shape shape = ShapeUtil::MakeShape(F32, {10, 10});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape, "in2"));
  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, i1));
  auto add2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape, HloOpcode::kAdd, i1, i2));

  builder.AddInstruction(HloInstruction::CreateTuple({add1, add2}));

  auto computation = builder.Build();

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kAdd, true, 0, nullptr, {1, 2}},
       {HloOpcode::kParameter, false, 0, nullptr, {}},
       {HloOpcode::kParameter, false, 1, nullptr, {}}}};
  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "test", 0, false);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(1, matcher.replace_count);
  EXPECT_EQ(5, hlo_module->entry_computation()->instruction_count());
}

TEST_F(HloMatcherTest, MatchTestTwoPatterns) {
  Shape shape1 = ShapeUtil::MakeShape(F32, {10, 10});
  Shape shape2 = ShapeUtil::MakeShape(F32, {10});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape1, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape1, "in2"));
  auto i3 =
      builder.AddInstruction(HloInstruction::CreateParameter(2, shape2, "in3"));
  auto b1 =
      builder.AddInstruction(HloInstruction::CreateBroadcast(shape1, i3, {1}));
  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kAdd, i1, i2));
  auto add2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kAdd, add1, b1));

  builder.AddInstruction(HloInstruction::CreateTuple({add2}));

  OpMetadata add1_md;
  add1_md.set_op_type("Add");
  add1_md.set_op_name("long/add1");
  add1->set_metadata(add1_md);

  OpMetadata add2_md;
  add2_md.set_op_type("Add");
  add2_md.set_op_name("long/add2");
  add2->set_metadata(add2_md);

  auto computation = builder.Build();

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kAdd, true, 0, nullptr, {2, 1}},
       {HloOpcode::kBroadcast, true, 0, nullptr, {3}},
       {HloOpcode::kParameter, false, 1, nullptr, {}},
       {HloOpcode::kParameter, false, 0, nullptr, {}}},

      {{HloOpcode::kAdd, true, 0, nullptr, {1, 2}},
       {HloOpcode::kParameter, false, 0, nullptr, {}},
       {HloOpcode::kParameter, false, 1, nullptr, {}}}};
  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "test2", 0, false);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(2, matcher.replace_count);
  EXPECT_EQ(6, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* call_inst = comp->root_instruction()->operand(0);
  EXPECT_EQ("test2", call_inst->to_apply()->name());

  EXPECT_EQ("long/add2", call_inst->metadata().op_name());
  EXPECT_EQ("long/add1", call_inst->operand(1)->metadata().op_name());
}

TEST_F(HloMatcherTest, MatchTestGraphWithPathsJoining) {
  Shape shape1 = ShapeUtil::MakeShape(F32, {10, 10});
  Shape shape2 = ShapeUtil::MakeShape(F32, {10});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape1, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape1, "in2"));
  auto i3 =
      builder.AddInstruction(HloInstruction::CreateParameter(2, shape2, "in3"));
  auto b1 =
      builder.AddInstruction(HloInstruction::CreateBroadcast(shape1, i3, {1}));
  auto sub1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kSubtract, i1, b1));
  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kAdd, i2, b1));

  auto sub2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kSubtract, add1, sub1));

  builder.AddInstruction(HloInstruction::CreateTuple({sub2}));

  OpMetadata md;
  md.set_op_type("Broadcast");
  md.set_op_name("long/bc");
  b1->set_metadata(md);

  b1->set_sharding(HloSharding::AssignDevice(1));

  auto computation = builder.Build();

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kAdd, true, 0, nullptr, {2, 1}},
       {HloOpcode::kBroadcast, true, 0, nullptr, {3}},
       {HloOpcode::kParameter, false, 0, nullptr, {}},
       {HloOpcode::kParameter, false, 1, nullptr, {}}}};
  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "fuse", 1, false);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(1, matcher.replace_count);
  EXPECT_EQ(8, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* call_inst = comp->root_instruction()->operand(0)->operand(0);
  EXPECT_EQ("fuse", call_inst->to_apply()->name());

  EXPECT_EQ("long/bc", call_inst->metadata().op_name());
  EXPECT_TRUE(call_inst->has_sharding());
  EXPECT_EQ(1, call_inst->sharding().UniqueDevice());
}

TEST_F(HloMatcherTest, MatchTestGraphWithPathsJoiningOnMultipleMatchNode) {
  Shape shape1 = ShapeUtil::MakeShape(F32, {10, 10});
  Shape shape2 = ShapeUtil::MakeShape(F32, {10});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape1, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape1, "in2"));
  auto i3 =
      builder.AddInstruction(HloInstruction::CreateParameter(2, shape2, "in3"));
  auto b1 =
      builder.AddInstruction(HloInstruction::CreateBroadcast(shape1, i3, {1}));
  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kAdd, i1, b1));
  auto add2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kAdd, i2, b1));

  auto sub1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kSubtract, add1, add2));

  builder.AddInstruction(HloInstruction::CreateTuple({sub1}));

  auto computation = builder.Build();

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {{
      {HloOpcode::kAdd, true, 0, nullptr, {2, 1}},
      {HloOpcode::kBroadcast, true, 0, nullptr, {3}},
      {HloOpcode::kParameter, false, 0, nullptr, {}},
      {HloOpcode::kParameter, false, 1, nullptr, {}},
  }};
  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "test", 0, false);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(2, matcher.replace_count);
  EXPECT_EQ(7, hlo_module->entry_computation()->instruction_count());
}

TEST_F(HloMatcherTest, MatchTestGraphWithMatchedByNonRemovedNodes) {
  Shape shape1 = ShapeUtil::MakeShape(F32, {10, 10});
  Shape shape2 = ShapeUtil::MakeShape(F32, {10});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape1, "in1"));
  auto i2 =
      builder.AddInstruction(HloInstruction::CreateParameter(1, shape1, "in2"));
  auto i3 =
      builder.AddInstruction(HloInstruction::CreateParameter(2, shape2, "in3"));
  auto b1 =
      builder.AddInstruction(HloInstruction::CreateBroadcast(shape1, i3, {1}));
  auto sub1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kSubtract, i1, b1));
  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kAdd, i2, b1));

  auto sub2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kSubtract, add1, sub1));

  builder.AddInstruction(HloInstruction::CreateTuple({sub2}));

  auto computation = builder.Build();

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kSubtract, true, 0, nullptr, {1, 3}},
       {HloOpcode::kAdd, true, 0, nullptr, {4, 2}},
       {HloOpcode::kBroadcast, false, 1, nullptr, {}},
       {HloOpcode::kParameter, false, 0, nullptr, {}},
       {HloOpcode::kParameter, false, 2, nullptr, {}}}};
  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "test", 0, false);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(1, matcher.replace_count);
  EXPECT_EQ(2, matcher.match_count[0]);
  EXPECT_EQ(7, hlo_module->entry_computation()->instruction_count());
}

TEST_F(HloMatcherTest, OutlineWithInstructionsNotRemoved) {
  Shape shape1 = ShapeUtil::MakeShape(F32, {10, 10});
  Shape shape2 = ShapeUtil::MakeShape(F32, {10});

  auto builder = HloComputation::Builder(TestName());
  auto i1 =
      builder.AddInstruction(HloInstruction::CreateParameter(0, shape1, "in1"));
  auto i2 = builder.AddInstruction(
      HloInstruction::CreateConstant(LiteralUtil::One(F32)));
  auto sub1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kSubtract, i1, i2));
  auto add1 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kAdd, i1, i2));
  auto sub2 = builder.AddInstruction(
      HloInstruction::CreateBinary(shape1, HloOpcode::kSubtract, add1, sub1));

  builder.AddInstruction(HloInstruction::CreateTuple({sub2}));

  auto computation = builder.Build();

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kSubtract, true, 0, nullptr, {2, 1}},
       {HloOpcode::kConstant, true, 0, nullptr, {}},
       {HloOpcode::kParameter, false, 0, nullptr, {}}}};
  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "abc", 0, false);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(1, matcher.replace_count);
  EXPECT_EQ(6, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* call_inst = comp->root_instruction()->operand(0)->operand(1);
  EXPECT_EQ("abc", call_inst->to_apply()->name());
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

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kAdd, true, 0, nullptr, {1, 2}},
       {HloOpcode::kSubtract, true, 0, nullptr, {3, 2}},
       {HloOpcode::kParameter, false, 1, nullptr, {}},
       {HloOpcode::kParameter, false, 0, nullptr, {}}}};

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "abc", 0, false,
                      look_through_depth);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(1, matcher.replace_count);
  EXPECT_EQ(5, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* root = comp->root_instruction();
  // Expect that root is add now
  EXPECT_EQ(root, add);

  // Expect that operand 1 of add has changed to a call
  EXPECT_EQ(add->operand(1)->opcode(), HloOpcode::kCall);
  auto* call_inst = comp->root_instruction()->operand(1);
  // Expect the name
  EXPECT_EQ("abc", call_inst->to_apply()->name());
  // Expect the parameters
  EXPECT_EQ(call_inst->operand(0), i1);
  EXPECT_EQ(call_inst->operand(1), c1);
  // Expect the call body
  auto* call_root = call_inst->to_apply()->root_instruction();
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

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kAdd, true, 0, nullptr, {1, 2}},
       {HloOpcode::kSubtract, false, 1, nullptr, {}},
       {HloOpcode::kParameter, false, 0, nullptr, {}}}};

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "abc", 0, false,
                      look_through_depth);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(1, matcher.replace_count);
  EXPECT_EQ(6, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* root = comp->root_instruction();
  // Expect that root is add now
  EXPECT_EQ(root, add);

  // Expect that operand 1 of add has changed to a call
  EXPECT_EQ(add->operand(1)->opcode(), HloOpcode::kCall);
  auto* call_inst = comp->root_instruction()->operand(1);
  // Expect the name
  EXPECT_EQ("abc", call_inst->to_apply()->name());
  // Expect the parameters
  EXPECT_EQ(call_inst->operand(0), c1);
  EXPECT_EQ(call_inst->operand(1), sub);
  // Expect the call body
  auto* call_root = call_inst->to_apply()->root_instruction();
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

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kMultiply, true, 0, nullptr, {1, 2}},
       {HloOpcode::kSubtract, true, 0, nullptr, {3, 2}},
       {HloOpcode::kParameter, false, 1, nullptr, {}},
       {HloOpcode::kParameter, false, 0, nullptr, {}}}};

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "abc", 0, false,
                      look_through_depth);

  EXPECT_TRUE(matcher.Run(hlo_module.get()).ValueOrDie());
  EXPECT_EQ(1, matcher.replace_count);
  EXPECT_EQ(10, hlo_module->entry_computation()->instruction_count());

  auto* comp = hlo_module->entry_computation();
  auto* root = comp->root_instruction();
  // Expect that root is mul1 now
  EXPECT_EQ(root, mul1);

  // Expect that operand 1 of mul1 has changed to a call
  EXPECT_EQ(mul1->operand(1)->opcode(), HloOpcode::kCall);
  auto* call_inst = comp->root_instruction()->operand(1);
  // Expect the name
  EXPECT_EQ("abc", call_inst->to_apply()->name());
  // Expect the parameters
  EXPECT_EQ(call_inst->operand(0), i1);
  EXPECT_EQ(call_inst->operand(1), c1);
  // Expect the call body
  auto* call_root = call_inst->to_apply()->root_instruction();
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

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kMultiply, true, 0, nullptr, {1, 2}},
       {HloOpcode::kSubtract, false, 1, nullptr, {}},
       {HloOpcode::kParameter, false, 0, nullptr, {}}}};

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "abc", 0, false,
                      look_through_depth);

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

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kMultiply, true, 0, nullptr, {1, 2}},
       {HloOpcode::kSubtract, false, 1, nullptr, {}},
       {HloOpcode::kParameter, false, 0, nullptr, {}}}};

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "abc", 0, false,
                      look_through_depth);

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

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kAdd, true, 0, nullptr, {1, 2}},
       {HloOpcode::kSubtract, false, 1, nullptr, {}},
       {HloOpcode::kParameter, false, 0, nullptr, {}}}};

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "abc", 0, false,
                      look_through_depth);

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

  auto hlo_module = CreateNewModule();
  hlo_module->AddEntryComputation(std::move(computation));

  std::vector<HloMatcherPattern> patterns = {
      {{HloOpcode::kBroadcast, true, 0, nullptr, {1}},
       {HloOpcode::kConstant, true, 0, nullptr, {}}}};

  CompilerAnnotations annotations(hlo_module.get());
  TestMatcher matcher(patterns, annotations, "abc", 0, false,
                      look_through_depth);

  EXPECT_FALSE(matcher.Run(hlo_module.get()).ValueOrDie());
}

}  // namespace
}  // namespace poplarplugin
}  // namespace xla
