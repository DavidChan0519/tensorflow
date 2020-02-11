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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_VISITORS_VISITOR_INLINE_CALL_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_VISITORS_VISITOR_INLINE_CALL_H_

#include "tensorflow/compiler/plugin/poplar/driver/visitors/visitor_full.h"

namespace poplar {
class Graph;
class Tensor;
}  // namespace poplar

namespace xla {
namespace poplarplugin {

class InlineCallVisitor : public FullVisitor {
 public:
  InlineCallVisitor(CompilerResources& res, const TensorVectors& inputs);

  Status HandleParameter(HloInstruction* inst) override;
  Status FinishVisit(HloInstruction* inst) override;

  const TensorVector& outputs() { return outputs_; }

 private:
  TensorVectors inputs_;
  TensorVector outputs_;
};

}  // namespace poplarplugin
}  // namespace xla

#endif
