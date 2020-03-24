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

#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_COMPILER_INFORMATION_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_COMPILER_INFORMATION_H_

#include "tensorflow/compiler/xla/types.h"

namespace xla {
namespace poplarplugin {

// This structure contains all information which is used during the
// modifications/optimisation of the XLA graph.
struct CompilerInformation {
  CompilerInformation(int64 max_all_reduce_buffer_size,
                      int64 max_reduce_scatter_buffer_size,
                      int64 max_inter_ipu_copies_buffer_size,
                      int64 max_send_recv_cluster_size,
                      int64 max_scheduler_lookahead_depth_,
                      int64 max_scheduler_search_space_size_)
      : max_all_reduce_buffer_size(max_all_reduce_buffer_size),
        max_reduce_scatter_buffer_size(max_reduce_scatter_buffer_size),
        max_inter_ipu_copies_buffer_size(max_inter_ipu_copies_buffer_size),
        max_send_recv_cluster_size(max_send_recv_cluster_size),
        max_scheduler_lookahead_depth(max_scheduler_lookahead_depth_),
        max_scheduler_search_space_size(max_scheduler_search_space_size_) {}

  const int64 max_all_reduce_buffer_size;

  const int64 max_reduce_scatter_buffer_size;

  const int64 max_inter_ipu_copies_buffer_size;

  const int64 max_send_recv_cluster_size;

  const int64 max_scheduler_lookahead_depth;

  const int64 max_scheduler_search_space_size;
};

}  // namespace poplarplugin
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_COMPILER_INFORMATION_H_
