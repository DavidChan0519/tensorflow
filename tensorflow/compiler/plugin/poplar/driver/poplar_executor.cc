/* Copyright 2017 Graphcore Ltd
 */

/* Copyright 2016 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/plugin/poplar/driver/poplar_executor.h"

#include <string.h>

#include <deque>
#include <fstream>
#include <unordered_map>
#include <utility>
#include <vector>

#include <poplar/DeviceManager.hpp>
#include <poplar/IPUModel.hpp>
#include <poplar/StreamCallback.hpp>
#include <poplar/Tensor.hpp>

#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"
#include "absl/types/optional.h"
#include "google/protobuf/util/message_differencer.h"
#include "include/json/json.h"

#include "tensorflow/compiler/plugin/poplar/driver/poplar_executable.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_platform.h"
#include "tensorflow/compiler/plugin/poplar/driver/poplar_platform_id.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/conversions.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/flags.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/hlo_hash.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/poplar_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/tools/util.h"
#include "tensorflow/compiler/plugin/poplar/driver/xla_ipu_common.h"

#include "tensorflow/compiler/tf2xla/shape_util.h"
#include "tensorflow/compiler/tf2xla/type_util.h"
#include "tensorflow/compiler/xla/service/hlo_instruction.h"
#include "tensorflow/compiler/xla/service/hlo_instructions.h"
#include "tensorflow/compiler/xla/service/transfer_manager.h"
#include "tensorflow/compiler/xla/status_macros.h"

#include "tensorflow/core/common_runtime/dma_helper.h"
#include "tensorflow/core/framework/dataset.h"
#include "tensorflow/core/framework/function_handle_cache.h"
#include "tensorflow/core/framework/types.pb.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/lib/strings/proto_serialization.h"
#include "tensorflow/core/lib/strings/stringprintf.h"
#include "tensorflow/core/public/version.h"

/*
 * TensorControl is a structure that maintains state about the location
 * of a tensor - either on the device or cached on the host.
 *
 * Tensorflow/XLA assumes that a tensor is on the device when the device
 * allocator is called (PoplarExecutor::Allocate).  However, Poplar cannot
 * allocate tensors independently of the compiled Engine.  The TensorControl
 * structure tracks where the tensors are.
 *
 * TensorControl has three pieces of interacting state:
 *   on_device: This says whether the data is on the device (in one of the
 *              tensors belonging to the currently loaded engine).  When this
 *              is false, it means the data is being held in the host side
 *              buffer.
 *
 *   input_handle: If the tensor is on_device, and this is not -1, then it
 *                 indicates which of the input tensors of the current engine
 *                 contains the data.
 *
 *   output_handle: If the tensor is on_device, and this is not empty, then it
 *                  indicates which of the output tensors of the current
 *                  engine contains the data.
 *
 *   The states are:
 *     on_device=false :
 *       The data is in the host buffer.  If this buffer is passed as an
 *       argument when an engine is executed then it must be copied to the
 *       device.
 *
 *     on_device=true, input_handle not empty, output_handle is empty :
 *       During the previous engine execution, the data was copied to the
 *       device as one of the arguments.  On the next execution, if the engine
 *       does not change, and the argument index is the same, then the data
 *       does not need to be recopied to the device.  I suspect that this case
 *       is rare.
 *
 *     on_device=true, input_handle is empty, output_handle not empty :
 *       During the last execution, the buffer was allocated to represent one
 *       of the outputs of the engine.  If the host wants to read the data back
 *       then it will have to be retrieved from the device.  If the next
 *       execution changes the engine, then the data will have to be read back.
 *
 *     on_device=true, input_handle not empty, output_handle not empty :
 *       During the last execution, the buffer was an argument to the execution
 *       and was also one of the output parameters.  This typically indicates
 *       that it is a variable (weights/biases) that has been updated in place.
 *       If the next execution doesn't change the engine, and the data is not
 *       read back to the host in between executions, and the data remains as
 *       an argument to the same input number, then the data does not need to be
 *       copied back to the host.  This is the ideal situation when executing an
 *       engine repeatedly with the same set of weights/biases.
 *
 */
namespace se = ::stream_executor;

namespace xla {
namespace poplarplugin {

std::string GetRandomNumberSeedStream() { return "__seed_stream"; }

std::string GetInputCopyHandle(int64 parameter, int64 index) {
  return tensorflow::strings::Printf("%lld.%lld", parameter, index);
}

std::string GetOutputCopyHandle(int64 output_index, int64 flat_tensor_index) {
  return tensorflow::strings::Printf("out_%lld.%lld", output_index,
                                     flat_tensor_index);
}

std::string GetInfeedCopyHandle(const std::string& name, int64 shape_index) {
  return tensorflow::strings::Printf("infeed_%s.%lld", name.c_str(),
                                     shape_index);
}

std::string GetOutfeedCopyHandle(const std::string& name, int64 shape_index) {
  return tensorflow::strings::Printf("outfeed_%s.%lld", name.c_str(),
                                     shape_index);
}

se::host::HostStream* PoplarExecutor::AsPoplarStream(se::Stream* stream) {
  DCHECK(stream != nullptr);
  return dynamic_cast<se::host::HostStream*>(stream->implementation());
}

PoplarXfeedManager* GetXfeedManager(int device_ordinal) {
  static auto* managers = new absl::flat_hash_map<int, PoplarXfeedManager*>();
  static absl::Mutex* mutex = new absl::Mutex();

  absl::MutexLock lock(mutex);
  auto it = managers->find(device_ordinal);
  if (it == managers->end()) {
    it = managers->emplace(device_ordinal, new PoplarXfeedManager()).first;
  }
  return it->second;
}

void ResetXfeedManager(int device_ordinal) {
  auto* xfeed_manager = GetXfeedManager(device_ordinal);
  xfeed_manager->Reset();
}

namespace {
Status CreateDirIfMissing(const std::string& path) {
  CHECK(!path.empty());
  auto* env = tensorflow::Env::Default();

  // Two threads could race to observe the absence of the directory and
  // simultaneously try to create it, causing the "losing" thread to get a
  // "directory already exists" error.  We can work around this by checking
  // again whether the dir exists.
  if (!env->IsDirectory(path).ok()) {
    const auto status = env->RecursivelyCreateDir(path);
    if (!status.ok() && !env->IsDirectory(path).ok()) {
      return status;
    }
  }

  return Status::OK();
}

Shape GetOutfeedShape(const Shape& output_shape,
                      const uint32 replication_factor) {
  if (replication_factor > 1) {
    // When the graph is replicated, we expect an extra dimension at the front
    // of the output.
    std::vector<int64> dimensions = {replication_factor};
    absl::c_copy(output_shape.dimensions(), std::back_inserter(dimensions));
    return ShapeUtil::MakeShape(output_shape.element_type(), dimensions);
  } else {
    return output_shape;
  }
}

std::vector<Shape> GetOutfeedShapes(const std::vector<Shape>& output_shapes,
                                    const uint32 replication_factor) {
  std::vector<Shape> result(output_shapes.size());
  absl::c_transform(output_shapes, result.begin(), [&](const Shape& shape) {
    return GetOutfeedShape(shape, replication_factor);
  });
  return result;
}

int64 GetConfigHash(const IpuOptions& to_hash) {
  IpuOptions hashable_config = to_hash;

  // Remove elements which do not contribute to a difference in the
  // compiled executable.  We hash the device characteristics independently
  // so there is no need to do any device selection state.
  hashable_config.mutable_profiling()->set_enable_poplar_reports_text(false);
  hashable_config.mutable_profiling()->set_report_every_nth_execution(0);
  hashable_config.mutable_profiling()->set_enable_ipu_trace_events(false);
  hashable_config.mutable_profiling()->set_enable_poplar_reports_cbor(false);
  hashable_config.mutable_profiling()->set_report_directory(std::string());
  hashable_config.mutable_profiling()->set_max_report_size(0);
  hashable_config.mutable_device_config()->Clear();

  std::string config_proto_str;
  tensorflow::SerializeToStringDeterministic(hashable_config,
                                             &config_proto_str);
  return std::hash<string>()(config_proto_str);
}

int64 CombinedHash(const std::vector<int64>& components) {
  int64 hash = 42;
  for (int64 h : components) {
    hash = tensorflow::Hash64Combine(hash, h);
  }
  return hash;
}
}  // namespace

PoplarExecutor::TensorControl::TensorControl(size_t size_) {
  size = size_;
  ref_count = 1;
  on_device = false;
  input_handle.clear();
  output_handle.clear();
  output_convertor = nullptr;
  converted_data.clear();
  data = static_cast<char*>(tensorflow::port::AlignedMalloc(size_, 64));
}

PoplarExecutor::TensorControl::~TensorControl() {
  tensorflow::port::AlignedFree(data);
}

PoplarExecutor::InfeedDatasetIterator::InfeedDatasetIterator(
    int64 replication_factor,
    std::unique_ptr<tensorflow::FunctionLibraryDefinition> flib_def,
    std::unique_ptr<tensorflow::ProcessFunctionLibraryRuntime> process_flib,
    std::unique_ptr<tensorflow::data::FunctionHandleCache> handle_cache,
    std::unique_ptr<tensorflow::data::IteratorBase> iterator,
    std::unique_ptr<tensorflow::data::IteratorContext> iterator_ctx,
    const std::vector<xla::Shape>& shapes)
    : flib_def(std::move(flib_def)),
      process_flib(std::move(process_flib)),
      handle_cache(std::move(handle_cache)),
      iterator(std::move(iterator)),
      iterator_ctx(std::move(iterator_ctx)),
      shapes(std::move(shapes)),
      tensor_queues(shapes.size()) {
  // Function applied after we evict a buffer from the queue.
  auto post_apply = [](tensorflow::TensorBuffer*& buffer) {
    if (buffer) {
      buffer->Unref();
      buffer = nullptr;
    }
  };

  // Set up the queue per tensor per replica.
  replication_factor = std::max<int64>(replication_factor, 1);
  for (uint64 i = 0; i < shapes.size(); i++) {
    for (int64 replica_id = 0; replica_id < replication_factor; replica_id++) {
      void* ptr = tensorflow::port::AlignedMalloc(sizeof(InfeedQueueType), 64);
      tensor_queues[i].emplace_back(new (ptr)
                                        InfeedQueueType(nullptr, post_apply));
    }
  }
}

PoplarExecutor::OutfeedContext::OutfeedContext(const FeedInfo& outfeed_info)
    : config(outfeed_info.config),
      shapes(GetOutfeedShapes(FlattenedXlaShape(outfeed_info.shape),
                              outfeed_info.config.replication_factor())),
      tf_data_types(outfeed_info.config.tf_data_types().size()),
      tf_shapes(shapes.size()),
      callback_to_io_thread_queues(shapes.size()) {
  CHECK_EQ(shapes.size(), tf_data_types.size());
  int64 replication_factor = config.replication_factor();
  for (uint64 i = 0; i < shapes.size(); i++) {
    tf_data_types[i] = static_cast<tensorflow::DataType>(
        outfeed_info.config.tf_data_types()[i]);
    tensorflow::XLAShapeToTensorShape(shapes[i], &tf_shapes[i]);

    // Set up the queue per tensor per replica.
    int64 num_bytes_per_replica =
        ShapeUtil::ByteSizeOf(shapes[i]) / replication_factor;
    num_bytes_per_replica *= outfeed_info.config.io_batch_size();
    for (int64 replica_id = 0; replica_id < replication_factor; replica_id++) {
      void* ptr = tensorflow::port::AlignedMalloc(sizeof(OutfeedQueueType), 64);
      callback_to_io_thread_queues[i].emplace_back(
          new (ptr) OutfeedQueueType(num_bytes_per_replica));
    }
  }
}

PoplarExecutor::PoplarExecutor()
    : ordinal_(0),
      infeeds_done_(true),
      outfeeds_done_(true),
      current_engine_(nullptr),
      device_open_(false),
      poplar_device_hash_(0),
      hardware_configured_(false),
      infeed_thread_pool_(tensorflow::Env::Default(),
                          "poplar_infeed_thread_pool_",
                          PoplarExecutor::NUM_THREADS),
      outfeed_thread_pool_(tensorflow::Env::Default(),
                           "poplar_outfeed_thread_pool_",
                           PoplarExecutor::NUM_THREADS),
      has_cycle_counter_(false),
      rendezvous_(tensorflow::NewLocalRendezvous()) {
  // TODO should this use the time/ms?
  static std::random_device rd;
  seed_generator_.Seed(rd());
}

PoplarExecutor::~PoplarExecutor() {}

se::DeviceMemoryBase PoplarExecutor::Allocate(
  uint64 size, int64 memory_space) {
  TensorControl* allocated = new TensorControl(size);
  {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    allocations_.push_back(allocated);
  }
  return se::DeviceMemoryBase(allocated, size);
}

void* PoplarExecutor::GetSubBuffer(se::DeviceMemoryBase* parent,
                                   uint64 offset_bytes, uint64 size_bytes) {
  TensorControl* tc = reinterpret_cast<TensorControl*>(parent->opaque());
  return tc->data + offset_bytes;
}

void PoplarExecutor::Deallocate(se::DeviceMemoryBase* mem) {
  TensorControl* tc = reinterpret_cast<TensorControl*>(mem->opaque());
  {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    if (tc->ref_count > 0) {
      tc->ref_count--;
    }
  }
}

Status PoplarExecutor::ConnectSendCallbacksToRendezvous(
    const SendRecvInfos& send_infos) {
  for (const SendRecvInfo& send : send_infos) {
    VLOG(1) << "Connecting Poplar stream to rendezvous key '"
            << send.rendezvous_key << "' with shape " << send.shape;

    tensorflow::TensorShape shape;
    TF_RETURN_IF_ERROR(tensorflow::XLAShapeToTensorShape(send.shape, &shape));

    TF_ASSIGN_OR_RETURN(
        const tensorflow::DataType type,
        tensorflow::EncodePrimitiveTypeAsDataType(send.shape.element_type()));

    tensorflow::Rendezvous::ParsedKey key;
    TF_RETURN_IF_ERROR(
        tensorflow::Rendezvous::ParseKey(send.rendezvous_key, &key));

    // We allow capturing a raw pointer to the rendezvous in the lambda as
    // `this` which holds a refcount of it should outlive the engine.
    auto* rendezvous = GetRendezvous();

    // Accept the output from the first replica.
    current_engine_->connectStreamToCallback(
        send.stream_handle,
        /*replica_id=*/0,
        [rendezvous, key, tensor = tensorflow::Tensor(type, shape)](void* src) {
          auto* dst = tensorflow::DMAHelper::buffer(&tensor);

          // We reuse the same tensor every time to avoid allocating in this
          // callback. This should be safe since every Send op must be matched
          // by a corresponding Recv op in the same graph, so the tensor must
          // be consumed before the next execution of the graph. Verify this
          // assumption here by checking that we are the only owner.
          CHECK(dst->RefCountIsOne());
          std::memcpy(dst->data(), src, dst->size());

          // Sending here increases the refcount until it is consumed.
          rendezvous->Send(key, tensorflow::Rendezvous::Args{}, tensor,
                           /*is_dead=*/false);
        });

    // Discard the output from the remainding replicas.
    for (int replica_id = 1; replica_id < current_replication_factor_;
         ++replica_id) {
      current_engine_->connectStreamToCallback(send.stream_handle, replica_id,
                                               [](void*) {});
    }
  }

  return Status::OK();
}

Status PoplarExecutor::ConnectRecvCallbacksToRendezvous(
    const SendRecvInfos& recv_infos) {
  for (const SendRecvInfo& recv : recv_infos) {
    VLOG(1) << "Connecting Poplar stream to rendezvous key '"
            << recv.rendezvous_key << "' with shape " << recv.shape;

    // We allow capturing a raw pointer to the rendezvous in the lambda as
    // `this` which holds a refcount of it should outlive the engine.
    auto* rendezvous = GetRendezvous();

    tensorflow::Rendezvous::ParsedKey key;
    TF_RETURN_IF_ERROR(
        tensorflow::Rendezvous::ParseKey(recv.rendezvous_key, &key));

    // This stream has ReplicatedStreamMode::BROADCAST, so every replica
    // will receive the same data sent here.
    current_engine_->connectStreamToCallback(
        recv.stream_handle, [rendezvous, key](void* dst) {
          tensorflow::Tensor tensor;
          bool is_dead = false;
          rendezvous->Recv(key, tensorflow::Rendezvous::Args{}, &tensor,
                           &is_dead);
          CHECK(!is_dead);
          auto* src = tensorflow::DMAHelper::buffer(&tensor);
          std::memcpy(dst, src->data(), src->size());
        });
  }

  return Status::OK();
}

namespace {
class InfeedPrefetchCallback : public poplar::StreamCallback {
 public:
  InfeedPrefetchCallback(InfeedQueueType* queue, uint64 num_bytes)
      : queue_(queue), num_bytes_(num_bytes) {}

