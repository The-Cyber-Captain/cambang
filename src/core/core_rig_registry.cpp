// src/core/core_rig_registry.cpp

#include "core/core_rig_registry.h"

namespace cambang {

bool CoreRigRegistry::retain_capture_profile(uint64_t rig_id,
                                             uint32_t width,
                                             uint32_t height,
                                             uint32_t format,
                                             uint64_t capture_profile_version) {
  if (rig_id == 0) {
    return false;
  }

  auto& rec = rigs_[rig_id];
  rec.rig_id = rig_id;
  rec.capture_width = width;
  rec.capture_height = height;
  rec.capture_format = format;
  rec.capture_profile_version = capture_profile_version;
  rec.live = true;
  return true;
}

const CoreRigRegistry::RigRecord* CoreRigRegistry::find(uint64_t rig_id) const noexcept {
  const auto it = rigs_.find(rig_id);
  if (it == rigs_.end()) {
    return nullptr;
  }
  return &it->second;
}

} // namespace cambang
