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

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Regex.h"
#include "llvm/TableGen/Main.h"
#include "llvm/TableGen/Record.h"
#include "llvm/TableGen/TableGenBackend.h"
#include "mlir/TableGen/Operator.h"  // TF:local_config_mlir

using llvm::LessRecord;
using llvm::raw_ostream;
using llvm::Record;
using llvm::RecordKeeper;
using mlir::tblgen::Operator;

// Helper macro that returns indented os.
#define OUT(X) os.indent((X))

// The function below has a non-constant reference as that is required by LLVM's
// TableGenMain.
// NOLINTNEXTLINE
static bool OpQuantSpecWriter(raw_ostream &os, RecordKeeper &records) {
  llvm::Regex acc_uniform_trait_regex{"AccumulatorUniformScale<([0-9]*),"};

  emitSourceFileHeader("TensorFlow Lite Ops Quant Spec Getters", os);

  // Retrieve all the definitions derived from TFL_Op and sort by record name.
  std::vector<Record *> defs = records.getAllDerivedDefinitions("Op");
  llvm::sort(defs, LessRecord());

  OUT(0) << "static std::unique_ptr<OpQuantSpec> "
            "GetOpQuantSpec(mlir::Operation *op) {\n";
  OUT(2) << "auto spec = absl::make_unique<OpQuantSpec>();\n";
  for (auto *def : defs) {
    Operator op(def);
    for (const auto t : op.getTraits()) {
      if (auto opTrait = llvm::dyn_cast<mlir::tblgen::NativeOpTrait>(&t)) {
        auto trait = opTrait->getTrait();
        // We only handle TFL specific native op traits.
        if (!trait.startswith("TFL::")) continue;
        trait.consume_front("TFL::");

        OUT(2) << "if (auto tfl = llvm::dyn_cast<" << op.getQualCppClassName()
               << ">(op)) {\n";

        // There is a "NoQuantizableResult" trait, set the flag.
        if (trait.equals("NoQuantizableResult")) {
          OUT(4) << "spec->is_quantizable = false;\n";
        }
        // There is a "SameOperandsAndResultScale" trait, set the flag.
        if (trait.equals("SameOperandsAndResultsScale")) {
          OUT(4) << "spec->requires_same_scale = true;\n";
        }
        // There is a "FixedResultUniformScale" trait, set the type for result.
        if (trait.startswith("FixedResultUniformScale")) {
          OUT(4) << "for (int i = 0, e = op->getNumResults(); i != e; ++i)\n";
          OUT(6) << "spec->restricted_output_params.push_back(tfl."
                    "GetResultQuantizedType(i));\n";
        }
        // There is a "AccumulatorUniformScale" trait, set the type for bias.
        auto trait_str = opTrait->getTrait().str();
        llvm::SmallVector<llvm::StringRef, 1> matches;
        if (acc_uniform_trait_regex.match(trait_str, &matches)) {
          OUT(4) << "spec->biases_params.emplace(std::make_pair(" << matches[1]
                 << ", std::make_pair(tfl.GetAllNonBiasOperands(),"
                 << "GetUniformQuantizedTypeForBias)));\n";
        }

        OUT(2) << "}\n";
      }
    }
  }
  OUT(2) << "return spec;\n";
  OUT(0) << "}\n";
  return false;
}

int main(int argc, char **argv) {
  llvm::PrettyStackTraceProgram X(argc, argv);
  llvm::InitLLVM y(argc, argv);
  llvm::cl::ParseCommandLineOptions(argc, argv);
  return TableGenMain(argv[0], &OpQuantSpecWriter);
}
