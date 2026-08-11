// src/core/core_rig_registry.cpp

#include "core/core_rig_registry.h"

namespace cambang {

bool CoreRigRegistry::record_capture_triggered(uint64_t rig_id, uint64_t rig_capture_id) {
  if (rig_id == 0 || rig_capture_id == 0) {
    return false;
  }
  const auto it = rigs_.find(rig_id);
  if (it == rigs_.end()) {
    return false;
  }
  ++it->second.captures_triggered;
  it->second.active_capture_id = rig_capture_id;
  return true;
}

bool CoreRigRegistry::record_capture_settled(uint64_t rig_id,
                                             uint64_t rig_capture_id,
                                             bool failed) {
  if (rig_id == 0 || rig_capture_id == 0) {
    return false;
  }
  const auto it = rigs_.find(rig_id);
  if (it == rigs_.end()) {
    return false;
  }
  if (failed) {
    ++it->second.captures_failed;
  } else {
    ++it->second.captures_completed;
  }
  it->second.last_capture_id = rig_capture_id;
  // Only clear the active id if this is the capture that was active: a later
  // trigger may already have taken the slot while this one was settling.
  if (it->second.active_capture_id == rig_capture_id) {
    it->second.active_capture_id = 0;
  }
  return true;
}

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

bool CoreRigRegistry::retain_member_hardware_ids(uint64_t rig_id, std::vector<std::string> member_hardware_ids) {
  if (rig_id == 0) {
    return false;
  }
  auto& rec = rigs_[rig_id];
  rec.rig_id = rig_id;
  rec.member_hardware_ids = std::move(member_hardware_ids);
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