  poplar::StreamCallback::Result prefetch(void* dest) noexcept override {
    tensorflow::TensorBuffer* buffer;
    // Try to get a value from the queue.
    if (queue_->TryPop(buffer)) {
      std::memcpy(dest, buffer->data(), num_bytes_);
      return poplar::StreamCallback::Result::Success;
    } else {
      return poplar::StreamCallback::Result::NotAvailable;
    }
  }

  void fetch(void* dest) noexcept override {
    tensorflow::TensorBuffer* buffer;
    queue_->BlockPop(buffer);
    std::memcpy(dest, buffer->data(), num_bytes_);
  }

  void complete() noexcept override { queue_->AdvanceReadPosition(); }

 private:
  InfeedQueueType* queue_;
  const uint64 num_bytes_;
};

class NullPrefetchCallback : public poplar::StreamCallback {
 public:
  explicit NullPrefetchCallback(InfeedAllocator* allocator, uint64 num_bytes)
      : num_bytes_(num_bytes), allocator_(allocator) {
    for (auto& buffer : buffers_) {
      buffer = static_cast<uint8*>(allocator_->AllocateRaw(64, num_bytes));
    }
  }

  ~NullPrefetchCallback() {
    for (auto& buffer : buffers_) {
      allocator_->DeallocateRaw(buffer);
    }
  }

  poplar::StreamCallback::Result prefetch(void* dest) noexcept override {
    std::memcpy(dest, buffers_[index_], num_bytes_);
    return poplar::StreamCallback::Result::Success;
  }

  void fetch(void* dest) noexcept override {
    // This case shouldn't be hit, if poplar prefetches the data.
    std::memcpy(dest, buffers_[index_], num_bytes_);
  }

  void complete() noexcept override { index_ = (index_ + 1) % 16; }

