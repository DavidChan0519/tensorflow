/* Copyright 2017 Graphcore Ltd
 */

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

#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>

#include "absl/strings/str_cat.h"

#include "tensorflow/compiler/plugin/poplar/driver/poplar_compiler.h"

#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/ops/ops.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/allocation_finder.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/casts_elimination.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/combine_instructions.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/commutative_instruction_reorder_operands.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/computation_flattener.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/constant_nan.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/constant_slice_folding.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/custom_op_replacer.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/dependency_replacer.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/elementwise_broadcast_converter.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/expression_outliner.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/f16_constant_folding.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/forward_allocation.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/fuse_ops_early.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/fuse_ops_late.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/fuse_wide_const.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/gradient_accumulation_fuser.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/hlo_computation_name_uniquify.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/inplace_finder.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/inter_ipu_copy_inserter.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/not_supported_gather_expander.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/not_supported_scatter_expander.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/parse_poplar_backend_config.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/recompute_instructions.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/replication_factor_to_constant.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/root_token_replacer.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/scatter_combiner.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/sharding_pass.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/while_loop_condition_simplify.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/while_loop_to_repeat_simplify.h"
#include "tensorflow/compiler/plugin/poplar/driver/passes/wide_const_finder.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_executable.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_executor.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_platform_id.h"
#include "tensorflow/compiler/plugin/poplar/driver/schedulers/ipu_scheduler.h"
#include "tensorflow/compiler/plugin/poplar/driver/schedulers/liveness_look_ahead_scheduler.h"
#include "tensorflow/compiler/plugin/poplar/driver/schedulers/look_ahead_scheduler.h"
#include "tensorflow/compiler/plugin/poplar/driver/schedulers/sync_list_scheduler.h"
#include "tensorflow/compiler/plugin/poplar/driver/tensor.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/convolution_preplanning.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/data_initializer.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/flags.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/plugin/poplar/driver/visitors/entry_visitor.h"

#include "tensorflow/compiler/xla/service/algebraic_simplifier.h"
#include "tensorflow/compiler/xla/service/cholesky_expander.h"
#include "tensorflow/compiler/xla/service/computation_placer.h"
#include "tensorflow/compiler/xla/service/dynamic_index_splitter.h"
#include "tensorflow/compiler/xla/service/hlo_constant_folding.h"
#include "tensorflow/compiler/xla/service/hlo_cse.h"
#include "tensorflow/compiler/xla/service/hlo_dce.h"
#include "tensorflow/compiler/xla/service/hlo_get_dimension_size_rewriter.h"
#include "tensorflow/compiler/xla/service/hlo_graph_dumper.h"
#include "tensorflow/compiler/xla/service/hlo_pass_fix.h"
#include "tensorflow/compiler/xla/service/hlo_pass_pipeline.h"
#include "tensorflow/compiler/xla/service/hlo_subcomputation_unification.h"
#include "tensorflow/compiler/xla/service/map_inliner.h"
#include "tensorflow/compiler/xla/service/reshape_mover.h"
#include "tensorflow/compiler/xla/service/sort_simplifier.h"
#include "tensorflow/compiler/xla/service/triangular_solve_expander.h"
#include "tensorflow/compiler/xla/service/tuple_simplifier.h"
#include "tensorflow/compiler/xla/service/while_loop_constant_sinking.h"
#include "tensorflow/compiler/xla/service/zero_sized_hlo_elimination.h"
#include "tensorflow/compiler/xla/shape_util.h"
#include "tensorflow/compiler/xla/status_macros.h"

#include "tensorflow/stream_executor/lib/initialize.h"

#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/random/random.h"

#include <poplar/exceptions.hpp>
#include <poputil/exceptions.hpp>

#include <poplin/codelets.hpp>
#include <popnn/codelets.hpp>
#include <popops/codelets.hpp>
#include <poprand/codelets.hpp>
#include <popsys/codelets.hpp>

#include <poplar/replication_factor.hpp>
#include <poprand/RandomGen.hpp>
#include <popsys/CSRFunctions.hpp>

namespace se = ::stream_executor;

using ::absl::StrCat;

