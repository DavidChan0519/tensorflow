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
#include "tensorflow/compiler/plugin/poplar/driver/compiler_annotations.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/add_block_recompute.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/apply_recompute_suggestion.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/custom_op_replacer.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/matmul_combiner.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/poplar_algebraic_simplifier.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/remove_blocked_recompute_suggestions.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/remove_recompute_suggestions.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/scatter_simplifier.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/suggest_recompute.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_platform.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/data_initializer.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/matcher_predicates.h"

#include "tensorflow/compiler/xla/service/hlo_casting_utils.h"
#include "tensorflow/compiler/xla/service/hlo_cse.h"
#include "tensorflow/compiler/xla/service/hlo_parser.h"
#include "tensorflow/compiler/xla/service/hlo_pass_fix.h"
#include "tensorflow/compiler/xla/service/hlo_pass_pipeline.h"
#include "tensorflow/compiler/xla/service/pattern_matcher.h"
#include "tensorflow/compiler/xla/test.h"
#include "tensorflow/compiler/xla/tests/hlo_test_base.h"
#include "tensorflow/core/lib/core/status_test_util.h"

namespace xla {
namespace m = match;
namespace poplarplugin {
namespace {

using RecomputeSuggestionTest = HloTestBase;

/**
 * Check that the module is unchanged when nothing needs to be recomputed.
 *
 * This is important so that we are sure we don't break other graphs.
 */
TEST_F(RecomputeSuggestionTest, BlockRemoveNoOp) {
  std::string hlo_string = R"(
HloModule main

ENTRY main {
  a = f32[] parameter(0)
  b = f32[] parameter(1)
  c = f32[] parameter(2)
  d = f32[] add(a, b)
  ROOT e = f32[] add(d, c)
}
  )";

  HloModuleConfig config;
  config.set_debug_options(GetDebugOptionsForTest());

  auto module = ParseAndReturnVerifiedModule(hlo_string, config).ValueOrDie();

  HloPassPipeline pipeline("test");
  pipeline.AddPass<CustomOpReplacer>();
  pipeline.AddPass<SuggestRecompute>();
  pipeline.AddPass<AddBlockRecompute>();
  {
    auto& pass = pipeline.AddPass<HloPassFix<HloPassPipeline>>(
        "resolve-recompute-suggestion");

    pass.AddPass<HloPassFix<RemoveBlockedRecomputeSuggestions>>();
    pass.AddPass<ApplyRecomputeSuggestion>();
  }
  pipeline.AddPass<HloPassFix<RemoveBlockedRecomputeSuggestions>>();
  pipeline.AddPass<HloPassFix<RemoveRecomputeSuggestions>>();

  EXPECT_TRUE(pipeline.Run(module.get()).ValueOrDie());
  EXPECT_EQ(module->entry_computation()->instruction_count(), 5);

  auto root = module->entry_computation()->root_instruction();
  EXPECT_TRUE(Match(
      root, m::Add(m::Add(m::Parameter(), m::Parameter()), m::Parameter())));
}

/**
 * Check that a manual recomputation suggestion is applied.
 */
TEST_F(RecomputeSuggestionTest, CheckRecomputed) {
  std::string hlo_string = R"(
HloModule main

ENTRY main {
  a = f32[] parameter(0)
  b = f32[] parameter(1)
  c = f32[] parameter(2)
  d = f32[] add(f32[] a, f32[] b)
  e = f32[] add(f32[] d, f32[] c)
  f = f32[] custom-call(f32[] e), custom_call_target="SuggestRecompute", backend_config="{}"
  g = f32[] add(f32[] f, f32[] f)
  ROOT h = f32[] add(f32[] g, f32[] f)
}
  )";

  HloModuleConfig config;
  config.set_debug_options(GetDebugOptionsForTest());

  auto module = ParseAndReturnVerifiedModule(hlo_string, config).ValueOrDie();

  HloPassPipeline pipeline("test");
  pipeline.AddPass<CustomOpReplacer>();
  pipeline.AddPass<SuggestRecompute>();
  pipeline.AddPass<AddBlockRecompute>();
  {
    auto& pass = pipeline.AddPass<HloPassFix<HloPassPipeline>>(
        "resolve-recompute-suggestion");

    pass.AddPass<HloPassFix<RemoveBlockedRecomputeSuggestions>>();
    pass.AddPass<ApplyRecomputeSuggestion>();
  }
  pipeline.AddPass<HloPassFix<RemoveBlockedRecomputeSuggestions>>();
  pipeline.AddPass<HloPassFix<RemoveRecomputeSuggestions>>();

  EXPECT_TRUE(pipeline.Run(module.get()).ValueOrDie());
  EXPECT_EQ(module->entry_computation()->instruction_count(), 9);

  auto a = m::Parameter();
  auto b = m::Parameter();
  auto c = m::Parameter();
  auto d_clone = m::Add(a, b);
  auto e_clone = m::Add(d_clone, c);
  auto g = m::Add(e_clone, e_clone);
  auto d_clone_1 = m::Add(a, b);
  auto e_clone_1 = m::Add(d_clone_1, c);
  auto h = m::Add(g, e_clone_1);  // ROOT

  auto root = module->entry_computation()->root_instruction();
  EXPECT_TRUE(Match(root, h));
}

/**
 * Check that a convert of a parameter is automatically recomputed.
 */
TEST_F(RecomputeSuggestionTest, ConvertAutoRecompute) {
  std::string hlo_string = R"(
HloModule main

ENTRY main {
  a = f32[] parameter(0)
  b = f32[] parameter(1)
  c = f16[] parameter(2)
  c1 = f32[] convert(f16[] c)
  d = f32[] add(f32[] a, f32[] c1)
  e = f32[] add(f32[] b, f32[] c1)
  ROOT f = f32[] add(f32[] d, f32[] e)
}
  )";

  HloModuleConfig config;
  config.set_debug_options(GetDebugOptionsForTest());

  auto module = ParseAndReturnVerifiedModule(hlo_string, config).ValueOrDie();

  HloPassPipeline pipeline("test");
  pipeline.AddPass<CustomOpReplacer>();
  pipeline.AddPass<SuggestRecompute>();
  pipeline.AddPass<AddBlockRecompute>();
  {
    auto& pass = pipeline.AddPass<HloPassFix<HloPassPipeline>>(
        "resolve-recompute-suggestion");

    pass.AddPass<HloPassFix<RemoveBlockedRecomputeSuggestions>>();
    pass.AddPass<ApplyRecomputeSuggestion>();
  }
  pipeline.AddPass<HloPassFix<RemoveBlockedRecomputeSuggestions>>();
  pipeline.AddPass<HloPassFix<RemoveRecomputeSuggestions>>();

  EXPECT_TRUE(pipeline.Run(module.get()).ValueOrDie());
  EXPECT_EQ(module->entry_computation()->instruction_count(), 8);

  auto a = m::Parameter();
  auto b = m::Parameter();
  auto c = m::Parameter();
  auto c1_clone = m::Convert(c);
  auto d = m::Add(a, c1_clone);
  auto c1_clone_1 = m::Convert(c);
  auto e = m::Add(b, c1_clone_1);
  auto f = m::Add(d, e);  // ROOT

  auto root = module->entry_computation()->root_instruction();
  EXPECT_TRUE(Match(root, f));
}
}  // namespace
}  // namespace poplarplugin
}  // namespace xla