 private:
  int index_ = 0;
  uint8* buffers_[16];
  const uint64 num_bytes_;
  InfeedAllocator* allocator_;
};
}  // namespace

void PoplarExecutor::ConnectInfeedsToStreamCallback(
    const InfeedInfos& infeed_infos) {
  // Don't connect any streams if using synthetic data
  if (UseSyntheticData()) {
    return;
  }

  for (const auto& infeed_info : infeed_infos) {
    auto itr = infeed_dataset_iterators_.find(infeed_info.config.feed_id());
    if (itr == infeed_dataset_iterators_.end()) {
      LOG(FATAL) << "Trying to access an infeed dataset iterator which has not "
                    "been created."
                 << " Did you initialize the infeed_queue?";
    }
    auto* infeed_dataset_iterator = itr->second.get();
    size_t tensor_count = infeed_dataset_iterator->shapes.size();
    for (size_t j = 0; j < tensor_count; ++j) {
      auto length = ShapeUtil::ByteSizeOf(infeed_dataset_iterator->shapes[j]);
      auto bytes_per_replica = length / current_replication_factor_;
      for (auto replica_id = 0; replica_id < current_replication_factor_;
           ++replica_id) {
        auto& queue = infeed_dataset_iterator->tensor_queues[j][replica_id];
        std::unique_ptr<poplar::StreamCallback> infeed_callback;
        if (PoplarXlaFlags::Get().null_data_feed) {
          infeed_callback = absl::make_unique<NullPrefetchCallback>(
              GetInfeedAllocator(), bytes_per_replica);
        } else {
          infeed_callback = absl::make_unique<InfeedPrefetchCallback>(
              queue.get(), bytes_per_replica);
        }
        current_engine_->connectStreamToCallback(
            GetInfeedCopyHandle(infeed_info.stream_prefix, j), replica_id,
            std::move(infeed_callback));
      }
    }
  }
}

void PoplarExecutor::ConnectOutfeedToStreamCallback(
    const OutfeedInfos& outfeed_infos) {
  // Don't connect any streams if using synthetic data
  if (UseSyntheticData()) {
    return;
  }

  for (const auto& outfeed_info : outfeed_infos) {
    const auto& outfeed_id = outfeed_info.config.feed_id();
    auto itr = outfeed_contexts_.find(outfeed_id);
    if (itr == outfeed_contexts_.end()) {
      LOG(FATAL) << "Outfeed with id='" << outfeed_id
                 << "' is not registered, but is required by the engine.";
    }

    auto* outfeed_context = itr->second.get();
    auto tensor_count = outfeed_context->shapes.size();
    for (unsigned j = 0; j < tensor_count; ++j) {
      size_t length = ShapeUtil::ByteSizeOf(outfeed_context->shapes[j]);
      auto bytes_per_replica = length / current_replication_factor_;
      bytes_per_replica *= outfeed_info.config.io_batch_size();
      for (auto replica_id = 0; replica_id < current_replication_factor_;
           ++replica_id) {
        auto& queue =
            outfeed_context->callback_to_io_thread_queues[j][replica_id];
        current_engine_->connectStreamToCallback(
            GetOutfeedCopyHandle(outfeed_info.stream_prefix, j), replica_id,
            [&queue, bytes_per_replica](void* src) {
              // The outfeed callback gets the buffer at the back of the queue,
              // writes to it, and then moves the write position of the queue.
              void* dest = queue->BlockBack();
              std::memcpy(dest, src, bytes_per_replica);
              queue->FinishedBack();
            });
      }
    }
  }
}

std::function<void()> PoplarExecutor::CreateInfeedIOThreadFunction(
    const InfeedInfos& infeed_infos) {
  infeed_thread_cancelled_ = false;
  // Check that the infeeds are done from the previous execution.
  CHECK(infeeds_done_.exchange(false));

  std::vector<InfeedDatasetIterator*> infeed_dataset_iterators;
  infeed_dataset_iterators.reserve(infeed_infos.size());
  for (const auto& infeed_info : infeed_infos) {
    auto itr = infeed_dataset_iterators_.find(infeed_info.config.feed_id());
    if (itr == infeed_dataset_iterators_.end()) {
      LOG(FATAL)
          << "Trying to access an infeed context which has not been created."
          << " Did you initialize the infeed_queue?";
    }
    infeed_dataset_iterators.push_back(itr->second.get());
  }

  return [this, infeed_dataset_iterators]() {
    while (!infeed_thread_cancelled_) {
      for (auto& infeed_dataset_iterator : infeed_dataset_iterators) {
        // We do not call GetNext if queues are full.
        // We make an assumption that all tensors from each queue for each
        // replica for an infeed are dequeued every iteration - we therefore
        // only need to check if the first queue is full to know whether all the
        // queues are full.
        if (infeed_dataset_iterator->tensor_queues[0][0]->IsFull()) {
          VLOG(1) << "Infeed queue is full.";
          continue;
        }

        const bool was_empty =
            infeed_dataset_iterator->tensor_queues[0][0]->IsEmpty();

        bool end_of_sequence = false;
        std::vector<tensorflow::Tensor> outputs;
        auto status = infeed_dataset_iterator->iterator->GetNext(
            infeed_dataset_iterator->iterator_ctx.get(), &outputs,
            &end_of_sequence);

        if (!status.ok()) {
          infeed_thread_cancelled_ = true;
          continue;
        }

        if (!end_of_sequence) {
          for (size_t j = 0; j < outputs.size(); ++j) {
            auto& tensor = outputs[j];
            std::vector<tensorflow::Tensor> tensor_slices;
            if (current_replication_factor_ > 1) {
              // For replicated graphs, slice the input tensor and enqueue
              // it separately for each replica.
              CHECK_EQ(tensor.dim_size(0), current_replication_factor_);
              tensor_slices.reserve(current_replication_factor_);
              for (auto replica_id = 0;
                   replica_id < current_replication_factor_; ++replica_id) {
                // Note that the tensor_slice shares the date buffer with the
                // tensor which works with ref counting.
                tensor_slices.push_back(tensor.SubSlice(replica_id));
              }
            } else {
              tensor_slices = {tensor};
            }

            // Enqueue tensors to each replica.
            for (size_t replica_id = 0; replica_id < tensor_slices.size();
                 replica_id++) {
              auto& queue =
                  infeed_dataset_iterator->tensor_queues[j][replica_id];
              auto* tb =
                  tensorflow::DMAHelper::buffer(&tensor_slices[replica_id]);
              tb->Ref();
              queue->BlockPush(tb);
              queue->AdvanceWritePosition();
            }
          }

          if (was_empty) {
            VLOG(1) << "Infeed queue is empty.";
          }
        } else {
          infeed_thread_cancelled_ = true;
          LOG(INFO)
              << "The dataset iterator has reached the end of the dataset.";
        }
      }
    }
    // Notify the main thread that infeeds are done.
    {
      std::lock_guard<std::mutex> l(infeeds_mutex_);
      infeeds_done_ = true;
    }
    infeeds_cond_var_.notify_one();
  };
}

namespace {
inline void AllocateTensors(std::deque<std::vector<tensorflow::Tensor>>& queue,
                            const std::vector<tensorflow::DataType>& types,
                            const std::vector<tensorflow::TensorShape>& shapes,
                            int count) {
  for (int c = 0; c < count; c++) {
    queue.emplace_front(types.size());
    auto& tensors = queue.front();
    for (size_t i = 0; i != types.size(); ++i) {
      tensors[i] = tensorflow::Tensor(types[i], shapes[i]);
    }
  }
}
}  // namespace

std::function<void()> PoplarExecutor::CreateOutfeedIOThreadFunction(
    const OutfeedInfos& outfeed_infos) {
  outfeed_thread_cancelled_ = false;
  // Check that the outfeeds are done from the previous execution.
  CHECK(outfeeds_done_.exchange(false));

  std::vector<OutfeedContext*> outfeed_contexts;
  outfeed_contexts.reserve(outfeed_infos.size());
  for (const auto& outfeed_info : outfeed_infos) {
    auto itr = outfeed_contexts_.find(outfeed_info.config.feed_id());
    if (itr == outfeed_contexts_.end()) {
      LOG(FATAL)
          << "Trying to access an outfeed context which has not been created.";
    }
    outfeed_contexts.push_back(itr->second.get());
  }

  return [this, outfeed_infos, outfeed_contexts]() {
    int replicas = current_replication_factor_;
    replicas = std::max(replicas, 1);

    // Lock all the outfeed queues which are of the GetLast type so that the CPU
    // OP does not try to dequeue the outfeed during the execution.
    for (auto& outfeed_context : outfeed_contexts) {
      if (outfeed_context->config.mode() == PoplarFeedConfig::GetLast) {
        outfeed_context->mutex.lock();
      }
    }

    // Continue while the thread has not been cancelled, and if it has been
    // cancelled allow for up to two extra runs.
    uint32 all_queues_empty_for = 0;
    while (!outfeed_thread_cancelled_ || all_queues_empty_for != 2) {
      bool all_queues_empty = true;
      for (auto& outfeed_context : outfeed_contexts) {
        int io_batch_size = outfeed_context->config.io_batch_size();
        for (auto& tensor_queues :
             outfeed_context->callback_to_io_thread_queues) {
          for (auto& replica_queue : tensor_queues) {
            all_queues_empty &= !replica_queue->HasItemsWaiting();
          }
        }

        // Track empty queues when we are trying to exit
        if (all_queues_empty && outfeed_thread_cancelled_) {
          all_queues_empty_for++;
        }

        // Continue if all the outfeed queues are empty.
        if (all_queues_empty) {
          continue;
        }

        // Lock the outfeed queue so that the CPU OP does not try to dequeue
        // whilst moving data off the device.
        {
          std::lock_guard<std::recursive_mutex> guard(outfeed_context->mutex);
          // Allocate the tensors before dequeuing.
          bool allocate_tensors = true;
          if (outfeed_context->config.mode() == PoplarFeedConfig::GetLast) {
            // For the get last we only allocate tensors once.
            allocate_tensors = outfeed_context->io_thread_output_queues.empty();
          }

          if (allocate_tensors) {
            AllocateTensors(outfeed_context->io_thread_output_queues,
                            outfeed_context->tf_data_types,
                            outfeed_context->tf_shapes, io_batch_size);
          }

          // We need to copy along 3 axis.  There are multiple queues from
          // the IPU, one  per tuple and per replica.  In each queue there
          // is a block of data containing one or more tensors.  There is a
          // single queue out of the executor, consisting of a vector of
          // Tensors, one per tuple entry.  If there are multiple replicas
          // then the outer dimension of the Tensors has the same value as the
          // replica count, and the output from each replica is concatenated
          // into that Tensor.
          //
          // We loop over each queue (by tuple  and replica), and dequeue the
          // block of data. This is then inserted  into the output queue as
          // appropriate.
          for (size_t tuple_idx = 0; tuple_idx < outfeed_context->shapes.size();
               ++tuple_idx) {
            // Dequeue tensors from each replica.
            for (int64 replica_id = 0; replica_id < replicas; replica_id++) {
              auto& queue =
                  outfeed_context
                      ->callback_to_io_thread_queues[tuple_idx][replica_id];

              // Dequeue the data and insert into the correct output queue.
              uint8_t* src = reinterpret_cast<uint8_t*>(queue->BlockFront());
              for (int b = 0; b < io_batch_size; b++) {
                std::vector<tensorflow::Tensor>& tensors_to_write_to =
                    outfeed_context->io_thread_output_queues.at(io_batch_size -
                                                                b - 1);

                auto& tensor = tensors_to_write_to[tuple_idx];

                // When there are mutiple replicas, insert the data into a slice
                // out of dinension 0.  Otherwise just use the whole tensor.
                auto output_tensor =
                    (replicas == 1 ? tensor : tensor.SubSlice(replica_id));
                auto* tb = tensorflow::DMAHelper::buffer(&output_tensor);

                std::memcpy(tb->data(), src, output_tensor.AllocatedBytes());
                src += output_tensor.AllocatedBytes();
              }
              queue->FinishedFront();
            }
          }
        }
      }
    }

    // Notify the main thread that outfeeds are done.
    {
      std::lock_guard<std::mutex> l(outfeeds_mutex_);
      outfeeds_done_ = true;
    }
    outfeeds_cond_var_.notify_one();

    // Unlock all the outfeed queues which are of the GetLast type.
    for (auto& outfeed_context : outfeed_contexts) {
      if (outfeed_context->config.mode() == PoplarFeedConfig::GetLast) {
        outfeed_context->mutex.unlock();
      }
    }
  };
}

void PoplarExecutor::LaunchIOThreads(const InfeedInfos& infeed_infos,
                                     const OutfeedInfos& outfeed_infos) {
  if (infeed_infos.size()) {
    std::function<void()> infeed_thread_io_fn =
        CreateInfeedIOThreadFunction(infeed_infos);
    infeed_thread_pool_.Schedule(infeed_thread_io_fn);
  }

  if (outfeed_infos.size()) {
    std::function<void()> outfeed_thread_io_fn =
        CreateOutfeedIOThreadFunction(outfeed_infos);
    outfeed_thread_pool_.Schedule(outfeed_thread_io_fn);
  }
}

void PoplarExecutor::StopIOThreads(const InfeedInfos& infeed_infos,
                                   const OutfeedInfos& outfeed_infos) {
  infeed_thread_cancelled_ = true;
  outfeed_thread_cancelled_ = true;

  if (infeed_infos.size()) {
    // Block until the infeed thread has finished.
    std::unique_lock<std::mutex> l(infeeds_mutex_);
    infeeds_cond_var_.wait(l,
                           [this] { return std::atomic_load(&infeeds_done_); });
  }

  if (outfeed_infos.size()) {
    // Block until the outfeed thread has finished.
    std::unique_lock<std::mutex> l(outfeeds_mutex_);
    outfeeds_cond_var_.wait(
        l, [this] { return std::atomic_load(&outfeeds_done_); });
  }
}

void PoplarExecutor::DeferredDeallocation() {
  std::lock_guard<std::recursive_mutex> g(mutex_);

  const auto new_end =
      std::partition(allocations_.begin(), allocations_.end(),
                     [](TensorControl* tc) { return tc->ref_count > 0; });

  std::for_each(new_end, allocations_.end(),
                [](TensorControl* tc) { delete tc; });

  allocations_.erase(new_end, allocations_.end());
}

bool PoplarExecutor::Memcpy(se::Stream* stream, void* host_dst,
                            const se::DeviceMemoryBase& pop_src, uint64 size) {
  AsPoplarStream(stream)->EnqueueTask([this, host_dst, pop_src, size]() {
    Status ok = SynchronousMemcpy(host_dst, pop_src, size);
  });
  return true;
}

bool PoplarExecutor::Memcpy(se::Stream* stream, se::DeviceMemoryBase* pop_dst,
                            const void* host_src, uint64 size) {
  se::DeviceMemoryBase dst = *pop_dst;
  AsPoplarStream(stream)->EnqueueTask([this, dst, host_src, size]() mutable {
    Status ok = SynchronousMemcpy(&dst, host_src, size);
  });
  return true;
}

Status PoplarExecutor::SynchronousMemcpy(se::DeviceMemoryBase* pop_dst,
                                         const void* host_src, uint64 size) {
  TensorControl* tc = reinterpret_cast<TensorControl*>(pop_dst->opaque());
  memcpy(tc->data, host_src, size);
  {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    tc->on_device = false;
    tc->input_handle.clear();
  }
  return Status::OK();
}

Status PoplarExecutor::SynchronousMemcpy(void* host_dst,
                                         const se::DeviceMemoryBase& pop_src,
                                         uint64 size) {
  const TensorControl* tc =
      reinterpret_cast<const TensorControl*>(pop_src.opaque());
  {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    if (tc->on_device == true && !tc->output_handle.empty()) {
      TF_RETURN_IF_ERROR(MoveDeviceToHost());
    }
  }
  memcpy(host_dst, tc->data, size);
  return Status::OK();
}

Status PoplarExecutor::SynchronousMemcpyDeviceToDevice(
    se::DeviceMemoryBase* dst, const se::DeviceMemoryBase& src, uint64 size) {
  TensorControl* dst_tc = reinterpret_cast<TensorControl*>(dst->opaque());
  const TensorControl* src_tc =
      reinterpret_cast<const TensorControl*>(src.opaque());
  {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    if (src_tc->on_device == true && !src_tc->output_handle.empty()) {
      TF_RETURN_IF_ERROR(MoveDeviceToHost());
    }
  }
  memcpy(dst_tc->data, src_tc->data, size);
  {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    dst_tc->on_device = false;
    dst_tc->input_handle.clear();
  }
  return Status::OK();
}

bool PoplarExecutor::MemcpyDeviceToDevice(se::Stream* stream,
                                          se::DeviceMemoryBase* pop_dst,
                                          const se::DeviceMemoryBase& pop_src,
                                          uint64 size) {
  se::DeviceMemoryBase dst = *pop_dst;
  AsPoplarStream(stream)->EnqueueTask([this, dst, pop_src, size]() mutable {
    SynchronousMemcpyDeviceToDevice(&dst, pop_src, size);
  });
  return true;
}

bool PoplarExecutor::HostCallback(se::Stream* stream,
                                  std::function<void()> callback) {
  AsPoplarStream(stream)->EnqueueTask(callback);
  return true;
}

bool PoplarExecutor::HostCallback(se::Stream* stream,
                                  std::function<Status()> callback) {
  AsPoplarStream(stream)->EnqueueTask(callback);
  return true;
}

bool PoplarExecutor::CreateStreamDependency(se::Stream* dependent,
                                            se::Stream* other) {
  AsPoplarStream(dependent)->EnqueueTask(
      [other]() { auto ok = other->BlockHostUntilDone(); });
  AsPoplarStream(dependent)->BlockUntilDone();
  return true;
}

bool PoplarExecutor::StartTimer(se::Stream* stream, se::Timer* timer) {
  dynamic_cast<se::host::HostTimer*>(timer->implementation())->Start(stream);
  return true;
}

bool PoplarExecutor::StopTimer(se::Stream* stream, se::Timer* timer) {
  dynamic_cast<se::host::HostTimer*>(timer->implementation())->Stop(stream);
  return true;
}

Status PoplarExecutor::BlockHostUntilDone(se::Stream* stream) {
  AsPoplarStream(stream)->BlockUntilDone();
  std::lock_guard<std::recursive_mutex> g(mutex_);
  return Status::OK();
}

bool PoplarExecutor::SynchronizeAllActivity() {
  std::lock_guard<std::recursive_mutex> g(mutex_);
  return true;
}

StatusOr<std::unique_ptr<se::DeviceDescription>>
PoplarExecutor::CreateDeviceDescription() const {
  auto platform =
      se::MultiPlatformManager::PlatformWithName(tensorflow::PLATFORM_NAME);
  if (platform.ok()) {
    auto* p = static_cast<PoplarPlatform*>(platform.ValueOrDie());
    return p->DescriptionForDevice(0);
  }
  return InternalError("Failed to create device description.");
}

std::string PoplarExecutor::GetDeviceTargetName() const {
  return poplar::toString(poplar_device_.getTarget().getTargetType());
}

static bool DeviceConfigurationsEqual(const IpuOptions& a,
                                      const IpuOptions& b) {
  return google::protobuf::util::MessageDifferencer::Equivalent(a, b);
}

bool PoplarExecutor::HasPoplarDevice() {
  const bool force_ipu_model = PoplarXlaFlags::Get().use_ipu_model;
  // If the device has not been configured via configure_ipu_system, but we have
  // requested an IPU model, then we create a CPU device.
  std::lock_guard<std::recursive_mutex> g(mutex_);
  if (!device_open_ && force_ipu_model) {
    // Poplar CPU device
    poplar_device_ = poplar::Device::createCPUDevice();
    if (poplar_device_.attach()) {
      device_open_ = true;
    }
  }
  return device_open_;
}

Status PoplarExecutor::ConfigurePoplarDevice(const IpuOptions& cfg) {
  if (!DeviceConfigurationsEqual(cfg, current_config_) &&
      hardware_configured_) {
    XLA_VLOG_LINES(1, "Current config: " + current_config_.DebugString() +
                          "\nNew config: " + cfg.DebugString());
    return InternalError("IPU system configuration can only be set once.");
  }
  try {
    if (device_open_) {
      if (DeviceConfigurationsEqual(current_config_, IpuOptions())) {
        // If there is no config associated to the open device then it is a CPU
        // device: dettach from it and initialize a Poplar device instead.
        VLOG(1) << "Detaching from " << GetDeviceTargetName() << " ordinal "
                << ordinal_;
        poplar_device_.detach();
        device_open_ = false;
      } else {
        VLOG(1) << "Poplar device: type " << GetDeviceTargetName()
                << " ordinal " << ordinal_
                << " is already configured: staying attached to it.";
      }
    }
    current_config_ = cfg;
    if (!device_open_) {
      bool opened = false;

      bool have_ipu_hardware = false;

      if (current_config_.device_config_size() > 0) {
        hardware_configured_ = true;
      }

      const bool force_ipu_model = PoplarXlaFlags::Get().use_ipu_model;

      if (!force_ipu_model) {
        auto device_list = GetDeviceManager().getDevices();
        for (const auto& d : device_list) {
          if (d.getTarget().getTargetType() == poplar::TargetType::IPU) {
            have_ipu_hardware = true;
            break;
          }
        }
      }

      if (have_ipu_hardware) {
        // Hardware devices
        auto device_list = GetDeviceManager().getDevices();

        if (current_config_.device_config_size() == 0) {
          // Default case - 1 single TF device with one single IPU
          for (auto& d : device_list) {
            if (d.getTarget().getTargetType() == poplar::TargetType::IPU &&
                d.getTarget().getNumIPUs() == 1) {
              if (d.attach()) {
                poplar_device_ = std::move(d);
                opened = true;
                break;
              }
            }
          }
        } else {
          // User has specified a configuration
          if (ordinal_ >= current_config_.device_config_size()) {
            return InternalError(
                "Device ordinal %d not in device configuration list.",
                ordinal_);
          }

          auto device = current_config_.device_config(ordinal_);

          if (device.selection_case() ==
              IpuOptions::DeviceConfig::SelectionCase::kCfgIndex) {
            const int32 cfg_index = device.cfg_index();

            poplar_device_ = std::move(device_list.at(cfg_index));
            if (poplar_device_.attach()) {
              opened = true;
            } else {
              return InternalError(
                  "Could not attach to requested device configuration index %d",
                  cfg_index);
            }
          } else {
            for (auto& d : device_list) {
              if (d.getTarget().getTargetType() == poplar::TargetType::IPU &&
                  static_cast<int32>(d.getTarget().getNumIPUs()) ==
                      device.auto_count()) {
                if (d.attach()) {
                  poplar_device_ = std::move(d);
                  opened = true;
                  break;
                }
              }
            }
          }
        }

        if (opened) {
          unsigned mj, mn, pt;
          poplar_device_.getDriverVersion(mj, mn, pt);
          VLOG(1) << "Poplar driver: " << mj << "." << mn << "." << pt;

          const auto& ids = poplar_device_.getDriverIDs();
          LOG(INFO) << "Device /device:IPU:" << ordinal_ << " attached to IPU"
                    << (ids.size() > 1 ? "s" : "") << ": "
                    << absl::StrJoin(ids, ",");
        }
      } else if (force_ipu_model) {
        if (current_config_.ipu_model_config().enable_ipu_model()) {
          // Poplar IPU Model device

          int num_ipus = 1;
          if (current_config_.device_config_size() > 0) {
            auto device = current_config_.device_config(ordinal_);

            if (device.selection_case() ==
                IpuOptions::DeviceConfig::SelectionCase::kCfgIndex) {
              return InvalidArgument(
                  "Must specify the number of IPUs using auto_count");
            }

            num_ipus = device.auto_count();
          }

          poplar::IPUModel model;
          model.numIPUs = num_ipus;

          model.compileIPUCode =
              current_config_.ipu_model_config().compile_ipu_code();
          poplar_device_ = model.createDevice();
          if (poplar_device_.attach()) {
            opened = true;
          }
        }
      }

      if (!opened) {
        return xla::ResourceExhausted(
            "Unable to acquire poplar device type for ordinal %d", ordinal_);
      }
      VLOG(1) << "Opened Poplar device type " << GetDeviceTargetName();
      device_open_ = true;
    }
  } catch (poplar::poplar_error e) {
    return xla::InternalError("Unable to open poplar device for ordinal %d: %s",
                              ordinal_, e.what());
  }
  option_flags_ = poplar::OptionFlags();
  option_flags_.set("target.workerStackSizeInBytes", "0x200");

  if (!current_config_.ipu_model_config().enable_ipu_model() &&
      current_config_.profiling().enable_execution_trace()) {
    // Enable getting the cycle counts for each compute set on hardware
    // when asking for an execution trace
    option_flags_.set("debug.instrument", "true");
  }

  // By setting stream options before user options we make sure the user can
  // override this default behaviour.
  if (current_config_.prefetch_data_streams()) {
    // By default we only rearrange copies on the host for resource variable
    // inputs which do not need to be prefetched, however if we rearrange
    // everything on the host, we do not overlap any stream buffers.
    option_flags_.set(
        "exchange.streamBufferOverlap",
        AlwaysRearrangeCopiesOnTheHost() ? "none" : "hostRearrangeOnly");
    option_flags_.set("exchange.enablePrefetch", "true");
  }

  for (const auto& opt : current_config_.compilation_options()) {
    option_flags_.set(opt.option(), opt.value());
  }

  for (const auto& opt : current_config_.convolution_options()) {
    conv_options_.set(opt.option(), opt.value());
  }

  for (const auto& opt : current_config_.matmul_options()) {
    matmul_options_.set(opt.option(), opt.value());
  }

  for (const auto& opt : current_config_.pooling_options()) {
    pooling_options_.set(opt.option(), opt.value());
  }

  for (const auto& opt : current_config_.profiling().options()) {
    report_options_.set(opt.option(), opt.value());
  }

  const auto max_compilation_threads =
      PoplarXlaFlags::Get().max_compilation_threads;
  if (max_compilation_threads > 0) {
    option_flags_.set("opt.maxCompilationThreads",
                      std::to_string(max_compilation_threads));
  }

  if (!PoplarXlaFlags::Get().save_oom_profiler.empty()) {
    option_flags_.set("debug.allowOutOfMemory", "true");
  }

  for (auto opt : option_flags_) {
    VLOG(1) << "Engine option: " << opt.first << " = " << opt.second;
  }

  for (auto opt : conv_options_) {
    VLOG(1) << "Convolution option: " << opt.first << " = " << opt.second;
  }

  for (auto opt : matmul_options_) {
    VLOG(1) << "MatMul option: " << opt.first << " = " << opt.second;
  }

  for (auto opt : pooling_options_) {
    VLOG(1) << "Pooling option: " << opt.first << " = " << opt.second;
  }

  for (auto opt : report_options_) {
    VLOG(1) << "Report option: " << opt.first << " = " << opt.second;
  }

  // Generate Target hash
  std::vector<int64> poplar_target;
  const auto& target = poplar_device_.getTarget();
  poplar_target.push_back(target.getNumTiles());
  poplar_target.push_back(target.getDataPathWidth());
  poplar_target.push_back(target.getBytesPerTile());
  poplar_target.push_back(target.getNumWorkerContexts());
  poplar_target.push_back(target.getTilesPerIPU());
  poplar_target.push_back(target.getNumIPUs());
  poplar_target.push_back((unsigned)target.getTargetType());

  // Generate Options hash
  poplar_target.push_back(GetConfigHash(current_config_));

  // Generate compiler hashes
  //poplar_target.push_back(std::hash<string>()(tf_git_version()));
  poplar_target.push_back(std::hash<string>()(poplar::packageHash()));

  // Get environment PoplarXlaFlags hash
  poplar_target.push_back(PoplarXlaFlags::Get().hlo_hash);

  poplar_device_hash_ = CombinedHash(poplar_target);

  return Status::OK();
}

bool PoplarExecutor::HaveExecutableCache() const {
  return !PoplarXlaFlags::Get().executable_cache_path.empty();
}

Status PoplarExecutor::CreateExecutableCacheDirIfMissing() const {
  return CreateDirIfMissing(PoplarXlaFlags::Get().executable_cache_path);
}

std::string PoplarExecutor::SerializedExecutableFilename(
    const HloModule& module) const {
  uint64 hash = HashModuleAndDevice(module);

  std::string filename = tensorflow::strings::Printf("%0llx.ipu_bin", hash);

  return tensorflow::io::JoinPath(SerializationFolder(), filename);
}

Status PoplarExecutor::CreateSerializedExecutableDirIfMissing() const {
  return CreateDirIfMissing(SerializationFolder());
}

uint64 PoplarExecutor::HashModuleAndDevice(const HloModule& module) const {
  HloHash module_hash(&module);
  uint64 hash = module_hash.GetHash();
  return tensorflow::Hash64Combine(hash, poplar_device_hash_);
}

std::string PoplarExecutor::CachedExecutableFilename(
    const HloModule& module) const {
  uint64 hash = HashModuleAndDevice(module);

  std::string filename = tensorflow::strings::Printf("%0llx.xla_engine", hash);

  return tensorflow::io::JoinPath(PoplarXlaFlags::Get().executable_cache_path,
                                  filename);
}

bool PoplarExecutor::HaveCachedExecutable(const std::string& filename) const {
  return tensorflow::Env::Default()->FileExists(filename).ok();
}

tensorflow::IpuTraceEvent PoplarExecutor::NewTraceEvent() {
  uint64 now = tensorflow::Env::Default()->NowMicros();
  tensorflow::IpuTraceEvent evt;
  evt.set_timestamp(static_cast<double>(now) / 1000000.0);
  evt.set_ordinal(ordinal_);
  return evt;
}

void PoplarExecutor::AddCompileBeginEventRecord(
    const std::string& module_name) {
  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::COMPILE_BEGIN);
  evt.mutable_compile_begin()->set_module_name(std::move(module_name));

