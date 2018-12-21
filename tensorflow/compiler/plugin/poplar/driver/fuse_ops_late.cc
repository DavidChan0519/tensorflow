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

#include "tensorflow/compiler/plugin/poplar/driver/fuse_ops_late.h"
#include "tensorflow/compiler/plugin/poplar/driver/inplace_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/matcher_predicates.h"

#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status.h"

namespace xla {
namespace poplarplugin {

static const std::vector<FusedGraphInfo> fuse_info = {
    {"const_slice_update", 0},
    {"const_slice_update", 0},
    {"const_slice", 0},
    {"const_slice", 0},
    {"relu", 0, InplaceUtil::InplaceHloInstructionDescription({0})},
    {"relu", 0, InplaceUtil::InplaceHloInstructionDescription({0})},
    {"sigmoid", 0, InplaceUtil::InplaceHloInstructionDescription({0})},
    {"sigmoid", 0, InplaceUtil::InplaceHloInstructionDescription({0})},
    {"relugrad", 0},
    {"relugrad", 0},
    {"sigmoidgrad", 0},
    {"sigmoidgrad", 0},
    {"biasadd", 0, InplaceUtil::InplaceHloInstructionDescription({0})},
    {"biasadd", 0, InplaceUtil::InplaceHloInstructionDescription({0})},
    {"zero_pad", 0},
    {"norm_scale_add", 4},
    {"norm_scale_add", 6},
    {"uniform_scale_add", 4},
    {"uniform_scale_add", 6},
    {"avg_pool", 1},
    {"avg_pool", 1},
    {"avg_pool", 1},
    {"bias_apply", 0, InplaceUtil::InplaceHloInstructionDescription({0})},
    {"conv_scaled_inplace", 4,
     InplaceUtil::InplaceHloInstructionDescription({0})},
    {"conv_scaled_inplace", 4,
     InplaceUtil::InplaceHloInstructionDescription({0})},
    {"scaled_inplace", 0, InplaceUtil::InplaceHloInstructionDescription({0})},
    {"scaled_inplace", 0, InplaceUtil::InplaceHloInstructionDescription({0})},
    {"padding_reduce_window", 0},
};

/*
 * Note about constructing these patterns.  Due to the behaviour of the fuser
 * there must be no backward references.  All nodes should appear after any
 * other nodes that refer to them.
 *
 * NOTE: Highest match priority is nearer the top of the list
 */

static const std::vector<HloMatcherPattern> patterns = {
    // dynamic update slice with constant coordinate
    {{HloOpcode::kDynamicUpdateSlice, true, 0, nullptr, {2, 3, 1}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}}},

    // dynamic update slice with wide constant coordinate
    {{HloOpcode::kDynamicUpdateSlice, true, 0, nullptr, {3, 4, 1}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {2}},
     {HloOpcode::kConstant, true, 0, IsScalarConstant, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}}},

