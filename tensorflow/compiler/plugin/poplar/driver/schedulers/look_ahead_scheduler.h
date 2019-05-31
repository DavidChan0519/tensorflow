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
#ifndef TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_SCHEDULERS_LOOK_AHEAD_SCHEDULER_H_
#define TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_SCHEDULERS_LOOK_AHEAD_SCHEDULER_H_

#include "tensorflow/compiler/xla/service/hlo_memory_scheduler.h"

namespace xla {
namespace poplarplugin {

// Scheduler which will look ahead and queue large chunks of the graph at a
// time.
MemorySchedulerAlgorithm CreateLookAheadMemoryScheduler(
    int64 maxmimum_all_reduce_buffer_size = 0);

}  // namespace poplarplugin
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_PLUGIN_POPLAR_DRIVER_SCHEDULERS_LOOK_AHEAD_SCHEDULER_H_
