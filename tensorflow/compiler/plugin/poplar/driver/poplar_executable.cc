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

#include "tensorflow/compiler/plugin/poplar/driver/poplar_executable.h"

#include <fstream>
#include <utility>

#include "tensorflow/compiler/plugin/poplar/driver/poplar_executable.pb.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_platform.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/poplar_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/plugin/poplar/driver/xla_ipu_common.h"

namespace xla {
namespace poplarplugin {

PoplarExecutable::PoplarExecutable(
    std::unique_ptr<HloModule> hlo_module,
    std::unique_ptr<HloProfilePrinterData> profile_printer,
    std::unique_ptr<HloProfileIndexMap> profile_index_map,
    std::unique_ptr<poplar::Engine> engine,
    const InputOutputAliasingMap& input_output_aliasing_map,
    const bool is_constant_graph,
    std::vector<std::vector<Literal>> literal_output, const bool is_remap_graph,
    std::vector<uint64> remaped_output, uint32 replication_factor,
    const InfeedInfos& infeed_infos, const OutfeedInfos& outfeed_infos,
    StreamInfos&& stream_infos, StreamMetaInfos&& stream_meta_info,
    SendRecvInfos&& send_infos, SendRecvInfos&& recv_infos)
    : Executable(std::move(hlo_module), std::move(profile_printer),
                 std::move(profile_index_map)),
      poplar_engine_(std::move(engine)),
      input_output_aliasing_map_(std::move(input_output_aliasing_map)),
      literal_output_(std::move(literal_output)),
      is_constant_graph_(is_constant_graph),
      remaped_output_(std::move(remaped_output)),
      is_remap_graph_(is_remap_graph),
      execution_count_(0),
      replication_factor_(replication_factor),
      infeed_infos_(std::move(infeed_infos)),
      outfeed_infos_(std::move(outfeed_infos)),
      stream_infos_(std::move(stream_infos)),
      stream_meta_infos_(std::move(stream_meta_info)),
      send_infos_(std::move(send_infos)),
      recv_infos_(std::move(recv_infos)),
      loaded_from_cache_(false) {}

PoplarExecutable::~PoplarExecutable() {
  if (poplar_engine_.get() != nullptr) {
    auto platform =
        se::MultiPlatformManager::PlatformWithName(tensorflow::PLATFORM_NAME);
    if (platform.ok()) {
      auto* p = static_cast<PoplarPlatform*>(platform.ValueOrDie());
      p->AboutToFreeEngine(poplar_engine_.get());
    }
  }
}

StatusOr<ScopedShapedBuffer> PoplarExecutable::ExecuteAsyncOnStream(
    const ServiceExecutableRunOptions* run_options,
    absl::Span<const ShapedBuffer* const> arguments,
    HloExecutionProfile* hlo_execution_profile) {
  se::Stream* stream = run_options->stream();

  std::vector<se::DeviceMemoryBase> argument_buffers;
  for (size_t i = 0; i < arguments.size(); ++i) {
    argument_buffers.push_back(arguments[i]->buffer(/*index=*/{}));
  }

  VLOG(1) << "Execute " << module().name();
  if (VLOG_IS_ON(2)) {
    for (const auto& a : argument_buffers) {
      VLOG(2) << "-- argument " << a.opaque();
    }
  }

  uint64 start_micros = tensorflow::Env::Default()->NowMicros();

  perftools::gputools::StreamExecutor* executor(stream->parent());
  PoplarExecutor* poplarExecutor(
      static_cast<PoplarExecutor*>(executor->implementation()));

  if (poplarExecutor->ConnectionType() == DeviceConnectionType::NEVER) {
    return InvalidArgument(
        "Trying to run an executable on a device that was configured for "
        "compilation only.");
  }

  if (!poplarExecutor->PoplarDeviceIsAttached()) {
    TF_RETURN_IF_ERROR(poplarExecutor->AttachToPoplarDevice());
  }
  se::DeviceMemoryAllocator* memory_allocator = run_options->allocator();

  se::DeviceMemoryBase result;
  PoplarExecutor::AsPoplarStream(stream)->BlockUntilDone();
  TF_ASSIGN_OR_RETURN(
      result, poplarExecutor->ExecuteEngine(executor, *this, memory_allocator,
                                            argument_buffers));

  execution_count_++;
  if (poplarExecutor->ReportEventNthExecution() > 0 &&
      execution_count_ >= poplarExecutor->ReportEventNthExecution()) {
    execution_count_ = 0;
  }

  uint64 end_micros = tensorflow::Env::Default()->NowMicros();

  if (run_options->run_options().execution_profile()) {
    auto profile = run_options->run_options().execution_profile();
    const double nanoseconds = (end_micros - start_micros) * 1000.0;
    profile->set_compute_time_ns(std::max(nanoseconds, 1.0));
    profile->set_compute_cycle_count(1);
  }

  ScopedShapedBuffer result_buffer(result_shape(), result_shape(),
                                   run_options->allocator(),
                                   stream->parent()->device_ordinal());

  // Copy DeviceMemoryBase values which contain the array(s) of the result into
  // the respective location in ShapedBuffer which is returned to the caller.

  TF_RETURN_IF_ERROR(result_buffer.buffers().ForEachMutableElementWithStatus(
      [&result, poplarExecutor](const ShapeIndex& index,
                                se::DeviceMemoryBase* device_memory) {
        se::DeviceMemoryBase buffer = result;
        for (auto i : index) {
          TF_ASSIGN_OR_RETURN(buffer,
                              poplarExecutor->GetTupleBufferByIndex(buffer, i));
        }
        CHECK(!buffer.is_null() || buffer.size() == 0);
        if (VLOG_IS_ON(2)) {
          VLOG(2) << "-- return " << buffer.opaque();
        }
        *device_memory = buffer;
        return Status::OK();
      }));

  return std::move(result_buffer);
}

/*static*/ int64 PoplarExecutable::ShapeSizeBytes(const Shape& shape) {
  if (shape.IsOpaque()) {
    return sizeof(void*);
  }
  return ShapeUtil::ByteSizeOf(shape, sizeof(void*));
}

/*static*/ StatusOr<PoplarExecutable*> PoplarExecutable::Deserialize(
    std::unique_ptr<HloModule> hlo_module,
    std::unique_ptr<HloProfilePrinterData> profile_printer,
    std::unique_ptr<HloProfileIndexMap> profile_index_map,
    const std::string& filename) {
  PoplarExecutableProto proto;

  TF_RETURN_IF_ERROR(
      ReadBinaryProto(tensorflow::Env::Default(), filename, &proto));

  // Load metadata
  int replication_factor = proto.replication_factor();

  InfeedInfos infeeds;
  for (const auto& infeed : proto.infeeds()) {
    infeeds.emplace_back(infeed.stream_prefix(), infeed.config(),
                         Shape(infeed.shape()));
  }

  OutfeedInfos outfeeds;
  for (const auto& outfeed : proto.outfeeds()) {
    outfeeds.emplace_back(outfeed.stream_prefix(), outfeed.config(),
                          Shape(outfeed.shape()));
  }

  SendRecvInfos sends;
  for (const auto& send : proto.sends()) {
    sends.emplace_back(send.stream_handle(), send.rendezvous_key(),
                       Shape(send.shape()));
  }

  SendRecvInfos recvs;
  for (const auto& recv : proto.recvs()) {
    recvs.emplace_back(recv.stream_handle(), recv.rendezvous_key(),
                       Shape(recv.shape()));
  }

  // Load the poplar compilation options from the serialized executable
  poplar::OptionFlags opts;
  for (const auto& flag : proto.option_flags()) {
    opts.set(flag.option(), flag.value());
  }

  // Load the executable
  std::string poplar_executable_filename = proto.engine();
  std::unique_ptr<poplar::Engine> engine;
  try {
    std::ifstream file(poplar_executable_filename, std::ios::binary);
    auto poplar_executable = poplar::Executable::deserialize(file);
    engine.reset(new poplar::Engine(std::move(poplar_executable), opts));
  } catch (const std::exception& e) {
    return PoplarExceptionToTensorflowStatus("[Deserialize] ", e);
  }

  auto iomap = InputOutputAliasingMap(hlo_module.get());

  auto executable = new PoplarExecutable(
      std::move(hlo_module), std::move(profile_printer),
      std::move(profile_index_map), std::move(engine), std::move(iomap), false,
      {}, false, {}, replication_factor, std::move(infeeds),
      std::move(outfeeds), {}, {}, std::move(sends), std::move(recvs));

  executable->loaded_from_cache_ = true;

  return executable;
}

/*static*/ Status PoplarExecutable::Serialize(
    const std::string& filename, const poplar::Executable& executable,
    const InfeedInfos& infeeds, const OutfeedInfos& outfeeds,
    const SendRecvInfos& sends, const SendRecvInfos& recvs,
    uint32 replication_count, const poplar::OptionFlags& opts) {
  PoplarExecutableProto proto;

  // Write poplar executable to a file
  std::string poplar_executable_filename = filename + ".poplar_exec";
  try {
    auto file = std::ofstream(poplar_executable_filename, std::ios::binary);
    executable.serialize(file);
  } catch (const std::exception& e) {
    return PoplarExceptionToTensorflowStatus("[Serialize] ", e);
  }

  proto.set_engine(poplar_executable_filename);

  proto.set_replication_factor(replication_count);

  for (const auto& infeed : infeeds) {
    auto* feed = proto.add_infeeds();
    feed->set_stream_prefix(infeed.stream_prefix);
    *(feed->mutable_config()) = infeed.config;
    *(feed->mutable_shape()) = infeed.shape.ToProto();
  }

  for (const auto& outfeed : outfeeds) {
    auto* feed = proto.add_outfeeds();
    feed->set_stream_prefix(outfeed.stream_prefix);
    *(feed->mutable_config()) = outfeed.config;
    *(feed->mutable_shape()) = outfeed.shape.ToProto();
  }

  for (const auto& send : sends) {
    auto* send_proto = proto.add_sends();
    send_proto->set_stream_handle(send.stream_handle);
    send_proto->set_rendezvous_key(send.rendezvous_key);
    *(send_proto->mutable_shape()) = send.shape.ToProto();
  }

  for (const auto& recv : recvs) {
    auto* recv_proto = proto.add_recvs();
    recv_proto->set_stream_handle(recv.stream_handle);
    recv_proto->set_rendezvous_key(recv.rendezvous_key);
    *(recv_proto->mutable_shape()) = recv.shape.ToProto();
  }

  // write the compilation options into the serialized executable
  for (const auto flag : opts) {
    auto* poplar_opt = proto.add_option_flags();
    poplar_opt->set_option(flag.first);
    poplar_opt->set_value(flag.second);
  }

  return WriteBinaryProto(tensorflow::Env::Default(), filename, proto);
}

}  // namespace poplarplugin
}  // namespace xla
