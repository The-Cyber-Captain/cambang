// src/core/core_device_registry.cpp

#include "core/core_device_registry.h"

namespace cambang {

bool CoreDeviceRegistry::note_device_identity(uint64_t device_instance_id, const std::string& hardware_id) {
  if (device_instance_id == 0 || hardware_id.empty()) {
    return false;
  }

  auto& rec = devices_[device_instance_id];
  rec.device_instance_id = device_instance_id;
  rec.hardware_id = hardware_id;
  return true;
}

bool CoreDeviceRegistry::set_camera_spec_version(uint64_t device_instance_id, uint64_t camera_spec_version) {
  if (device_instance_id == 0) {
    return false;
  }

  auto& rec = devices_[device_instance_id];
  rec.device_instance_id = device_instance_id;
  rec.camera_spec_version = camera_spec_version;
  return true;
}

bool CoreDeviceRegistry::retain_capture_profile(uint64_t device_instance_id,
                                                uint32_t width,
                                                uint32_t height,
                                                uint32_t format,
                                                uint64_t capture_profile_version) {
  if (device_instance_id == 0) {
    return false;
  }

  auto& rec = devices_[device_instance_id];
  rec.device_instance_id = device_instance_id;
  rec.capture_width = width;
  rec.capture_height = height;
  rec.capture_format = format;
  rec.capture_profile_version = capture_profile_version;
  return true;
}

bool CoreDeviceRegistry::set_warm_hold_ms(uint64_t device_instance_id, uint32_t warm_hold_ms) {
  if (device_instance_id == 0) {
    return false;
  }

  auto& rec = devices_[device_instance_id];
  rec.device_instance_id = device_instance_id;
  rec.warm_hold_ms = warm_hold_ms;
  if (warm_hold_ms == 0) {
    rec.warm_deadline_active = false;
    rec.warm_deadline_ns = 0;
    rec.warm_expired_close_requested = false;
  }
  return true;
}

bool CoreDeviceRegistry::arm_warm_deadline(uint64_t device_instance_id, uint64_t deadline_ns) {
  if (device_instance_id == 0) {
    return false;
  }

  auto it = devices_.find(device_instance_id);
  if (it == devices_.end()) {
    return false;
  }

  it->second.warm_deadline_active = true;
  it->second.warm_deadline_ns = deadline_ns;
  it->second.warm_expired_close_requested = false;
  return true;
}

bool CoreDeviceRegistry::clear_warm_deadline(uint64_t device_instance_id) {
  if (device_instance_id == 0) {
    return false;
  }

  auto it = devices_.find(device_instance_id);
  if (it == devices_.end()) {
    return false;
  }

  it->second.warm_deadline_active = false;
  it->second.warm_deadline_ns = 0;
  it->second.warm_expired_close_requested = false;
  return true;
}

bool CoreDeviceRegistry::mark_warm_expired_close_requested(uint64_t device_instance_id, bool requested) {
  if (device_instance_id == 0) {
    return false;
  }

  auto it = devices_.find(device_instance_id);
  if (it == devices_.end()) {
    return false;
  }

  it->second.warm_expired_close_requested = requested;
  return true;
}

bool CoreDeviceRegistry::set_warm_was_in_use(uint64_t device_instance_id, bool warm_was_in_use) {
  if (device_instance_id == 0) {
    return false;
  }

  auto it = devices_.find(device_instance_id);
  if (it == devices_.end()) {
    return false;
  }

  it->second.warm_was_in_use = warm_was_in_use;
  return true;
}

bool CoreDeviceRegistry::on_device_opened(uint64_t device_instance_id) {
  if (device_instance_id == 0) {
    return false;
  }

  auto& rec = devices_[device_instance_id];
  rec.device_instance_id = device_instance_id;
  rec.open = true;
  rec.warm_deadline_active = false;
  rec.warm_deadline_ns = 0;
  rec.warm_expired_close_requested = false;
  rec.warm_was_in_use = false;
  return true;
}

bool CoreDeviceRegistry::on_device_closed(uint64_t device_instance_id) {
  if (device_instance_id == 0) {
    return false;
  }

  auto it = devices_.find(device_instance_id);
  if (it == devices_.end()) {
    return false;
  }
  devices_.erase(it);
  return true;
}

bool CoreDeviceRegistry::on_device_error(uint64_t device_instance_id, uint32_t error_code) {
  if (device_instance_id == 0) {
    return false;
  }
  auto& rec = devices_[device_instance_id];
  rec.device_instance_id = device_instance_id;
  rec.last_error_code = error_code;
  rec.errors_count++;
  return true;
}

const CoreDeviceRegistry::DeviceRecord* CoreDeviceRegistry::find(uint64_t device_instance_id) const noexcept {
  auto it = devices_.find(device_instance_id);
  if (it == devices_.end()) {
    return nullptr;
  }
  return &it->second;
}

} // namespace cambang
