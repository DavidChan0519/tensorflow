#include "tensorflow/compiler/plugin/poplar/driver/custom_ops/poplibs_ops.h"

#include <string>
#include "absl/container/flat_hash_map.h"

namespace xla {
namespace poplarplugin {
namespace {
absl::flat_hash_map<std::string, CustomPoplibOpInfo> info_map = {};
}

const absl::flat_hash_map<std::string, CustomPoplibOpInfo>&
GetPoplinOpInfoMap() {
  return info_map;
}

}  // namespace poplarplugin
}  // namespace xla
