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

#include "tensorflow/compiler/plugin/poplar/driver/tools/flags.h"

#include "absl/container/flat_hash_map.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"
#include "absl/strings/strip.h"
#include "tensorflow/compiler/xla/parse_flags_from_env.h"
#include "tensorflow/core/util/command_line_flags.h"

#include <mutex>
#include <sstream>

namespace tensorflow {
namespace {

PoplarXlaFlags* poplar_xla_flags;

std::once_flag flags_init;

absl::flat_hash_map<std::string, std::string> GetFlagUsage() {
  static absl::flat_hash_map<std::string, std::string> flag_usage = {
      {"help", "Display all the flags infos. (bool)"},
      {"use_synthetic_data",
       "If enabled, there will be no data transfers between the host and the "
       "IPU(s). (bool)"},
      {"use_ipu_model",
       "If enabled, this computation will be executed on the IPU model. "
       "(bool)"},
      {"force_replicated_mode",
       "If enabled, we allow replicated graphs with no AllReduce operations in "
       "them to still run in replicated mode. (bool)"},
      {"while_loop_brute_force_max_trip_count",
       "When trying to convert a while loop to a repeat loop, we can try and "
       "use a brute force method to simulate the conditional part of the while "
       "and find the number of iterations. This flag sets how many iterations "
       "of the while loop we should try and brute force it for. (int=128)"},
      {"max_compilation_threads",
       "The maximum number of threads Poplar should use during compilation of "
       "the graph. Negative value allows Poplar to pick the number of threads "
       "automatically. (int=-1)"},
      {"save_oom_profiler",
       "Path to a file where the profiling information is saved to when an Out "
       "Of Memory error occurs. (path)"},
      {"save_vertex_graph",
       "Path to a file where the Poplar vertex graph should be saved to. "
       "(path)"},
      {"executable_cache_path", "Path to the executable cache. (path)"},
      {"dump_schedule_as_dot", "Dumps the scheduler graph as a dot file."},
      {"tensor_map_file_path", "Directory for tensor map dump files."},
      {"fallback_scheduler",
       "Use the sync list scheduler rather than the default one."},
      {"add_all_reduce_copies",
       "EXPERIMENTAL Adds extra copies before performing an all reduce "
       "operation - can improve compiler performance."}};
  return flag_usage;
}

void AllocateAndParseFlags() {
  poplar_xla_flags = new PoplarXlaFlags;
  // Display all the flags infos.
  poplar_xla_flags->help = false;

  // If enabled, there will be no data transfers between the host and the
  // IPU(s).
  poplar_xla_flags->use_synthetic_data = false;

  // If enabled, this computation will be executed on the IPU model.
  poplar_xla_flags->use_ipu_model = false;

  // If enabled, we allow replicated graphs with no AllReduce operations in them
  // to still run in replicated mode.
  poplar_xla_flags->force_replicated_mode = false;

  // When trying to convert a while loop to a repeat loop, we can try and use a
  // brute force method to simulate the conditional part of the while and find
  // the number of iterations. This flag sets how many iterations of the while
  // loop we should try and brute force it for (default 128).
  poplar_xla_flags->while_loop_brute_force_max_trip_count = 128;

  // The maximum number of threads Poplar should use during compilation of the
  // graph.
  poplar_xla_flags->max_compilation_threads = -1;

  // Path to a file where the profiling information is saved to when an Out Of
  // Memory occurs.
  poplar_xla_flags->save_oom_profiler = "";

  // Path to a file where the Poplar vertex graph should be saved to.
  poplar_xla_flags->save_vertex_graph = "";

  // Path to the executable cache.
  poplar_xla_flags->executable_cache_path = "";

  // Path for tensormap files
  poplar_xla_flags->tensor_map_file_path = "";

  // Dump the schedule graph as a dot to VLOG.
  poplar_xla_flags->dump_schedule_as_dot = false;

  // Use the fallback scheduler instead of the default one.
  poplar_xla_flags->fallback_scheduler = false;

  // TODO T8856 - remove this flag.
  // Indicates whether to add the copies before the all reduce.
  poplar_xla_flags->add_all_reduce_copies = false;

  auto flag_usage = GetFlagUsage();

  std::vector<Flag> flag_list = {
#define ADD_FLAG(FLAG_NAME) \
  Flag(#FLAG_NAME, &poplar_xla_flags->FLAG_NAME, flag_usage.at(#FLAG_NAME)),
      // clang-format off
    ADD_FLAG(help)
    ADD_FLAG(use_synthetic_data)
    ADD_FLAG(use_ipu_model)
    ADD_FLAG(force_replicated_mode)
    ADD_FLAG(while_loop_brute_force_max_trip_count)
    ADD_FLAG(max_compilation_threads)
    ADD_FLAG(save_oom_profiler)
    ADD_FLAG(save_vertex_graph)
    ADD_FLAG(executable_cache_path)
    ADD_FLAG(dump_schedule_as_dot)
    ADD_FLAG(tensor_map_file_path)
    ADD_FLAG(fallback_scheduler)
    ADD_FLAG(add_all_reduce_copies)

// clang-format on
#undef ADD_FLAG
  };
  xla::ParseFlagsFromEnvAndDieIfUnknown("TF_POPLAR_FLAGS", flag_list);

  // Store all the flags as a string.
  poplar_xla_flags->as_string = "";
  if (const char* flag_buffer = std::getenv("TF_POPLAR_FLAGS")) {
    poplar_xla_flags->as_string = flag_buffer;
  }
}

}  // namespace

const PoplarXlaFlags& GetPoplarXlaFlags() {
  std::call_once(flags_init, &AllocateAndParseFlags);
  return *poplar_xla_flags;
}

const std::string GetFlagUsageString() {
  auto flag_usage = GetFlagUsage();
  std::stringstream usage_stream;
  usage_stream << "Usage for TF_POPLAR_FLAGS is:" << std::endl;
  for (auto pair : flag_usage) {
    usage_stream << "\t--" << pair.first << ": " << pair.second << std::endl;
  }
  return usage_stream.str();
}
}  // namespace tensorflow