namespace xla {
namespace poplarplugin {
namespace {
std::once_flag help_flag_printed;

int64 SizeFunction(const BufferValue& buffer) {
  return ShapeUtil::ByteSizeOf(buffer.shape(), 1);
};

std::string GetPathToGraphProgFile(std::string filename) {
  Dl_info dlInfo;
  static const void* dummy;
  if (dladdr(&dummy, &dlInfo)) {
    std::string path(dlInfo.dli_fname);
    path = path.substr(0, path.find_last_of('/') + 1);
    path = path + "../compiler/plugin/poplar/" + filename;
    if (access(path.c_str(), R_OK) != -1) {
      return path;
    }
  }

  // This is for unit tests
  {
    char buf[256];
    getcwd(buf, 255);
    std::string path(buf);
    path = path + "/tensorflow/compiler/plugin/poplar/" + filename;
    if (access(path.c_str(), R_OK) != -1) {
      return path;
    }
  }

  return "";
}
bool GetConstantSubOutput(const HloInstruction* root, const Shape& layout,
                          std::vector<Literal>& sub_result) {
  if (root->opcode() == HloOpcode::kConstant) {
    auto literal = root->literal().Relayout(layout);
    sub_result.emplace_back(std::move(literal));
    return true;
  } else if (root->opcode() == HloOpcode::kTuple) {
    for (unsigned int i = 0; i < root->operand_count(); i++) {
      auto& sub_shape = layout.tuple_shapes(i);
      if (!GetConstantSubOutput(root->operand(i), sub_shape, sub_result)) {
        return false;
      }
    }
    return true;
  }
  return false;
}

// This function returns true if all the root outputs are constants and all the
// constants are stored in result in a flat tuple order for each output
bool GetConstantOutput(const HloInstruction* root, const Shape& layout,
                       std::vector<std::vector<Literal>>& result) {
  if (root->opcode() == HloOpcode::kConstant) {
    auto literal = root->literal().Relayout(layout);
    std::vector<Literal> sub_result;
    sub_result.emplace_back(std::move(literal));
    result.emplace_back(std::move(sub_result));
    return true;
  } else if (root->opcode() == HloOpcode::kTuple) {
    for (unsigned int i = 0; i < root->operand_count(); i++) {
      auto& sub_shape = layout.tuple_shapes(i);
      std::vector<Literal> sub_result;
      if (!GetConstantSubOutput(root->operand(i), sub_shape, sub_result)) {
        return false;
      }
      result.emplace_back(std::move(sub_result));
    }
    return true;
  }
  return false;
}

bool AnyComputationHasSideEffects(const HloModule* module) {
  for (const auto& comp : module->computations()) {
    if (IsPopOpsFusion(comp)) {
      continue;
    }

    if (comp->HasSideEffect()) {
      return true;
    }
  }
  return false;
}

bool ShardingEnabled(const HloModule* module) {
  for (const auto* comp : module->MakeNonfusionComputations()) {
    for (const auto* inst : comp->instructions()) {
      if (inst->has_sharding()) {
        auto sharding = inst->sharding();
        if (IsSupportedSharding(sharding)) {
          return true;
        }
      }
    }
  }
  return false;
}

int64 MaximalShard(const HloModule* module) {
  int64 maximal_shard = 0;
  for (const auto* comp : module->MakeNonfusionComputations()) {
    for (const auto* inst : comp->instructions()) {
      if (inst->has_sharding()) {
        auto sharding = inst->sharding();
        if (IsSupportedSharding(sharding)) {
          const auto& vec = GetShardingDeviceIdVector(sharding);
          for (auto s : vec) {
            maximal_shard = std::max(maximal_shard, s);
          }
        }
      }
    }
  }
  return maximal_shard;
}

int64 NumIPUsInShards(const HloModule* module) {
  int64 num_explicit_shards = MaximalShard(module) + 1;
  // Round it up to the next highest power of 2.
  if (num_explicit_shards <= 1LL) {
    return 1LL;
  }
  int64 rounded = 2;
  num_explicit_shards--;
  while (num_explicit_shards >>= 1LL) {
    rounded <<= 1LL;
  }
  return rounded;
}

bool AreAllOutputsParameters(const HloModule* module,
                             std::vector<uint64>& output_paramater_numbers) {
  const HloComputation* entry = module->entry_computation();
  const HloInstruction* root = entry->root_instruction();

  // Get all the outputs
  std::vector<const HloInstruction*> outputs;
  if (root->opcode() == HloOpcode::kTuple) {
    outputs = {root->operands().begin(), root->operands().end()};
  } else if (root->opcode() == HloOpcode::kParameter) {
    outputs = {root};
  } else {
    return false;
  }

  // Check if all the outputs are parameters so that we can simply remap input
  // instead of executing the engine
  for (auto op : outputs) {
    if (op->opcode() != HloOpcode::kParameter) {
      return false;
    } else {
      output_paramater_numbers.push_back(op->parameter_number());
    }
  }

  // Check that all the parameters are in a standard layout format.
  const ComputationLayout layout = module->entry_computation_layout();
  for (auto param_number : output_paramater_numbers) {
    if (param_number < layout.parameter_count()) {
      auto parameter_shape = layout.parameter_layout(param_number).shape();
      const bool parameter_has_standard_layout = absl::c_all_of(
          FlattenedXlaShape(parameter_shape), [](const Shape& shape) {
            return LayoutUtil::IsMonotonicWithDim0Major(shape.layout());
          });
      if (!parameter_has_standard_layout) {
        return false;
      }
    }
  }

  // Check that the computation output shape is the same as the root
  return ShapeUtil::Equal(
      root->shape(),
      root->GetModule()->entry_computation_layout().result_shape());
}

StatusOr<std::string> SerializeComputationToGraphDef(
    const HloComputation& comp) {
  return RenderGraph(comp, comp.name(), {}, RenderedGraphFormat::kDot, nullptr,
                     true);
}

HloPrintOptions GetPrintOptions() {
  HloPrintOptions opts;
  opts.set_print_operand_shape(false)
      .set_print_percent(false)
      .set_include_layout_in_shapes(false);
  return opts;
}

StatusOr<poplar::program::Program> InitializeSeed(poplar::Graph& graph,
                                                  int replication_factor) {
  const std::string seed_prefix = "__seed";

  auto seed =
      graph.addVariable(poplar::UNSIGNED_INT, {2}, seed_prefix + "/tensor");
  graph.setTileMapping(seed, 0);

  poplar::program::Sequence seq;
  if (!UseSyntheticData()) {
    // Copy the seed from the data stream and set it.
    auto data_stream = graph.addHostToDeviceFIFO(
        GetRandomNumberSeedStream(), seed.elementType(), seed.numElements());
    seq.add(poplar::program::Copy(data_stream, seed));
  } else if (UseSyntheticData() && UseSyntheticDataInitializer()) {
    // Initialize the seed on the device.
    auto& initializer = DataInitializer::GetSyntheticDataInitializer();
    TF_ASSIGN_OR_RETURN(auto literal,
                        initializer.GetData(ShapeUtil::MakeShape(U32, {2})));
    TF_RETURN_IF_ERROR(SetInitialTensorValue(graph, seed, literal));
  }
  poprand::setSeed(graph, seed, 0, seq, seed_prefix + "/set");

  return seq;
}

void setFpBehaviour(poplar::Graph& graph,
                    const IpuOptions::FloatingPointBehaviour& fp_control,
                    poplar::program::Sequence& seq) {
  if (graph.getTarget().getTargetType() == poplar::TargetType::IPU) {
    popsys::FloatingPointBehaviour fp_behaviour(
        fp_control.inv(), fp_control.div0(), fp_control.oflo(),
        fp_control.esr(), fp_control.nanoo());
    popsys::setFloatingPointBehaviour(graph, seq, fp_behaviour,
                                      "setFpBehaviour");
  } else {
    LOG(WARNING) << "Setting IPU floating point behaviour is not supported "
                    "on IPU__MODEL";
  }
}

void PrintHelpString() { LOG(INFO) << PoplarXlaFlags::GetFlagUsageString(); }

void CreatePoplarGraphs(CompilerResources& resources, const HloModule* module,
                        const poplar::Device& dev) {
  resources.main_graph = absl::make_unique<poplar::Graph>(
      dev, 0, poplar::replication_factor(resources.replication_factor));
  auto& main_graph = GetMasterGraph(resources);
  if (ShardingEnabled(module)) {
    auto num_ipus = main_graph.getTarget().getNumIPUs();
    // Check that we have enough IPUs for this sharding configuration.
    auto tiles_per_ipu = main_graph.getTarget().getTilesPerIPU();
    for (unsigned ipu = 0; ipu < num_ipus; ++ipu) {
      resources.shard_graphs.emplace_back(main_graph.createVirtualGraph(
          ipu * tiles_per_ipu, (ipu + 1) * tiles_per_ipu));
    }
    VLOG(1) << "Created " << num_ipus << " IPU shards";
  }
  main_graph.addCodelets(GetPathToGraphProgFile("tf.gp"));
  poplin::addCodelets(main_graph);
  popnn::addCodelets(main_graph);
  popops::addCodelets(main_graph);
  poprand::addCodelets(main_graph);
  popsys::addCodelets(main_graph);
}
}  // namespace

StatusOr<std::unique_ptr<HloModule>> PoplarCompiler::RunHloPasses(
    std::unique_ptr<HloModule> module,
    perftools::gputools::StreamExecutor* executor,
    se::DeviceMemoryAllocator* device_allocator) {
  return std::move(module);
}

StatusOr<std::unique_ptr<Executable>> PoplarCompiler::RunBackend(
    std::unique_ptr<HloModule> module,
    perftools::gputools::StreamExecutor* stream_exec,
    se::DeviceMemoryAllocator* device_allocator) {
  if (stream_exec == nullptr) {
    return tensorflow::errors::Unknown(
        "NULL stream pointer in poplar compiler");
  }

  if (PoplarXlaFlags::Get().help) {
    std::call_once(help_flag_printed, &PrintHelpString);
  }

  VLOG(1) << "Begin compilation: " << module->name() << " for ordinal  "
          << stream_exec->device_ordinal();

  PoplarExecutor* poplarExecutor(
      static_cast<PoplarExecutor*>(stream_exec->implementation()));

  std::unique_ptr<HloProfileIndexMap> profile_index_map;
  std::unique_ptr<HloProfilePrinterData> profile_printer;
  if (module->config().hlo_profiling_enabled()) {
    const auto& name = module->entry_computation()->name();
    HloCostAnalysis cost_analysis(ShapeSizeBytesFunction());
    profile_index_map = absl::make_unique<HloProfileIndexMap>(*module);
    profile_printer =
        CreateHloProfilePrinterData(*profile_index_map, cost_analysis, name);
  }

  std::string cache_filename;
  if (poplarExecutor->HaveExecutableCache()) {
    cache_filename = poplarExecutor->CachedExecutableFilename(*module);

    if (poplarExecutor->HaveCachedExecutable(cache_filename)) {
      TF_ASSIGN_OR_RETURN(PoplarExecutable * poplar_executable,
                          PoplarExecutable::Deserialize(
                              std::move(module), std::move(profile_printer),
                              std::move(profile_index_map), cache_filename));
      // When restoring the executable we still need to make sure all the
      // outfeeds are unique.
      TF_RETURN_IF_ERROR(poplarExecutor->RegisterOutfeeds(
          poplar_executable->GetOutfeedInfos()));

      std::unique_ptr<Executable> executable;
      executable.reset(poplar_executable);

      VLOG(1) << "Loaded " << executable->module().name() << " from "
              << cache_filename;

      return std::move(executable);
    } else {
      VLOG(1) << "Couldn't find " << cache_filename << " in executable cache";
    }
  }

  if (!poplarExecutor->HasPoplarDevice()) {
    return xla::FailedPrecondition(
        "No device has been configured. Did you configure the IPU devices by "
        "running `tensorflow.python.ipu.configure_ipu_system(ipu_options)`?");
  }
  const poplar::Device& poplar_device = poplarExecutor->GetPoplarDevice();

  std::lock_guard<std::mutex> g(static_mu_);

  uint64 start_micros = tensorflow::Env::Default()->NowMicros();

  // Work out the IPU division for this IPU.
  // Given device with `num_ipus` IPU chips, we get the number of shards
  // `num_shards` and the replication factor is `num_ipus`/`num_shards` (and
  // we also make sure `num_ipus` % `num_shards` == 0).
  const auto num_ipus = poplar_device.getTarget().getNumIPUs();
  const auto num_shards = NumIPUsInShards(module.get());
  const auto replication_factor = num_ipus / num_shards;

  // Check that it's divisible.
  if (num_ipus % num_shards) {
    return xla::ResourceExhaustedStrCat(
        "Trying to compile a graph for an IPU device with ", num_ipus,
        " IPUs and ", num_shards,
        " shards. The number of shards needs to "
        " divide the number of IPUs.");
  }

  CompilerResources resources(
      poplarExecutor->GetConvolutionOptions(),
      poplarExecutor->GetPoolingOptions(),
      poplarExecutor->DisableGraphConvCaching(),
      poplarExecutor->MergeInfeedCopies(), replication_factor,
      poplarExecutor->GetMaxAllReduceBufferSize(),
      poplarExecutor->GetMaxInterIpuCopyBufferSize(),
      poplarExecutor->GetMaxSchedulerLookaheadDepth(),
      poplarExecutor->GetMaxSchedulerSearchSpaceSize(), module.get(),
      poplarExecutor->FloatingPointBehaviour(),
      poplarExecutor->ClearMatMulPass());

  if (replication_factor > 1) {
    VLOG(1) << "Created " << replication_factor << " replica IPU graph.";
  }

  {
    AlgebraicSimplifierOptions simplifier_opts(
        [](const Shape&, const Shape&) { return false; });
    simplifier_opts.set_is_layout_sensitive(false);
    simplifier_opts.set_enable_conv_simplification(false);
    simplifier_opts.set_enable_dot_strength_reduction(false);
    simplifier_opts.set_enable_window_reduce_to_reduce_replacement(false);
    simplifier_opts.set_enable_dot_to_multiply_rewrite(false);

    HloPassPipeline pipeline("IPU");
    if (!poplarExecutor->RetainControlDependencies()) {
      pipeline.AddPass<DependencyReplacer>(false);
    }
    pipeline.AddPass<HloGetDimensionSizeRewriter>();
    pipeline.AddPass<CustomOpReplacer>();
    pipeline.AddPass<ParsePoplarBackendConfig>();
    pipeline.AddPass<ReplicationFactorToConstant>(resources.replication_factor);
    pipeline.AddPass<GradientAccumulationFuser>(resources.annotations);
    pipeline.AddPass<HloComputationNameUniquify>();
    pipeline.AddPass<CholeskyExpander>();
    pipeline.AddPass<TriangularSolveExpander>();
    pipeline.AddPass<NotSupportedGatherExpander>();
    pipeline.AddPass<NotSupportedScatterExpander>();
    pipeline.AddPass<DynamicIndexSplitter>();
    pipeline.AddPass<HloPassFix<ConstantSliceFolding>>();
    pipeline.AddPass<HloPassFix<FuseOpsEarly>>(resources.annotations);
    pipeline.AddPass<HloCSE>(false);
    pipeline.AddPass<HloPassFix<AlgebraicSimplifier>>(simplifier_opts);
    pipeline.AddPass<SortSimplifier>();
    pipeline.AddPass<RootTokenReplacer>();
    pipeline.AddPass<ReshapeMover>();
    pipeline.AddPass<MapInliner>();
    pipeline.AddPass<HloPassFix<AlgebraicSimplifier>>(simplifier_opts);
    pipeline.AddPass<ZeroSizedHloElimination>();
    pipeline.AddPass<ComputationFlattener>();
    pipeline.AddPass<TupleSimplifier>(true);
    // pass.AddPass<ConditionalSimplifier>();
    pipeline.AddPass<F16ConstantFolding>();
    pipeline.AddPass<HloConstantFolding>();
    pipeline.AddPass<HloCSE>(true);
    pipeline.AddPass<WideConstFinder>();
    pipeline.AddPass<CommutativeInstructionReorderOperands>();
    {
      auto& pass =
          pipeline.AddPass<HloPassFix<HloPassPipeline>>("repeated-fusing");
      pass.AddPass<CastsElimination>(resources.annotations);
      pass.AddPass<HloCSE>(true);
      pass.AddPass<HloDCE>();
      pass.AddPass<WhileLoopConstantSinking>();
      pass.AddPass<HloPassFix<AlgebraicSimplifier>>(simplifier_opts);
      pass.AddPass<ReshapeMover>();
      pass.AddPass<SortSimplifier>();
      pass.AddPass<ScatterCombiner>(resources.annotations);
      pass.AddPass<HloDCE>();
      pass.AddPass<WhileLoopConditionSimplify>();
      pass.AddPass<HloPassFix<WhileLoopToRepeatSimplify>>();
    }
    pipeline.AddPass<HloPassFix<FuseOpsLate>>(resources.annotations);
    pipeline.AddPass<ElementwiseBroadcastConverter>();
    pipeline.AddPass<FuseWideConst>(resources.annotations);
    pipeline.AddPass<HloSubcomputationUnification>();
    pipeline.AddPass<RecomputeInstructions>(
        poplarExecutor->InstructionRecomputationEnabled());
    pipeline.AddPass<HloDCE>();
    pipeline.AddPass<DependencyReplacer>(true);
    pipeline.AddPass<HloSubcomputationUnification>();
    pipeline.AddPass<ShardingPass>();
    pipeline.AddPass<InterIpuCopyInserter>();
    pipeline.AddPass<InplaceFinder>();
    pipeline.AddPass<ExpressionOutliner>();
    pipeline.AddPass<HloDCE>();
    // Beyond this point non of the passes in the pipeline are allowed to modify
    // the instructions in the HloModule.

    // TODO(T10195) re-enable.
    // if (!PoplarXlaFlags::Get().allow_nans) {
    //   pipeline.AddPass<ConstantNaN>();
    // }

    pipeline.AddPass<ConvolutionClassifier>();
    pipeline.AddPass<AllocationFinder>(resources.annotations);
    pipeline.AddPass<HloPassFix<ForwardAllocation>>(resources.annotations);
    if (resources.information.max_all_reduce_buffer_size > 0 ||
        resources.information.max_inter_ipu_copies_buffer_size > 0) {
      pipeline.AddPass<IpuScheduler>(
          SizeFunction, CreateLookAheadMemoryScheduler(resources.information));
      pipeline.AddPass<CombineInstructions>();
      pipeline.AddPass<HloDescheduler>();
    }

    TF_ASSIGN_OR_RETURN(
        auto scheduler,
        BestIpuSchedule(
            {CreateLookAheadMemoryScheduler(resources.information),
             MemorySchedulerAlgorithmToIPU(PostOrderMemoryScheduler)}));

    pipeline.AddPass<IpuScheduler>(SizeFunction, scheduler);

    TF_RETURN_IF_ERROR(pipeline.Run(module.get()).status());
  }

  HloComputation* entry = module->entry_computation();

  if (poplarExecutor->IpuTraceEventsEnabled()) {
    poplarExecutor->AddCompileBeginEventRecord(module->name());
  }

  // Set layout if there isn't one
  auto comp_layout =
      module->mutable_entry_computation_layout()->mutable_result_layout();
  if (!comp_layout->LayoutIsSet()) {
    auto shape = entry->root_instruction()->shape();
    TF_CHECK_OK(comp_layout->CopyLayoutFromShape(shape));
  }

  VLOG(1) << "Compiling main computation " << entry->name();
  if (VLOG_IS_ON(1)) {
    XLA_VLOG_LINES(1, module->ToString(GetPrintOptions()));
  }

  if (VLOG_IS_ON(2)) {
    const auto& annotations = resources.annotations;
    XLA_VLOG_LINES(2, annotations.input_output_aliasing_map.ToString());
  }

  std::unique_ptr<poplar::Engine> engine;
  std::vector<poplar::program::Program> progs;
  EntryVisitor visitor(resources,
                       poplarExecutor->AlwaysRearrangeCopiesOnTheHost());

  std::vector<std::vector<Literal>> constant_output;
  const bool is_constant_output = GetConstantOutput(
      entry->root_instruction(), comp_layout->shape(), constant_output);

  const bool any_computation_has_side_effects =
      AnyComputationHasSideEffects(module.get());
  const auto is_constant_graph =
      is_constant_output && !any_computation_has_side_effects;

  std::string map_json;
  std::vector<uint64> remaped_output;

  const bool all_outputs_are_parameters =
      AreAllOutputsParameters(module.get(), remaped_output);

  bool is_remap_graph =
      all_outputs_are_parameters && !any_computation_has_side_effects;

  if (is_constant_graph) {
    VLOG(1) << "Skip engine compilation - output is constant.";
  } else if (is_remap_graph) {
    VLOG(1) << "Skip engine compilation - all outputs are inputs.";
  } else {
    // Only create the graphs if we are compiling.
    CreatePoplarGraphs(resources, module.get(), poplar_device);
    auto& main_graph = GetMasterGraph(resources);

    try {
      ConvolutionPreplanning convolution_preplanning;
      TF_RETURN_IF_ERROR(convolution_preplanning.Plan(module.get(), resources));
      auto order = module->schedule().sequence(entry).instructions();

      TF_RETURN_IF_ERROR(entry->AcceptOrdered(&visitor, order));
    } catch (const std::exception& e) {
      return PoplarExceptionToTensorflowStatus("[Build graph] ", e);
    }

    poplar::program::Sequence main_program;

    // Register the outfeeds which this executable creates.
    TF_RETURN_IF_ERROR(
        poplarExecutor->RegisterOutfeeds(resources.annotations.outfeed_infos));
    // Set up the random seed
    TF_ASSIGN_OR_RETURN(auto seed_setup,
                        InitializeSeed(main_graph, replication_factor));
    main_program.add(seed_setup);

    // Set up the floating point control register if required
    const auto& fp_control = poplarExecutor->FloatingPointBehaviour();
    if (fp_control.flags_set()) {
      setFpBehaviour(main_graph, fp_control, main_program);
    }

    // Add the main program sequence
    main_program.add(visitor.GetSequence());

    // =======================================================================
    // DO NOT CHANGE THE ORDER OF THESE WITHOUT UPDATING PoplarProgramType IN
    // exectutor.h
    // =======================================================================
    progs.push_back(visitor.GetHostToDevice());
    progs.push_back(main_program);
    progs.push_back(visitor.GetDeviceToHost());

    if (!PoplarXlaFlags::Get().save_vertex_graph.empty()) {
      auto filename =
          tensorflow::io::JoinPath(PoplarXlaFlags::Get().save_vertex_graph,
                                   module->name() + ".vertex_graph");
      VLOG(1) << "Dumping vertex graph " << filename;
      std::ofstream stream(filename);
      main_graph.outputVertexGraph(stream, progs);
    }

    try {
      VLOG(1) << "Compile engine " << module->name();

      map_json = GetTensorMappingJson(module->name(), main_graph,
                                      resources.tensor_maps);

      auto& opts = poplarExecutor->GetOptionsFlags();
      auto progress_logging = [](int progress, int total) {
        float progress_percent = std::floor(
            100.0f * static_cast<float>(progress) / static_cast<float>(total));
        VLOG(1) << "Poplar compilation " << progress_percent << "% complete";
      };

      poplar::Executable exec =
          poplar::compileGraph(main_graph, progs, opts, progress_logging);

      if (poplarExecutor->HaveExecutableCache()) {
        if (!poplarExecutor->HaveCachedExecutable(cache_filename)) {
          TF_RETURN_IF_ERROR(PoplarExecutable::Serialize(
              cache_filename, exec, resources.annotations.infeed_infos,
              resources.annotations.outfeed_infos, replication_factor,
              poplarExecutor->GetReportFlags()));
        }
      }

      engine.reset(new poplar::Engine(std::move(exec), opts));

    } catch (const std::exception& e) {
      if (poplarExecutor->CompilerReportingEnabled()) {
        DumpIfPoplarOutOfMemoryAllocationException(poplarExecutor);
      }
      return PoplarExceptionToTensorflowStatus("[Compile engine] ", e);
    }
  }

  if (poplarExecutor->IpuTraceEventsEnabled()) {
    std::stringstream report_stream;

    if (poplarExecutor->CompilerReportingEnabled() && engine != nullptr) {
      try {
        auto rep = engine->getGraphProfile();
        if (poplarExecutor->CompilerReportingTextFormat()) {
          auto opts = poplarExecutor->GetReportFlags();
          SetFlagIfNotPresent(opts, "showVarStorage", "true");
          poplar::printGraphSummary(report_stream, rep, opts);
        } else if (poplarExecutor->CompilerReportingCborFormat()) {
          poplar::serializeToCBOR(report_stream, rep);
        } else {
          poplar::serializeToJSON(report_stream, rep);
        }
      } catch (const std::exception& e) {
        return PoplarExceptionToTensorflowStatus("[Compiler report] ", e);
      }
    }

    uint64 duration = tensorflow::Env::Default()->NowMicros() - start_micros;

    if (report_stream.tellp() > poplarExecutor->MaxReportSize()) {
      LOG(WARNING) << "Dropping Poplar compilation report, size was "
                   << report_stream.tellp();
      report_stream.str(std::string());
    }

    poplarExecutor->AddCompileEndEventRecord(
        module->name(), report_stream.str(), map_json, duration);
  }

  std::unique_ptr<Executable> executable;
  PoplarExecutable* poplar_executable = new PoplarExecutable(
      std::move(module), std::move(profile_printer),
      std::move(profile_index_map), std::move(engine),
      std::move(resources.annotations.input_output_aliasing_map),
      is_constant_graph, std::move(constant_output), is_remap_graph,
      std::move(remaped_output), replication_factor,
      std::move(resources.annotations.infeed_infos),
      std::move(resources.annotations.outfeed_infos));

  executable.reset(poplar_executable);

  return std::move(executable);
}

Status PoplarCompiler::RunHloPassesOnModuleGroup(
    HloModuleGroup* module_group,
    absl::Span<se::StreamExecutor* const> executors,
    se::DeviceMemoryAllocator* device_allocator) {
  return xla::InvalidArgument("Module groups not supported on Poplar");
}

StatusOr<std::vector<std::unique_ptr<Executable>>>
PoplarCompiler::RunBackendOnModuleGroup(
    std::unique_ptr<HloModuleGroup> module_group,
    std::vector<std::vector<se::StreamExecutor*>> stream_exec,
    se::DeviceMemoryAllocator* device_allocator) {
  return xla::InvalidArgument("Module groups not supported on Poplar");
}

StatusOr<std::vector<std::unique_ptr<Executable>>> PoplarCompiler::Compile(
    std::unique_ptr<HloModuleGroup> module_group,
    std::vector<std::vector<se::StreamExecutor*>> stream_exec,
    se::DeviceMemoryAllocator* device_allocator) {
  if (module_group->empty()) {
    return std::vector<std::unique_ptr<Executable>>();
  }
  if (module_group->size() > 1) {
    return tensorflow::errors::Unimplemented(
        "Compilation of multiple HLO modules is not supported on Poplar.");
  }
  if (stream_exec.size() != 1 || stream_exec[0].size() != 1) {
    return tensorflow::errors::Unimplemented(
        "Unexpected number of StreamExecutor's.");
  }
  auto hlo_modules = module_group->ConsumeModules();
  TF_ASSIGN_OR_RETURN(auto module,
                      RunHloPasses(std::move(hlo_modules[0]), stream_exec[0][0],
                                   device_allocator));
  TF_ASSIGN_OR_RETURN(
      auto executable,
      RunBackend(std::move(module), stream_exec[0][0], device_allocator));
  std::vector<std::unique_ptr<Executable>> ret;
  ret.push_back(std::move(executable));
  return std::move(ret);
}

StatusOr<std::vector<std::unique_ptr<AotCompilationResult>>>
PoplarCompiler::CompileAheadOfTime(std::unique_ptr<HloModuleGroup>,
                                   const AotCompilationOptions&) {
  return xla::InvalidArgument("AOT compilation not supported on Poplar");
}

se::Platform::Id PoplarCompiler::PlatformId() const {
  return kPoplarPlatformId;
}

HloCostAnalysis::ShapeSizeFunction PoplarCompiler::ShapeSizeBytesFunction()
    const {
  return PoplarExecutable::ShapeSizeBytes;
}

std::mutex PoplarCompiler::static_mu_;

}  // namespace poplarplugin
}  // namespace xla

static std::unique_ptr<xla::ComputationPlacer> CreateComputationPlacer() {
  return absl::make_unique<xla::ComputationPlacer>();
}

static bool RegisterComputationPlacer() {
  xla::ComputationPlacer::RegisterComputationPlacer(
      xla::poplarplugin::kPoplarPlatformId, &CreateComputationPlacer);
  return true;
}

bool placer_registration = RegisterComputationPlacer();

static bool InitModule() {
  xla::Compiler::RegisterCompilerFactory(
      xla::poplarplugin::kPoplarPlatformId,
      []() { return absl::make_unique<xla::poplarplugin::PoplarCompiler>(); });
  return true;
}
static bool module_initialized = InitModule();