    // dynamic slice with constant coordinate
    {{HloOpcode::kDynamicSlice, true, 0, nullptr, {2, 1}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // dynamic slice with wide constant coordinate
    {{HloOpcode::kDynamicSlice, true, 0, nullptr, {3, 1}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {2}},
     {HloOpcode::kConstant, true, 0, IsScalarConstant, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // Relu
    {{HloOpcode::kMaximum, true, 0, IsFloatType, {2, 1}},
     {HloOpcode::kConstant, true, 0, IsConstantZero, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // Relu with broadcast
    {{HloOpcode::kMaximum, true, 0, IsFloatType, {3, 1}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {2}},
     {HloOpcode::kConstant, true, 0, IsConstantZero, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // Sigmoid
    {{HloOpcode::kAdd, true, 0, IsFloatType, {4, 1}},
     {HloOpcode::kMultiply, true, 0, nullptr, {4, 2}},
     {HloOpcode::kTanh, true, 0, nullptr, {3}},
     {HloOpcode::kMultiply, true, 0, nullptr, {4, 5}},
     {HloOpcode::kConstant, true, 0, IsConstantHalf, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // Sigmoid with broadcast
    {{HloOpcode::kAdd, true, 0, IsFloatType, {1, 4}},
     {HloOpcode::kMultiply, true, 0, nullptr, {2, 4}},
     {HloOpcode::kTanh, true, 0, nullptr, {3}},
     {HloOpcode::kMultiply, true, 0, nullptr, {6, 4}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {5}},
     {HloOpcode::kConstant, true, 0, IsConstantHalf, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // ReluGrad
    {{HloOpcode::kSelect, true, 0, IsFloatType, {1, 3, 2}},
     {HloOpcode::kGt, true, 0, IsTfReluGradOp, {4, 2}},
     {HloOpcode::kConstant, true, 0, IsConstantZero, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // ReluGrad with broadcast
    {{HloOpcode::kSelect, true, 0, IsFloatType, {1, 4, 2}},
     {HloOpcode::kGt, true, 0, IsTfReluGradOp, {5, 2}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {3}},
     {HloOpcode::kConstant, true, 0, IsConstantZero, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // SigmoidGrad
    {{HloOpcode::kMultiply, true, 0, IsFloatType, {1, 2}},
     {HloOpcode::kMultiply, true, 0, nullptr, {4, 5}},
     {HloOpcode::kSubtract, true, 0, nullptr, {3, 5}},
     {HloOpcode::kConstant, true, 0, IsConstantOne, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // SigmoidGrad with broadcast
    {{HloOpcode::kMultiply, true, 0, IsFloatType, {1, 2}},
     {HloOpcode::kMultiply, true, 0, nullptr, {5, 6}},
     {HloOpcode::kSubtract, true, 0, nullptr, {3, 6}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {4}},
     {HloOpcode::kConstant, true, 0, IsConstantOne, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // BiasAdd on convolution (w/ broadcast)
    {{HloOpcode::kAdd, true, 0, nullptr, {2, 1}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {3}},
     {HloOpcode::kCall, false, 0, IsPopOpsConvolution, {}},
     {HloOpcode::kParameter, false, 1, Is1DVector, {}}},

    // BiasAdd on convolution (w/ broadcast)
    {{HloOpcode::kAdd, true, 0, nullptr, {2, 1}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {3}},
     {HloOpcode::kConvolution, false, 0, nullptr, {}},
     {HloOpcode::kParameter, false, 1, Is1DVector, {}}},

    // External padding with constant zero
    {{HloOpcode::kPad, true, 0, IsExternalPadding, {2, 1}},
     {HloOpcode::kConstant, true, 0, IsConstantZero, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // Random normal with post scale and add
    {{HloOpcode::kAdd, true, 0, nullptr, {2, 1}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kMultiply, true, 0, nullptr, {4, 3}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kRng, true, 0, IsRandomNormal, {5, 6}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kConstant, true, 0, nullptr, {}}},

    // Random normal with broadcasted post scale and add
    {{HloOpcode::kAdd, true, 0, nullptr, {3, 1}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {2}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kMultiply, true, 0, nullptr, {6, 4}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {5}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kRng, true, 0, IsRandomNormal, {7, 8}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kConstant, true, 0, nullptr, {}}},

    // Random uniform with post scale and add
    {{HloOpcode::kAdd, true, 0, nullptr, {2, 1}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kMultiply, true, 0, nullptr, {4, 3}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kRng, true, 0, IsRandomUniform, {5, 6}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kConstant, true, 0, nullptr, {}}},

    // Random uniform with broadcasted post scale and add
    {{HloOpcode::kAdd, true, 0, nullptr, {3, 1}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {2}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kMultiply, true, 0, nullptr, {6, 4}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {5}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kRng, true, 0, IsRandomUniform, {7, 8}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kConstant, true, 0, nullptr, {}}},

    // Average pool (valid)
    {{HloOpcode::kDivide, true, 0, IsAveragePool, {1, 3}},
     {HloOpcode::kReduceWindow, true, 0, Is2DReductionWindow, {4, 2}},
     {HloOpcode::kConstant, true, 0, IsConstantZero, {}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // Average pool (same)
    {{HloOpcode::kDivide, true, 0, IsAveragePool, {1, 2}},
     {HloOpcode::kReduceWindow, true, 0, Is2DReductionWindow, {7, 6}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {3}},
     {HloOpcode::kReduceWindow, true, 0, nullptr, {4, 6}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {5}},
     {HloOpcode::kConstant, true, 0, IsConstantOne, {}},
     {HloOpcode::kConstant, true, 0, IsConstantZero, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // Average pool (same) - broadcast converted to reshape
    {{HloOpcode::kDivide, true, 0, IsAveragePool, {1, 2}},
     {HloOpcode::kReduceWindow, true, 0, Is2DReductionWindow, {7, 6}},
     {HloOpcode::kReshape, true, 0, nullptr, {3}},
     {HloOpcode::kReduceWindow, true, 0, nullptr, {4, 6}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {5}},
     {HloOpcode::kConstant, true, 0, IsConstantOne, {}},
     {HloOpcode::kConstant, true, 0, IsConstantZero, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}}},

    // Bias reduction and application
    {{HloOpcode::kSubtract, true, 0, IsOutputFeed, {1, 2}},
     {HloOpcode::kParameter, false, 0, IsTrueParameter, {}},
     {HloOpcode::kMultiply, true, 0, nullptr, {5, 3}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {4}},
     {HloOpcode::kConstant, true, 0, nullptr, {}},
     {HloOpcode::kReduce, true, 0, IsBiasReduce, {7, 6}},
     {HloOpcode::kConstant, true, 0, IsConstantZero, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}}},

    // Convolution followed by scaled add to - A = A + B * c
    {{HloOpcode::kAdd, true, 0, nullptr, {5, 1}},
     {HloOpcode::kMultiply, true, 0, nullptr, {4, 2}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {3}},
     {HloOpcode::kConstant, true, 0, IsScalarConstant, {}},
     {HloOpcode::kConvolution, true, 0, nullptr, {6, 7}},
     {HloOpcode::kParameter, false, 0, nullptr, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}},
     {HloOpcode::kParameter, false, 2, nullptr, {}}},

    // Convolution followed by scaled subtract from - A = A - B * c
    {{HloOpcode::kSubtract, true, 0, nullptr, {5, 1}},
     {HloOpcode::kMultiply, true, 0, nullptr, {4, 2}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {3}},
     {HloOpcode::kConstant, true, 0, IsScalarConstant, {}},
     {HloOpcode::kConvolution, true, 0, nullptr, {6, 7}},
     {HloOpcode::kParameter, false, 0, nullptr, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}},
     {HloOpcode::kParameter, false, 2, nullptr, {}}},

    // Scaled add to - A = A + B * c
    {{HloOpcode::kAdd, true, 0, nullptr, {4, 1}},
     {HloOpcode::kMultiply, true, 0, nullptr, {5, 2}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {3}},
     {HloOpcode::kConstant, true, 0, IsScalarConstant, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}}},

    // Scaled subtract from - A = A - B * c
    {{HloOpcode::kSubtract, true, 0, nullptr, {4, 1}},
     {HloOpcode::kMultiply, true, 0, nullptr, {5, 2}},
     {HloOpcode::kBroadcast, true, 0, nullptr, {3}},
     {HloOpcode::kConstant, true, 0, IsScalarConstant, {}},
     {HloOpcode::kParameter, false, 0, nullptr, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}}},

    // Reduce window with a window size of 1x1, stride 1 and identity reduction
    // function (param 1 is returned)
    {{HloOpcode::kReduceWindow, true, 0, IsPaddingReduceWindow, {1, 2}},
     {HloOpcode::kParameter, false, 0, nullptr, {}},
     {HloOpcode::kParameter, false, 1, nullptr, {}}},
};

FuseOpsLate::FuseOpsLate(struct CompilerAnnotations& annotations)
    : SingleHloMatcher(annotations, patterns, fuse_info, "_pop_op_") {}

}  // namespace poplarplugin
}  // namespace xla