  reports_.push_back(evt);
}

std::string PoplarExecutor::ReportFileExtension() const {
  std::string report_file_extension = "";
  if (CompilerReportingTextFormat()) {
    report_file_extension = "txt";
  } else if (CompilerReportingCborFormat()) {
    report_file_extension = "cbor";
  } else {
    report_file_extension = "json";
  }

  return report_file_extension;
}

void PoplarExecutor::AddCompileEndEventRecord(
    const std::string& module_name, const std::string& report,
    const std::string& tensor_map, const std::string& instruction_info,
    int64 duration) {
  std::string rep = std::move(report);
  std::string map = std::move(tensor_map);

  if (ReportDirectory().size() > 0) {
    std::unique_ptr<tensorflow::WritableFile> file;

    std::string report_file_extension = ReportFileExtension();

    std::string filename = tensorflow::io::JoinPath(
        ReportDirectory(),
        module_name + ".compile_report." + report_file_extension);

    TF_CHECK_OK(tensorflow::Env::Default()->NewWritableFile(filename, &file));
    TF_CHECK_OK(file->Append(rep));
    TF_CHECK_OK(file->Close());
    rep = filename;

    filename = tensorflow::io::JoinPath(
        ReportDirectory(),
        module_name + ".tensor_map." + report_file_extension);
    TF_CHECK_OK(tensorflow::Env::Default()->NewWritableFile(filename, &file));
    TF_CHECK_OK(file->Append(map));
    TF_CHECK_OK(file->Close());
    map = filename;
  }

  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::COMPILE_END);

