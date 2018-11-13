#include "include/json/json.h"

#include "absl/container/inlined_vector.h"
#include "absl/strings/str_cat.h"

#include "tensorflow/compiler/plugin/poplar/driver/compiler_resources.h"
#include "tensorflow/compiler/plugin/poplar/driver/inplace_util.h"
#include "tensorflow/compiler/plugin/poplar/driver/util.h"

#include <algorithm>
#include <limits>

#include <poputil/TileMapping.hpp>

using ::absl::StrCat;

namespace xla {
namespace poplarplugin {

std::string GetDebugName(const HloInstruction* inst) {
  const std::string& tf_core_name = inst->metadata().op_name();
  return tf_core_name + "/" + inst->name();
}

poplar::Graph& GetGraph(CompilerResources& res, const HloInstruction* inst) {
  if (inst->has_sharding()) {
    const auto& sharding = inst->sharding();
    if (sharding.HasUniqueDevice()) {
      uint64 device_id = sharding.GetUniqueDevice();
      if (device_id < res.shard_graphs.size()) {
        return res.shard_graphs[device_id];
      }
    }
  }

  return res.main_graph;
}

static bool InstructionSharded(const HloInstruction* a) {
  if (a->has_sharding()) {
    const auto& a_sharding = a->sharding();
    if (a_sharding.HasUniqueDevice()) {
      return true;
    }
  }

  return false;
}

static uint64 GetShard(const HloInstruction* inst) {
  if (inst->has_sharding()) {
    const auto& sharding = inst->sharding();
    if (sharding.HasUniqueDevice()) {
      return sharding.GetUniqueDevice();
    }
  }
  return 0;
}

std::pair<int64, int64> FindTupleInputIndices(const HloInstruction* tuple,
                                              int64 n) {
  int64 start = 0;
  for (int64 i = 0; i < n; i++) {
    start += CountShapes(tuple->operand(i)->shape());
  }
  int64 end = start + CountShapes(tuple->operand(n)->shape());
  return std::make_pair(start, end);
}

ArgVector FindTupleInInstructionInput(const TensorMap& map,
                                      const HloInstruction* inst, int64 input,
                                      int64 n) {
  const HloInstruction* operand = inst->operand(input);
  const Shape& shape = operand->shape();
  OutVector outputs = FindInstructionOutputs(map, operand);
  int64 start = 0;
  for (int64 i = 0; i < n; i++) {
    start += CountShapes(ShapeUtil::GetTupleElementShape(shape, i));
  }
  int64 end = start + CountShapes(ShapeUtil::GetTupleElementShape(shape, n));

  return ArgVector(&outputs[start], &outputs[end]);
}

StatusOr<poplar::Tensor> FindInstructionInput(const TensorMap& map,
                                              CompilerResources& res,
                                              const HloInstruction* inst,
                                              int64 input,
                                              poplar::program::Sequence& seq) {
  const HloInstruction* operand = inst->operand(input);
  OutVector outputs = FindInstructionOutputs(map, operand);
  if (outputs.size() == 0) {
    return tensorflow::errors::Unknown(
        StrCat("[Poplar] Couldn't find input ", input, " for ", inst->name()));
  }

  poplar::Tensor out = outputs[0];
  if (InstructionSharded(inst)) {
    out = poputil::copyToIpu(res.main_graph, out, seq, GetShard(inst));
  }

  return out;
}

ArgVector FindInstructionInputs(const TensorMap& map, CompilerResources& res,
                                const HloInstruction* inst, int64 input,
                                poplar::program::Sequence& seq) {
  const HloInstruction* operand = inst->operand(input);
  OutVector inputs = FindInstructionOutputs(map, operand);
  if (InstructionSharded(inst)) {
    for (unsigned int i = 0; i < inputs.size(); i++) {
      inputs[i] =
          poputil::copyToIpu(res.main_graph, inputs[i], seq, GetShard(inst));
    }
  }
  return inputs;
}

OutVector FindInstructionOutputs(const TensorMap& map,
                                 const HloInstruction* inst) {
  auto lower = std::make_pair(inst->name(), 0);
  auto upper = std::make_pair(inst->name(), std::numeric_limits<int64>::max());
  OutVector outputs;
  for (auto it = map.lower_bound(lower); it != map.upper_bound(upper); it++) {
    outputs.push_back(it->second);
  }
  return outputs;
}

StatusOr<ArgVector> GetInplaceOutputTensors(poplar::Graph& graph,
                                            CompilerResources& res,
                                            poplar::program::Sequence& seq,
                                            const HloInstruction* inst,
                                            TensorMap& tensor_map) {
  const bool is_still_inplace =
      res.annotations.inplace_instructions.count(inst);

  auto inst_description =
      InplaceUtil::GetHloInstructionDescription(inst, res.annotations);
  // Check that the instruction description is for an inplace operation.
  if (!inst_description->IsInPlaceType(inst)) {
    LOG(FATAL) << "Trying to execute " << inst->name()
               << " as an inplace operation, but it is not.";
  }

  // Go through all the inplace tensors and check if we need to add copies.
  auto& inplace_description =
      *static_cast<InplaceUtil::InplaceHloInstructionDescription*>(
          inst_description.get());
  ArgVector outs;
  for (auto inplace_idx : inplace_description.GetInplaceOperandIndexes()) {
    ArgVector inputs =
        FindInstructionInputs(tensor_map, res, inst, inplace_idx, seq);
    for (auto input : inputs) {
      poplar::Tensor out = input;
      // We need to add a copy before an inplace op if:
      // 1. out is not ParallelWriteable,
      // 2. inst has been removed from inplace ops by a different pass.
      bool requires_copy_inplace =
          !out.isParallelWriteable() || !is_still_inplace;
      if (requires_copy_inplace) {
        VLOG(1) << "Adding a copy for inplace op " << inst->name();
        poplar::Tensor copy = graph.clone(out, GetDebugName(inst) + ".clone");
        seq.add(poplar::program::Copy(out, copy));
        out = copy;
      }
      outs.push_back(out);
    }
  }
  return outs;
}

Status AddOutputTensor(TensorMap& map, const HloInstruction* inst, int64 n,
                       const poplar::Tensor& tensor) {
  auto p = std::make_pair(inst->name(), n);
  auto it = map.find(p);
  if (it != map.end()) {
    return tensorflow::errors::Unknown(StrCat(
        "[Poplar] Ouptut Tensor for ", GetDebugName(inst), " already exists"));
  }
  map[p] = tensor;
  return Status::OK();
}

template <typename TYPE>
static void SetVertexField(poplar::Graph& graph, const poplar::FieldRef& field,
                           const Literal& literal) {
  const TYPE* value(static_cast<const TYPE*>(literal.untyped_data()));
  graph.setInitialValue<TYPE>(field, *value);
}

static void SetFp16VertexField(poplar::Graph& graph,
                               const poplar::FieldRef& field,
                               const Literal& literal) {
  const uint16_t* value(static_cast<const uint16_t*>(literal.untyped_data()));
  graph.setInitialValueHalf(field, *value);
}

Status SetVertexField(poplar::Graph& graph, const poplar::FieldRef& field,
                      const Literal& literal) {
  switch (literal.shape().element_type()) {
    case PRED:
      SetVertexField<bool>(graph, field, literal);
      break;
    case S32:
    case U32:
      SetVertexField<int>(graph, field, literal);
      break;
    case F16:
      SetFp16VertexField(graph, field, literal);
      break;
    case F32:
      SetVertexField<float>(graph, field, literal);
      break;
    default:
      return xla::FailedPrecondition("Unrecognised type in SetVertexField: %d",
                                     literal.shape().element_type());
  }
  return Status::OK();
}

std::string GetTensorMappingJson(const poplar::Graph& graph,
                                 const TensorMaps& tensor_maps) {
  Json::Value mappings;

  for (auto tm : tensor_maps) {
    mappings[tm.first] = Json::Value(Json::arrayValue);

    for (auto pair : tm.second) {
      const auto& pop_tensor = pair.second;

      Json::Value tensor;
      tensor["inst_name"] = Json::Value(pair.first.first);
      tensor["output_index"] = Json::Value::UInt64(pair.first.second);
      tensor["constant"] = Json::Value::UInt64(pop_tensor.containsConstant());
      tensor["tiles"] = Json::Value(Json::arrayValue);

      const auto& mapping = graph.getTileMapping(pop_tensor);
      unsigned tiles_used = 0;
      size_t total_elements = 0;

      for (size_t tile_idx = 0; tile_idx < mapping.size(); tile_idx++) {
        const auto& tile = mapping[tile_idx];
        if (tile.size() != 0) {
          tiles_used++;
          size_t tile_element_count = 0;
          for (const auto& interval : tile) {
            tile_element_count += interval.size();
          }

          Json::Value tile;
          tile["tile_id"] = Json::Value::UInt64(tile_idx);
          tile["num_intervals"] = Json::Value::UInt64(tile.size());
          tile["num_elements"] = Json::Value::UInt64(tile_element_count);
          tile["element_type"] =
              Json::Value(pop_tensor.elementType().toString());
          tensor["tiles"].append(tile);

          total_elements += tile_element_count;
        }
      }

      tensor["tiles_used"] = Json::Value::UInt64(tiles_used);
      tensor["total_elements"] = Json::Value::UInt64(total_elements);

      mappings[tm.first].append(tensor);
    }
  }

  Json::Value root;
  root["mappings"] = mappings;

  Json::StreamWriterBuilder json_builder;
  std::string json_msg = Json::writeString(json_builder, root);

  if (VLOG_IS_ON(2)) {
    VLOG(2) << "[Poplar] Dumping tensor mapping";
    VLOG(2) << json_msg;
  }

  return json_msg;
}

Status PoplarExceptionToTensorflowStatus(const std::string& prefix,
                                         const std::exception& e) {
  /* NOTE: Reduce this list if/when Poplar errors are subclassed */
  try {
    std::rethrow_exception(std::current_exception());
  } catch (const poplar::file_load_error& e) {
    return tensorflow::errors::NotFound(prefix, e.what());
  } catch (const poplar::missing_cycle_estimate& e) {
    return tensorflow::errors::NotFound(prefix, e.what());
  } catch (const poplar::symbol_error& e) {
    return tensorflow::errors::NotFound(prefix, e.what());
  } catch (const poplar::unknown_field& e) {
    return tensorflow::errors::NotFound(prefix, e.what());
  } catch (const poplar::unknown_vertex_type& e) {
    return tensorflow::errors::NotFound(prefix, e.what());
  } catch (const poplar::no_environment& e) {
    return tensorflow::errors::NotFound(prefix, e.what());
  } catch (const poplar::parse_error& e) {
    return tensorflow::errors::InvalidArgument(prefix, e.what());
  } catch (const poplar::invalid_option& e) {
    return tensorflow::errors::InvalidArgument(prefix, e.what());
  } catch (const poplar::invalid_machine_model& e) {
    return tensorflow::errors::InvalidArgument(prefix, e.what());
  } catch (const poplar::stream_connection_error& e) {
    return tensorflow::errors::InvalidArgument(prefix, e.what());
  } catch (const poplar::graph_cycle_error& e) {
    return tensorflow::errors::InvalidArgument(prefix, e.what());
  } catch (const poplar::invalid_tile_mapping& e) {
    return tensorflow::errors::InvalidArgument(prefix, e.what());
  } catch (const poplar::type_error& e) {
    return tensorflow::errors::InvalidArgument(prefix, e.what());
  } catch (const poplar::no_size_specified& e) {
    return tensorflow::errors::InvalidArgument(prefix, e.what());
  } catch (const poplar::profiling_disabled& e) {
    return tensorflow::errors::InvalidArgument(prefix, e.what());
  } catch (const poplar::control_program_error& e) {
    return tensorflow::errors::InvalidArgument(prefix, e.what());
  } catch (const poplar::runtime_error& e) {
    return tensorflow::errors::Internal(prefix, e.what());
  } catch (const poplar::overflow_error& e) {
    return tensorflow::errors::Internal(prefix, e.what());
  } catch (const poplar::tensor_io_state_error& e) {
    return tensorflow::errors::Internal(prefix, e.what());
  } catch (const poplar::graph_connection_error& e) {
    return tensorflow::errors::Internal(prefix, e.what());
  } catch (const poplar::graph_object_load_error& e) {
    return tensorflow::errors::Internal(prefix, e.what());
  } catch (const poplar::graph_object_creation_error& e) {
    return tensorflow::errors::Internal(prefix, e.what());
  } catch (const poplar::graph_program_compilation_error& e) {
    return tensorflow::errors::Internal(prefix, e.what());
  } catch (const poputil::poplib_error& e) {
    return tensorflow::errors::Internal(prefix, e.what());
  } catch (const poplar::link_error& e) {
    return tensorflow::errors::ResourceExhausted(prefix, e.what());
  } catch (const poplar::stream_memory_allocation_error& e) {
    return tensorflow::errors::ResourceExhausted(prefix, e.what());
  } catch (const poplar::graph_memory_allocation_error& e) {
    return tensorflow::errors::ResourceExhausted(prefix, e.what());
  } catch (const poplar::tensor_creation_error& e) {
    return tensorflow::errors::ResourceExhausted(prefix, e.what());
  } catch (const poplar::memory_elem_constraints_error& e) {
    return tensorflow::errors::ResourceExhausted(prefix, e.what());
  } catch (const poplar::index_error& e) {
    return tensorflow::errors::OutOfRange(prefix, e.what());
  } catch (const poplar::poplar_error& e) {
    return tensorflow::errors::Internal(prefix, e.what());
  }

  return tensorflow::errors::Unknown(prefix, e.what());
}

}  // namespace poplarplugin
}  // namespace xla
