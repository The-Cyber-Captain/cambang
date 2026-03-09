#include "core/core_spec_state.h"

namespace cambang {

void CoreSpecState::reset_for_generation(uint64_t imaging_spec_version) {
  camera_spec_versions_.clear();
  imaging_spec_version_ = imaging_spec_version;
}

void CoreSpecState::set_camera_spec_version(const std::string& hardware_id, uint64_t camera_spec_version) {
  if (hardware_id.empty()) {
    return;
  }
  camera_spec_versions_[hardware_id] = camera_spec_version;
}

uint64_t CoreSpecState::camera_spec_version(const std::string& hardware_id) const noexcept {
  if (hardware_id.empty()) {
    return 0;
  }
  const auto it = camera_spec_versions_.find(hardware_id);
  if (it == camera_spec_versions_.end()) {
    return 0;
  }
  return it->second;
}

void CoreSpecState::set_imaging_spec_version(uint64_t imaging_spec_version) noexcept {
  imaging_spec_version_ = imaging_spec_version;
}

} // namespace cambang