  auto* compile_end = evt.mutable_compile_end();
  compile_end->set_module_name(std::move(module_name));
  compile_end->set_compilation_report(std::move(rep));
  compile_end->set_duration(duration);
  compile_end->set_tensor_map(std::move(map));
  compile_end->set_instruction_info(std::move(instruction_info));

  reports_.push_back(evt);
}

void PoplarExecutor::AddHostToDeviceEventRecord(const std::string& json) {
  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::HOST_TO_DEVICE_TRANSFER);
  evt.mutable_data_transfer()->set_data_transfer(std::move(json));

  reports_.push_back(evt);
}

void PoplarExecutor::AddDeviceToHostEventRecord(const std::string& json) {
  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::DEVICE_TO_HOST_TRANSFER);
  evt.mutable_data_transfer()->set_data_transfer(std::move(json));

  reports_.push_back(evt);
}

void PoplarExecutor::AddLoadEngineEventRecord(const std::string& module_name) {
  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::LOAD_ENGINE);
  evt.mutable_load_engine()->set_module_name(std::move(module_name));

  reports_.push_back(evt);
}

void PoplarExecutor::AddExecuteEventRecord(const std::string& module_name,
                                           const std::string& report) {
  std::string rep = std::move(report);
  if (ReportDirectory().size() > 0 && report.size()) {
    std::unique_ptr<tensorflow::WritableFile> file;

    std::string report_file_extension = ReportFileExtension();

    std::string filename = tensorflow::io::JoinPath(
        ReportDirectory(),
        module_name + ".execute_report." + report_file_extension);
    TF_CHECK_OK(tensorflow::Env::Default()->NewWritableFile(filename, &file));
    TF_CHECK_OK(file->Append(rep));
    TF_CHECK_OK(file->Close());
    rep = filename;
  }

  auto evt = NewTraceEvent();
  evt.set_type(tensorflow::IpuTraceEvent::EXECUTE);
  evt.mutable_execute()->set_module_name(std::move(module_name));
  evt.mutable_execute()->set_execution_report(std::move(rep));

  reports_.push_back(evt);
}

Status PoplarExecutor::GetCompilerEvents(
    std::list<tensorflow::IpuTraceEvent>& out) {
  std::lock_guard<std::recursive_mutex> g(mutex_);
  out.splice(out.end(), std::move(reports_));
  reports_.clear();
  return Status::OK();
}

void PoplarExecutor::FlattenedDeviceMemoryList(
    InputPairList& list, const xla::Shape& shape, void* base,
    const InputOutputAliasingMap::InputInfo& input_info) {
  TensorControl* tc = static_cast<TensorControl*>(base);
  if (shape.IsTuple()) {
    void** ptrs = reinterpret_cast<void**>(tc->data);
    for (unsigned int t = 0; t < xla::ShapeUtil::TupleElementCount(shape);
         t++) {
      void* ptr = ptrs[t];
      FlattenedDeviceMemoryList(list,
                                xla::ShapeUtil::GetTupleElementShape(shape, t),
                                ptr, input_info);
    }
  } else {
    list.push_back(InputDef(tc, GetInputConversionFunction(shape),
                            input_info.IsStreaming()));
  }
}

void PoplarExecutor::UpdateArgsHandleMap(
    const Args& args, se::DeviceMemoryAllocator* allocator,
    const xla::poplarplugin::PoplarExecutable& executable) {
  args_map_.clear();

  const auto* comp = executable.module().entry_computation();
  std::vector<xla::Shape> shapes(comp->num_parameters());
  for (const auto& inst : comp->parameter_instructions()) {
    shapes[inst->parameter_number()] = inst->shape();
  }

  const auto& inputs_info =
      executable.GetInputOutputAliasingMap().GetEntryInputInfos();
  CHECK_EQ(inputs_info.size(), args.size());
  CHECK_EQ(shapes.size(), args.size());

  // We require all the resource arguments which are modified to be not-aliasing
  // with each other.
  absl::flat_hash_set<const TensorControl*> modified_resources;

  for (unsigned int a = 0; a < inputs_info.size(); a++) {
    const auto& input_info = inputs_info[a];
    InputPairList bufs;
    FlattenedDeviceMemoryList(bufs, shapes[a],
                              const_cast<void*>(args[a].opaque()), input_info);
    for (unsigned i = 0; i < bufs.size(); i++) {
      InputDef input = bufs[i];
      auto input_handle = GetInputCopyHandle(a, i);
      if (input_info.IsResource() && !input_info.IsResourceNotModified()) {
        if (modified_resources.contains(input.tc)) {
          // We found an alias - we add a copy.
          VLOG(1) << "Found an alias for input handle " << input_handle
                  << ", duplicating the buffer.";
          se::DeviceMemoryBase allocated =
              allocator->Allocate(ordinal_, input.tc->size, false)
                  .ConsumeValueOrDie()
                  .Release();
          TensorControl* tc =
              reinterpret_cast<TensorControl*>(allocated.opaque());
          std::memcpy(tc->data, input.tc->data, input.tc->size);
          input = InputDef(tc, input.fn, input.streamed);
        }
        modified_resources.insert(input.tc);
      }

      args_map_[input_handle] = input;
    }
  }
}

void PoplarExecutor::FlattenedOutputDeviceMemoryList(
    OutputPairList& list, const xla::Shape& shape, void* base,
    const InputOutputAliasingMap::OutputInfo& output_info) {
  TensorControl* tc = static_cast<TensorControl*>(base);
  if (shape.IsTuple()) {
    void** ptrs = reinterpret_cast<void**>(tc->data);
    for (unsigned int t = 0; t < xla::ShapeUtil::TupleElementCount(shape);
         t++) {
      void* ptr = ptrs[t];
      FlattenedOutputDeviceMemoryList(
          list, xla::ShapeUtil::GetTupleElementShape(shape, t), ptr,
          output_info);
    }
  } else {
    list.push_back(OutputDef(tc, output_info.IsStreaming()));
  }
}

void PoplarExecutor::UpdateOutputsHandleMap(
    const xla::poplarplugin::PoplarExecutable& executable,
    const xla::Shape& shape, se::DeviceMemoryBase retbuf) {
  outputs_map_.clear();

  // Get all output pointers and their shapes
  std::vector<void*> outputs;
  std::vector<xla::Shape> shapes;

  if (shape.IsTuple()) {
    TensorControl* tc = static_cast<TensorControl*>(retbuf.opaque());
    void** ptrs = reinterpret_cast<void**>(tc->data);
    for (int64 i = 0; i < ShapeUtil::TupleElementCount(shape); i++) {
      shapes.push_back(ShapeUtil::GetTupleElementShape(shape, i));
      outputs.push_back(ptrs[i]);
    }
  } else {
    shapes.push_back(shape);
    outputs.push_back(retbuf.opaque());
  }

  // For all outputs
  const auto& outputs_info =
      executable.GetInputOutputAliasingMap().GetEntryOutputInfos();
  CHECK_EQ(outputs_info.size(), shapes.size());
  CHECK_EQ(outputs.size(), shapes.size());
  for (unsigned int a = 0; a < outputs_info.size(); a++) {
    const auto& output_info = outputs_info[a];
    OutputPairList bufs;
    FlattenedOutputDeviceMemoryList(bufs, shapes[a], outputs[a], output_info);
    for (unsigned i = 0; i < bufs.size(); i++) {
      outputs_map_[bufs[i].tc->output_handle] = bufs[i];
    }
  }
}

