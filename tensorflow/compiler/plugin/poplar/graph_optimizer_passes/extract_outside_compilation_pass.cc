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

#include "tensorflow/compiler/plugin/poplar/graph_optimizer_passes/extract_outside_compilation_pass.h"

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/strings/str_cat.h"
#include "tensorflow/compiler/jit/encapsulate_subgraphs_pass.h"
#include "tensorflow/compiler/jit/encapsulate_util.h"
#include "tensorflow/compiler/jit/extract_outside_compilation_pass.h"
#include "tensorflow/compiler/tf2xla/tf2xla_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/core/common_runtime/device_factory.h"
#include "tensorflow/core/common_runtime/function.h"
#include "tensorflow/core/common_runtime/placer.h"
#include "tensorflow/core/framework/function.h"
#include "tensorflow/core/framework/graph_to_functiondef.h"
#include "tensorflow/core/framework/node_def_builder.h"
#include "tensorflow/core/framework/node_def_util.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/public/version.h"
#include "tensorflow/core/util/dump_graph.h"

namespace tensorflow {

namespace {

bool IsKeyPlaceholderNode(const Node& n) {
  return n.type_string() == "Placeholder" &&
         absl::EndsWith(n.name(), "_key_placeholder");
}

bool IsSequencerNode(const Node& n) {
  return n.type_string() == "NoOp" &&
         HasNodeAttr(n.def(), "_xla_host_transfer_sequencer");
}

bool IsXlaLaunchNode(const Node& n) { return n.type_string() == "XlaLaunch"; }

Status ReplaceKeyPlaceholdersWithConstants(Graph* g) {
  for (Node* n : g->nodes()) {
    if (IsKeyPlaceholderNode(*n)) {
      NodeDef const_def;
      TF_RETURN_IF_ERROR(NodeDefBuilder(n->name(), "Const")
                             .Attr("dtype", DT_STRING)
                             .Attr("value", Tensor(DT_STRING))
                             .Finalize(&const_def));
      TF_ASSIGN_OR_RETURN(n, ReplaceNode(g, n, const_def));
    }
  }

  return Status::OK();
}

void RemoveSequencerNodes(Graph* g) {
  for (Node* n : g->nodes()) {
    if (IsSequencerNode(*n)) {
      g->RemoveNode(n);
    }
  }
}

Status CheckForXlaSendToHostNodes(const FunctionDef* function_def) {
  for (const NodeDef& n : function_def->node_def()) {
    if (n.op() == "XlaSendToHost") {
      return errors::Unimplemented(
          "`outside_compilation_scope` enclosed in control flow "
          "(loop or cond) is not supported");
    }
  }

  return Status::OK();
}

std::unordered_map<string, XlaClusterInfo> FindClusters(Graph* g) {
  std::unordered_map<string, XlaClusterInfo> clusters;

  for (Node* n : g->op_nodes()) {
    if (IsXlaLaunchNode(*n)) {
      const AttrValue* f = n->attrs().Find("function");
      CHECK_NOTNULL(f);
      CHECK(f->has_func());
      const string& func = f->func().name();
      NameAttrList func_name_attrs;
      func_name_attrs.set_name(func);
      clusters.emplace(func, XlaClusterInfo{func, func_name_attrs, n,
                                            std::map<string, int>{}});
    }
  }

  return clusters;
}

static constexpr char kXlaClusterAttrName[] = "_XlaCluster";

}  // namespace

Status ExtractOutsideCompilationPass::Run(
    const GraphOptimizationPassOptions& options) {
  FunctionLibraryDefinition* flib_def = options.flib_def;
  TF_RET_CHECK(flib_def != nullptr);

  auto pflr = absl::make_unique<ProcessFunctionLibraryRuntime>(
      nullptr, options.session_options->env, TF_GRAPH_DEF_VERSION, flib_def,
      OptimizerOptions());

  Graph* graph = options.graph->get();

  const auto clusters = FindClusters(graph);

  FunctionLibraryRuntime* flr =
      pflr->GetFLR(ProcessFunctionLibraryRuntime::kDefaultFLRDevice);
  TF_RET_CHECK(flr);

  bool modified = false;

  // Rewrites XLA computation in `clusters` to replace outside compilation nodes
  // with XlaHostCompute, and moves those outside compilations into `graph`.
  TF_RETURN_IF_ERROR(ExtractOutsideCompilation(
      kXlaClusterAttrName, kXlaOutsideCompilationAttrName, clusters, graph, flr,
      flib_def, &modified));

  if (!modified) {
    return Status::OK();
  }

  if (VLOG_IS_ON(4)) {
    DumpGraphToFile("extract_outside_compilation_fixup_before", *graph,
                    flib_def);
  }

  // According to the docs in tpu_host_compute_ops.cc, the key placeholder is
  // supposed to be sent at runtime by the compile node to identify which
  // execution the transfer corresponds to. We should have control over this
  // by the Send/Recv registration in poplar_compiler.cc and poplar_executor.cc.
  TF_RETURN_IF_ERROR(ReplaceKeyPlaceholdersWithConstants(graph));

  // The sequencer node has control input edges from the
  // _XlaRecvAtHost/_XlaSendFromHost ops and a control output edge to the
  // XlaLaunch op, so it requires the Send/Recv to complete before engine
  // compilation, which does not match our architecture, resulting in deadlock.
  // To be honest, not really sure what it's supposed to do for TPUs.
  RemoveSequencerNodes(graph);

  // XlaSendToHost nodes are inserted when the outside compilation scope
  // is enclosed in control flow. This is not currently supported, so
  // report a nice error message if we find any.
  for (const auto& func : flib_def->ListFunctionNames()) {
    const FunctionDef* function_def = flib_def->Find(func);
    TF_RET_CHECK(function_def != nullptr);
    TF_RETURN_IF_ERROR(CheckForXlaSendToHostNodes(function_def));
  }

  // Run the placer again to assign devices to the nodes added by this pass.
  Placer placer(graph, "", options.device_set);
  TF_RETURN_IF_ERROR(placer.Run());

  if (VLOG_IS_ON(4)) {
    DumpGraphToFile("extract_outside_compilation_fixup_after", *graph,
                    flib_def);
  }

  return Status::OK();
}

}  // namespace tensorflow