se::DeviceMemoryBase PoplarExecutor::ConstantOutputAllocation::GetAllocation(
    se::DeviceMemoryAllocator* allocator, const xla::Shape& shape,
    const int64 output_index, int64& flat_tensor_index, const Args&,
    const InputOutputAliasingMap::OutputInfo&, const ArgsHandleMap&,
    const int ordinal) const {
  const auto& constant = constants_[output_index][flat_tensor_index];
  const int64 size(xla::ShapeUtil::ByteSizeOf(shape));
  se::DeviceMemoryBase allocated =
      allocator->Allocate(ordinal, size, false).ConsumeValueOrDie().Release();
  TensorControl* tc = reinterpret_cast<TensorControl*>(allocated.opaque());
  tc->size = size;
  tc->on_device = false;
  tc->output_handle = std::string();
  tc->output_convertor = nullptr;

  void* buf(static_cast<void*>(tc->data));
  memcpy(buf, constant.untyped_data(), constant.size_bytes());
  return allocated;
}

se::DeviceMemoryBase PoplarExecutor::RemapOutputAllocation::GetAllocation(
    se::DeviceMemoryAllocator* allocator, const xla::Shape&,
    const int64 output_index, int64& flat_tensor_index, const Args& args,
    const InputOutputAliasingMap::OutputInfo&, const ArgsHandleMap& args_map,
    const int ordinal) const {
  const auto& remap_idx = remap_map_[output_index];
  auto it = args_map.find(GetInputCopyHandle(remap_idx, flat_tensor_index));
  if (it == args_map.end()) {
    LOG(FATAL) << "Could not remap an output to input tensor.";
  }

  bool make_a_copy = false;

  auto input_infos = input_output_aliasing_map_.GetEntryInputInfos();
  auto output_infos = input_output_aliasing_map_.GetEntryOutputInfos();
  if (input_infos.size() > 0 && output_infos.size() > 0) {
    int input_index = output_infos[output_index].GetInputIndex();
    bool is_input_resource = input_infos[input_index].IsResource();
    bool is_output_resource = output_infos[output_index].IsResource();
    make_a_copy = is_input_resource != is_output_resource;
  }

  if (make_a_copy) {
    TensorControl* orig = it->second.tc;
    se::DeviceMemoryBase allocated =
        allocator->Allocate(ordinal, orig->size, false)
            .ConsumeValueOrDie()
            .Release();
    TensorControl* tc = reinterpret_cast<TensorControl*>(allocated.opaque());
    if (orig->on_device) {
      auto status = executor_->MoveDeviceToHost();
      if (!status.ok()) {
        LOG(FATAL) << status.ToString();
      }
    }

    memcpy(tc->data, orig->data, orig->size);

    return se::DeviceMemoryBase(tc, tc->size);
  } else {
    // Return a reference
    TensorControl* tc = it->second.tc;
    tc->ref_count++;
    return se::DeviceMemoryBase(tc, tc->size);
  }
}

se::DeviceMemoryBase PoplarExecutor::BufferOutputAllocation::GetAllocation(
    se::DeviceMemoryAllocator* allocator, const xla::Shape& shape,
    const int64 output_index, int64& flat_tensor_index, const Args& args,
    const InputOutputAliasingMap::OutputInfo& output_info,
    const ArgsHandleMap& args_map, const int ordinal) const {
  int64 size(xla::ShapeUtil::ByteSizeOf(shape));
  if (output_info.IsResourceModified()) {
    // The output is an in-place update of one of the inputs
    // TODO: is this a multi-threading bug?
    auto it = args_map.find(
        GetInputCopyHandle(output_info.GetInputIndex(), flat_tensor_index));
    if (it == args_map.end()) {
      LOG(FATAL) << "Could not find matching input resource tensor.";
    }
    TensorControl* tc = it->second.tc;
    tc->size = size;
    tc->on_device = output_info.IsStreaming() ? false : true;
    tc->ref_count++;
    tc->output_handle = GetOutputCopyHandle(output_index, flat_tensor_index);
    tc->output_convertor = GetOutputConversionFunction(shape);
    return se::DeviceMemoryBase(tc);
  } else {
    // The output is not one of the inputs
    se::DeviceMemoryBase allocated =
        allocator->Allocate(ordinal, size, false).ConsumeValueOrDie().Release();
    TensorControl* tc = reinterpret_cast<TensorControl*>(allocated.opaque());
    tc->size = size;
    tc->on_device = output_info.IsStreaming() ? false : true;
    tc->output_handle = GetOutputCopyHandle(output_index, flat_tensor_index);
    tc->output_convertor = GetOutputConversionFunction(shape);
    return allocated;
  }
}

se::DeviceMemoryBase PoplarExecutor::HandleOutputBuffer(
    se::DeviceMemoryAllocator* allocator,
    const PoplarExecutor::OutputAllocation& allocation_info,
    const xla::Shape& shape, const int64 output_index, int64& flat_tensor_index,
    const Args& args, const InputOutputAliasingMap::OutputInfo& output_info) {
  if (!shape.IsTuple()) {
    se::DeviceMemoryBase buf = allocation_info.GetAllocation(
        allocator, shape, output_index, flat_tensor_index, args, output_info,
        args_map_, ordinal_);
    flat_tensor_index++;
    return buf;
  } else {
    int64 size(xla::ShapeUtil::ByteSizeOf(shape, sizeof(void*)));
    se::DeviceMemoryBase allocated = allocator->Allocate(ordinal_, size, false)
                                         .ConsumeValueOrDie()
                                         .Release();
    TensorControl* tc = reinterpret_cast<TensorControl*>(allocated.opaque());

    void** buf = reinterpret_cast<void**>(tc->data);
    for (int64 i = 0; i < xla::ShapeUtil::TupleElementCount(shape); i++) {
      se::DeviceMemoryBase out = HandleOutputBuffer(
          allocator, allocation_info, shape.tuple_shapes(i), output_index,
          flat_tensor_index, args, output_info);
      *buf++ = out.opaque();
    }
    return se::DeviceMemoryBase(tc, size);
  }
}

se::DeviceMemoryBase PoplarExecutor::GetOutputBuffer(
    const xla::poplarplugin::PoplarExecutable& executable,
    se::DeviceMemoryAllocator* allocator,
    const PoplarExecutor::OutputAllocation& allocation_info,
    const xla::Shape& shape, const Args& args,
    const InputOutputAliasingMap& input_output_aliasing_map) {
  // Get all output shapes
  std::vector<xla::Shape> shapes;
  const int64 size = shape.IsTuple()
                         ? xla::ShapeUtil::ByteSizeOf(shape, sizeof(void*))
                         : xla::ShapeUtil::ByteSizeOf(shape);

  if (shape.IsTuple()) {
    for (int64 i = 0; i < ShapeUtil::TupleElementCount(shape); i++) {
      shapes.push_back(ShapeUtil::GetTupleElementShape(shape, i));
    }
  } else {
    shapes.push_back(shape);
  }

  std::vector<void*> ptrs;
  // For all outputs
  // Call a recursive function HandleOutputBuffer for each output instruction
  const auto& outputs_info =
      executable.GetInputOutputAliasingMap().GetEntryOutputInfos();
  CHECK_EQ(outputs_info.size(), shapes.size());
  for (unsigned int idx = 0; idx < shapes.size(); idx++) {
    const auto& output_info = outputs_info[idx];
    int64 start_flat_tensor_index = 0;
    se::DeviceMemoryBase out =
        HandleOutputBuffer(allocator, allocation_info, shapes[idx], idx,
                           start_flat_tensor_index, args, output_info);
    ptrs.push_back(out.opaque());
  }
  if (shape.IsTuple()) {
    se::DeviceMemoryBase allocated = allocator->Allocate(ordinal_, size, false)
                                         .ConsumeValueOrDie()
                                         .Release();
    TensorControl* tc = reinterpret_cast<TensorControl*>(allocated.opaque());
    void** buf = reinterpret_cast<void**>(tc->data);
    for (void* ptr : ptrs) {
      *buf++ = ptr;
    }
    return se::DeviceMemoryBase(tc, size);
  } else {
    CHECK_EQ(ptrs.size(), 1);
    return se::DeviceMemoryBase(ptrs[0]);
  }
}

// Takes a tensor and returns a pointer to a buffer with the data in the right
// format
void* PoplarExecutor::PreProcessBuffer(InputDef& id) {
  TensorControl* tc = id.tc;
  void* buf(static_cast<void*>(tc->data));
  if (id.fn != nullptr) {
    tc->converted_data = id.fn(buf, tc->size, 0);
    buf = tc->converted_data.data();
  }
  return buf;
}

// Convers the data into the right host format
void PoplarExecutor::PostProcessBuffer(TensorControl* tc) {
  if (tc->output_convertor) {
    void* buf(static_cast<void*>(tc->data));
    std::vector<char> converted = tc->output_convertor(buf, 0, tc->size);
    memcpy(buf, converted.data(), converted.size());
  }
}

StatusOr<bool> PoplarExecutor::CheckMoveDeviceToHostRequired(
    const bool engine_changed) {
  // Pull previous execution outputs back from device if:
  // a) one is on the device _and_
  // b)   the engine is changing _or_
  // c)   output buffer isn't an input to the current engine _or_
  // d)   output buffer isn't currently in the right place for the new input
  bool do_device_to_host = false;
  for (const auto& tc : allocations_) {
    if (tc->on_device == true && !tc->output_handle.empty()) {
      if (engine_changed || args_map_.count(tc->input_handle) == 0 ||
          tc != args_map_.at(tc->input_handle).tc) {
        do_device_to_host = true;
      }
    }
  }
  return do_device_to_host;
}

StatusOr<bool> PoplarExecutor::CheckMoveHostToDeviceRequired(
    const bool engine_changed) {
  // Put resources on the device if:
  // a) the engine has changed
  // b) resource is not on the device
  // c) resource is on the device, but in the wrong place
  bool do_host_to_device = false;

  for (const auto& arg : args_map_) {
    if (!arg.second.streamed) {
      auto it =
          std::find(allocations_.begin(), allocations_.end(), arg.second.tc);
      if (it == allocations_.end()) {
        return tensorflow::errors::InvalidArgument(
            "Argument isn't allocated on device: ", (void*)arg.second.tc);
      }
      if (engine_changed || arg.second.tc->on_device == false ||
          arg.second.tc->input_handle != arg.first) {
        do_host_to_device = true;
      }
    }
  }
  return do_host_to_device;
}

void PoplarExecutor::ConnectReplicatedDeviceToHost(
    const std::string& stream_name, TensorControl* tc) {
  void* dest = static_cast<void*>(tc->data);
  const std::size_t size = tc->size;
  for (int64 replica_id = 0; replica_id < current_replication_factor_;
       ++replica_id) {
    auto callback = [dest, size, replica_id](void* ptr) {
      if (replica_id == 0) {
        std::memcpy(dest, ptr, size);
      }
    };

    current_engine_->connectStreamToCallback(stream_name, replica_id, callback);
  }
}

Status PoplarExecutor::MoveDeviceToHost() {
  if (UseSyntheticData()) {
    return Status::OK();
  }

  Json::Value root;
  root["tensors"] = Json::Value(Json::arrayValue);
  uint64 total_size = 0;
  uint64 total_count = 0;
  try {
    for (const auto& tc : allocations_) {
      // Set up streams
      if (tc->on_device == true && !tc->output_handle.empty()) {
        ConnectReplicatedDeviceToHost(tc->output_handle, tc);

        Json::Value tensor;
        tensor["name"] = Json::Value(tc->output_handle);
        tensor["size"] = Json::Value::UInt64(tc->size);
        root["tensors"].append(tensor);
        total_size += tc->size;
        total_count++;
      }
    }
    root["total_size"] = Json::Value::UInt64(total_size);
    Json::StreamWriterBuilder json_builder;
    std::string json_msg = Json::writeString(json_builder, root);

    // perform device -> host read
    if (total_count > 0) {
      current_engine_->disableExecutionProfiling();
      current_engine_->run(PoplarProgramType::DEVICE_TO_HOST);
    }

    if (current_config_.profiling().enable_ipu_trace_events() &&
        current_config_.profiling().enable_io_trace()) {
      AddDeviceToHostEventRecord(json_msg);
    }

    // Post process upload
    for (const auto& tc : allocations_) {
      if (tc->on_device == true && !tc->output_handle.empty()) {
        PostProcessBuffer(tc);
      }

      tc->on_device = false;
      tc->output_handle.clear();
      tc->input_handle.clear();
    }
  } catch (const std::exception& e) {
    return PoplarExceptionToTensorflowStatus("[Device to host] ", e);
  }
  return Status::OK();
}

Status PoplarExecutor::MoveHostToDevice() {
  if (UseSyntheticData()) {
    return Status::OK();
  }
  try {
    Json::Value root;
    root["tensors"] = Json::Value(Json::arrayValue);
    uint64 total_size = 0;

    for (auto arg : args_map_) {
      TensorControl* tc = arg.second.tc;
      std::vector<std::pair<std::string, int64>> stream_list;
      void* buf(static_cast<void*>(tc->data));
      if (!arg.second.streamed) {
        buf = PreProcessBuffer(arg.second);

        current_engine_->connectStream(arg.first, buf);

        tc->on_device = true;
        tc->input_handle = arg.first;

        Json::Value tensor;
        tensor["name"] = Json::Value(arg.first);
        tensor["size"] = Json::Value::UInt64(tc->size);
        root["tensors"].append(tensor);
        total_size += tc->size;

        stream_list.push_back(std::make_pair(arg.first, 0));
      }
    }
    root["total_size"] = Json::Value::UInt64(total_size);
    Json::StreamWriterBuilder json_builder;
    std::string json_msg = Json::writeString(json_builder, root);

    current_engine_->disableExecutionProfiling();
    current_engine_->run(PoplarProgramType::HOST_TO_DEVICE);

    if (current_config_.profiling().enable_ipu_trace_events() &&
        current_config_.profiling().enable_io_trace()) {
      AddHostToDeviceEventRecord(json_msg);
    }

    for (auto arg : args_map_) {
      TensorControl* tc = arg.second.tc;
      tc->converted_data.clear();
    }
  } catch (const std::exception& e) {
    return PoplarExceptionToTensorflowStatus("[Host to device] ", e);
  }

  return Status::OK();
}

StatusOr<se::DeviceMemoryBase> PoplarExecutor::GetTupleBufferByIndex(
    const se::DeviceMemoryBase& base, int64 value) {
  const TensorControl* tc =
      reinterpret_cast<const TensorControl*>(base.opaque());
  void** bufs = (void**)tc->data;
  int64 size = reinterpret_cast<const TensorControl*>(bufs[value])->size;

  return se::DeviceMemoryBase(bufs[value], size);
}

void PoplarExecutor::ConnectStreamedVariablesHostToDevice() {
  // Don't connect any streams if using synthetic data
  if (UseSyntheticData()) {
    return;
  }

  for (auto arg : args_map_) {
    if (arg.second.streamed) {
      void* buf = PreProcessBuffer(arg.second);
      current_engine_->connectStream(arg.first, buf);
    }
  }
}

void PoplarExecutor::ConnectStreamedVariablesDeviceToHost() {
  // Don't connect any streams if using synthetic data
  if (UseSyntheticData()) {
    return;
  }

  for (auto output : outputs_map_) {
    if (output.second.streamed) {
      TensorControl* tc = output.second.tc;
      ConnectReplicatedDeviceToHost(output.first, tc);
    }
  }
}

void PoplarExecutor::PostProcessStreamedVariablesDeviceToHost() {
  for (auto output : outputs_map_) {
    if (output.second.streamed) {
      PostProcessBuffer(output.second.tc);
    }
  }
}

void PoplarExecutor::AboutToFreeEngine(poplar::Engine* engine) {
  if (current_engine_ != nullptr) {
    std::lock_guard<std::recursive_mutex> g(mutex_);
    if (engine == current_engine_) {
      auto status = MoveDeviceToHost();
      if (!status.ok()) {
        LOG(FATAL) << status.ToString();
      }
      DeferredDeallocation();
      current_engine_ = NULL;
    }
  }
}

const int PoplarExecutor::device_ordinal() const { return ordinal_; }

poplar::DeviceManager& PoplarExecutor::GetDeviceManager() {
  static poplar::DeviceManager device_mgr =
      poplar::DeviceManager::createDeviceManager();
  return device_mgr;
}

void PoplarExecutor::CreateInfeedDatasetIterator(
    const PoplarFeedConfig& feed_config,
    std::unique_ptr<tensorflow::FunctionLibraryDefinition>& flib_def,
    std::unique_ptr<tensorflow::ProcessFunctionLibraryRuntime>& process_lib,
    std::unique_ptr<tensorflow::data::FunctionHandleCache>& handle_cache,
    std::unique_ptr<tensorflow::data::IteratorBase>& iterator,
    std::unique_ptr<tensorflow::data::IteratorContext>& iterator_ctx,
    const std::vector<xla::Shape>& shapes) {
  auto& feed_id = feed_config.feed_id();
  if (infeed_dataset_iterators_.contains(feed_id)) {
    LOG(FATAL) << "Infeed with id='" << feed_id
               << "' already exists. Consider changing the `feed_name` in "
                  "IPUInfeedQueue. The Poplar backend requires all infeeds in "
                  "the same TensorFlow device to have unique names.";
  } else {
    infeed_dataset_iterators_[feed_id] =
        absl::make_unique<InfeedDatasetIterator>(
            feed_config.replication_factor(), std::move(flib_def),
            std::move(process_lib), std::move(handle_cache),
            std::move(iterator), std::move(iterator_ctx), shapes);
  }
}

Status PoplarExecutor::DeleteInfeedDatasetIterator(const std::string& feed_id) {
  std::lock_guard<std::mutex> l(infeeds_mutex_);

  if (!infeeds_done_) {
    return xla::FailedPrecondition(
        "Cannot delete infeed with id='%s' while in use", feed_id.c_str());
  }

  const auto num_erased = infeed_dataset_iterators_.erase(feed_id);
  if (num_erased == 0) {
    return xla::NotFound(
        "Infeed with id='%s'. Make sure that you have run the initializer "
        "for this infeed before attempting to delete it.",
        feed_id.c_str());
  }

  return Status::OK();
}

InfeedAllocator* PoplarExecutor::GetInfeedAllocator() {
  return &infeed_allocator;
}

std::vector<std::vector<tensorflow::Tensor>>
PoplarExecutor::GetTensorsFromOutfeed(const std::string& feed_id,
                                      const PoplarFeedConfig_Mode& mode) {
  auto itr = outfeed_contexts_.find(feed_id);
  if (itr == outfeed_contexts_.end()) {
    LOG(INFO)
        << "Trying to dequeue elements from the outfeed queue with id="
        << feed_id
        << " which has not executed yet. Make sure to execute the "
           "program with the outfeed before trying to dequeue an outfeed.";
    return {};
  }
  auto& outfeed_context = itr->second;
  // Lock whilst we dequeue all the tensors.
  std::lock_guard<std::recursive_mutex> guard(outfeed_context->mutex);

  if (mode == xla::poplarplugin::PoplarFeedConfig::GetAll) {
    std::vector<std::vector<tensorflow::Tensor>> output(
        outfeed_context->io_thread_output_queues.size());
    for (size_t i = 0; i < output.size(); ++i) {
      output[i] = outfeed_context->io_thread_output_queues.back();
      outfeed_context->io_thread_output_queues.pop_back();
    }
    return output;
  } else {
    std::vector<std::vector<tensorflow::Tensor>> output(1);
    output[0] = outfeed_context->io_thread_output_queues.front();
    outfeed_context->io_thread_output_queues.clear();
    return output;
  }
}

Status PoplarExecutor::RegisterOutfeeds(const OutfeedInfos& outfeed_infos) {
  for (auto& outfeed_info : outfeed_infos) {
    auto outfeed_id = outfeed_info.config.feed_id();
    if (outfeed_contexts_.contains(outfeed_id)) {
      return xla::FailedPrecondition(
          "Outfeed with id='%s' already exists. Consider changing the "
          "`feed_name` in IPUOutfeedQueue. The Poplar backend requires all "
          "outfeeds in the same TensorFlow device to have unique names.",
          outfeed_id.c_str());
    } else {
      outfeed_contexts_[outfeed_id] =
          absl::make_unique<OutfeedContext>(outfeed_info);
    }
  }
  return Status::OK();
}

Status PoplarExecutor::DeleteOutfeed(const std::string& feed_id) {
  std::lock_guard<std::mutex> l(outfeeds_mutex_);

  if (!outfeeds_done_) {
    return xla::FailedPrecondition(
        "Cannot delete outfeed with id='%s' while in use", feed_id.c_str());
  }

  const auto num_erased = outfeed_contexts_.erase(feed_id);
  if (num_erased == 0) {
    return xla::NotFound(
        "Outfeed with id='%s'. Make sure that you have executed the program "
        "with this outfeed before attempting to delete it.",
        feed_id.c_str());
  }

  return Status::OK();
}

tensorflow::Rendezvous* PoplarExecutor::GetRendezvous() {
  return rendezvous_.get();
}

void PoplarExecutor::ConnectSeedCallback() {
  // Don't connect any streams if using synthetic data
  if (UseSyntheticData()) {
    return;
  }

  auto& generator = seed_generator_;
  for (int replica_id = 0; replica_id < current_replication_factor_;
       ++replica_id) {
    auto callback = [&generator, replica_id](void* ptr) mutable {
      reinterpret_cast<uint64_t*>(ptr)[0] = generator.Get(replica_id);
    };

    current_engine_->connectStreamToCallback(GetRandomNumberSeedStream(),
                                             replica_id, callback);
  }
}

void PoplarExecutor::ResetSeed(int seed) { seed_generator_.Seed(seed); }

std::string PoplarExecutor::GetCycleCounterStream() {
  return "__cycle_count_stream";
}

void PoplarExecutor::ConnectCycleCounterCallback() {
  if (has_cycle_counter_) {
    for (int i = 0; i < current_replication_factor_; i++) {
      current_engine_->connectStreamToCallback(
          PoplarExecutor::GetCycleCounterStream(), i, [=](void* p) {
            // Just log cyclecount for replica 0
            if (i == 0) {
              uint64_t count;
              std::memcpy(&count, p, sizeof(count));
              LOG(INFO) << "Cycle count: " << count;
            }
          });
    }
  }
}

StatusOr<se::DeviceMemoryBase> PoplarExecutor::ExecuteEngine(
    perftools::gputools::StreamExecutor* executor,
    xla::poplarplugin::PoplarExecutable& executable,
    se::DeviceMemoryAllocator* allocator, const Args& args) {
  std::lock_guard<std::recursive_mutex> g(mutex_);
  const auto& input_output_aliasing_map =
      executable.GetInputOutputAliasingMap();
  const auto& output_shape = executable.result_shape();
  poplar::Engine* engine = executable.Engine();

  perftools::gputools::DeviceMemoryBase retbuf;

  bool engine_changed(current_engine_ != engine);

  UpdateArgsHandleMap(args, allocator, executable);

  if (engine == NULL) {
    // An empty engine is either a graph that just passes its inputs through
    // to its outputs, or a graph which returns a constant.
    if (executable.IsConstantGraph()) {
      retbuf =
          GetOutputBuffer(executable, allocator,
                          ConstantOutputAllocation(executable.LiteralValue()),
                          output_shape, args, input_output_aliasing_map);
    } else if (executable.IsRemapGraph()) {
      RemapOutputAllocation remap(this, executable.RemapMap(),
                                  input_output_aliasing_map);
      retbuf = GetOutputBuffer(executable, allocator, remap, output_shape, args,
                               input_output_aliasing_map);
    } else {
      LOG(FATAL) << "Cannot construct a NULL graph.";
    }
  } else {
    if (!executable.has_module()) {
      return tensorflow::errors::InvalidArgument(
          "Executable must have an HloModule");
    }

    TF_ASSIGN_OR_RETURN(const bool move_device_to_host,
                        CheckMoveDeviceToHostRequired(engine_changed));

    if (move_device_to_host) {
      TF_RETURN_IF_ERROR(MoveDeviceToHost());
    }

    if (engine_changed) {
      try {
        engine->load(poplar_device_);

        current_engine_ = engine;
        current_replication_factor_ = executable.GetReplicationFactor();

        ConnectSeedCallback();
        ConnectCycleCounterCallback();

        if (current_config_.profiling().enable_ipu_trace_events() &&
            current_config_.profiling().enable_io_trace()) {
          AddLoadEngineEventRecord(executable.module().name());
        }

        executable.OnEngineLoaded();

      } catch (const std::exception& e) {
        return PoplarExceptionToTensorflowStatus("[Load engine] ", e);
      }
    }

    // Deallocate all the marked buffers.
    DeferredDeallocation();

    TF_ASSIGN_OR_RETURN(const bool move_host_to_device,
                        CheckMoveHostToDeviceRequired(engine_changed));
    if (move_host_to_device) {
      TF_RETURN_IF_ERROR(MoveHostToDevice());
    }

    // Outfeeds add empty tuples as output shape, no need to get an output
    // buffer in this case
    if (ShapeUtil::IsEmptyTuple(output_shape)) {
      outputs_map_.clear();
    } else {
      retbuf = GetOutputBuffer(executable, allocator, BufferOutputAllocation(),
                               output_shape, args, input_output_aliasing_map);

      UpdateOutputsHandleMap(executable, output_shape, retbuf);
    }

    VLOG(1) << "Executing on poplar stream ordinal " << ordinal_ << " of type "
            << GetDeviceTargetName();

    // Create our own free list which we use to allocate all the memory used by
    // all the tensors.
    std::list<std::unique_ptr<char[]>> memory_buffer;

    // Allocate the parameters for each of the functors, sorted by the user
    // instruction which they are created for.
    std::unordered_map<const HloInstruction*, std::vector<void*>> in_buffers;
    std::unordered_map<const HloInstruction*, std::vector<std::uint32_t>>
        in_sizes;
    std::unordered_map<const HloInstruction*, std::vector<void*>> out_buffer;

    try {
      // Connect the streams to and from the device
      ConnectStreamedVariablesHostToDevice();
      ConnectStreamedVariablesDeviceToHost();
      const StreamInfos& stream_infos = executable.GetStreamInfos();

      // If this is a user op copy the buffers.
      // We add one call back to the stream which allocates the buffers and once
      // all buffers have been allocated finally calls down to the user
      // operation.
      for (auto& pair : executable.GetStreamMetaInfos()) {
        StreamCopyMetaInfo infos = pair.second;

        const HloInstruction* instruction = infos.parent_instruction;

        out_buffer[instruction].resize(infos.output_stream_info.size());

        // Resize the input vectors to be the number of inputs in advance.
        in_buffers[instruction].resize(infos.num_inputs);
        in_sizes[instruction].resize(infos.num_inputs);

        // For each of the output stream copies allocate a buffer.
        for (StreamCopyInfo* stream_copy : infos.output_stream_info) {
          assert(stream_copy->operand_number <
                     infos.output_stream_info.size() &&
                 "Operand ID is greater than the number of output streams "
                 "StreamCopyMetaInfo can see.");

          const std::uint32_t totalSize =
              stream_copy->size_of_element * stream_copy->number_of_elements;
          memory_buffer.push_back(std::unique_ptr<char[]>(new char[totalSize]));

          out_buffer[instruction][stream_copy->operand_number] =
              (void*)memory_buffer.back().get();
        }
      }

      TF_RETURN_IF_ERROR(
          ConnectSendCallbacksToRendezvous(executable.GetSendInfos()));

      TF_RETURN_IF_ERROR(
          ConnectRecvCallbacksToRendezvous(executable.GetRecvInfos()));

      const auto& infeed_infos = executable.GetInfeedInfos();
      if (!infeed_infos.empty()) {
        ConnectInfeedsToStreamCallback(infeed_infos);
      }

      const auto& outfeed_infos = executable.GetOutfeedInfos();
      if (!outfeed_infos.empty()) {
        ConnectOutfeedToStreamCallback(outfeed_infos);
      }

      for (auto& pair : stream_infos) {
        const std::string name = pair.first;
        const std::list<StreamCopyInfo>& list = pair.second;

        // Track how many inputs have been initalized so far.
        std::uint32_t number_of_inputs_initalized = 0;

        // For all of the stream copies, both inputs and outputs.
        for (const StreamCopyInfo& info : list) {
          StreamCopyInfo::FunctionTy functor = info.callback_to_register;

          // If there is a functor then this is an input tensor, we will attach
          // the callbacks to the stream otherwise just copy into the previously
          // allocated pegged memory.
          if (functor != nullptr) {
            // Create a custom callback which we use to copy the inputs as
            // these callbacks are called in a random order we have to work
            // out which tensor we are writing into and we have to check how
            // many inputs we have already initialized so we know to call the
            // user provided operation once they have all been set up.
            auto callback = [&, functor](void* buffer) {
              std::vector<void*>& in_buffer =
                  in_buffers[info.parent_instruction];
              std::vector<std::uint32_t>& in_size =
                  in_sizes[info.parent_instruction];

              // Allocate space for the input tensor and then memcopy into it.
              // The 'buffer' pointer is only garunteed to be alive for the
              // duration of this callback.
              std::uint32_t totalSize =
                  info.size_of_element * info.number_of_elements;
              memory_buffer.push_back(
                  std::unique_ptr<char[]>(new char[totalSize]));
              in_buffer[info.operand_number] =
                  (void*)memory_buffer.back().get();

              // Copy into the newly allocated memory.
              std::memcpy((char*)in_buffer[info.operand_number], (char*)buffer,
                          totalSize);
              number_of_inputs_initalized++;

              // Store the size of each input.
              in_size[info.operand_number] = info.number_of_elements;

              // These callbacks are called in a random order by poplar so we
              // need to only call the user provided callback once, after all of
              // the data has been initialized.
              if (number_of_inputs_initalized == in_buffer.size()) {
                functor(in_buffer, in_size,
                        out_buffer[info.parent_instruction]);
              }
            };

            current_engine_->connectStreamToCallback(info.stream_handle,
                                                     callback);
          } else {
            // Connect the output stream to the correct pre-allocated buffer.
            current_engine_->connectStream(
                info.stream_handle,
                out_buffer[info.parent_instruction][info.operand_number]);
          }
        }
      }
      // Launch the IO threads when we are not using synthetic data and have
      // infeeds/outfeeds.
      bool io_threads_running = false;
      if (!UseSyntheticData() &&
          (!infeed_infos.empty() || !outfeed_infos.empty())) {
        LaunchIOThreads(infeed_infos, outfeed_infos);
        io_threads_running = true;
      }

      // Before executing the main program, prepare the random seeds for each
      // replica.
      seed_generator_.PrepareSeedsForReplicas(current_replication_factor_);

      // Run the main engine
      current_engine_->enableExecutionProfiling();
      current_engine_->run(PoplarProgramType::MAIN_SEQUENCE);

      if (io_threads_running) {
        StopIOThreads(infeed_infos, outfeed_infos);
      }

      // We need to call post process to make sure all the data is in the
      // right format on the host
      PostProcessStreamedVariablesDeviceToHost();

    } catch (const std::exception& e) {
      return PoplarExceptionToTensorflowStatus("[Execute engine] ", e);
    }

    try {
      if (!PoplarXlaFlags::Get().save_interval_report.empty() &&
          executable.ExecutionCount() == 0) {
        auto filename =
            tensorflow::io::JoinPath(PoplarXlaFlags::Get().save_interval_report,
                                     executable.module().name() + ".csv");
        VLOG(1) << "Dumping interval report " << filename;
        std::ofstream stream(filename);
        current_engine_->reportIntervals(stream);
      }

      if (current_config_.profiling().enable_ipu_trace_events()) {
        std::string report;
        if (current_config_.profiling().enable_execution_trace() > 0) {
          if (executable.ExecutionCount() == 0 &&
              !executable.IsLoadedFromCache()) {
            std::stringstream report_stream;
            auto graph_profile = current_engine_->getGraphProfile();
            auto exec_profile = current_engine_->getExecutionProfile();

            if (PoplarXlaFlags::Get().dump_text_reports_to_stdio) {
              auto opts = GetReportFlags();
              SetFlagIfNotPresent(opts, "showExecutionSteps", "true");
              poplar::printExecutionSummary(std::cout, graph_profile,
                                            exec_profile, opts);
            }

            if (CompilerReportingTextFormat()) {
              auto opts = GetReportFlags();
              SetFlagIfNotPresent(opts, "showExecutionSteps", "true");

              poplar::printExecutionSummary(report_stream, graph_profile,
                                            exec_profile, opts);
            } else if (CompilerReportingCborFormat()) {
              poplar::serializeToCBOR(report_stream, exec_profile);
            } else {
              poplar::serializeToJSON(report_stream, exec_profile);
            }

            current_engine_->resetExecutionProfile();

            if (report_stream.tellp() > MaxReportSize()) {
              LOG(WARNING) << "Dropping Poplar execution report, size was "
                           << report_stream.tellp();
              report_stream.str(std::string());
            }
            report = report_stream.str();
          }
        }

        AddExecuteEventRecord(executable.module().name(), report);
      }
    } catch (const std::exception& e) {
      return PoplarExceptionToTensorflowStatus("[Execute engine] ", e);
    }
  }

  return retbuf;
}

}  // namespace poplarplugin
}  // namespace xla
