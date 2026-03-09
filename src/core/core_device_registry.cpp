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

bool CoreDeviceRegistry::on_device_opened(uint64_t device_instance_id) {
  if (device_instance_id == 0) {
    return false;
  }

  auto& rec = devices_[device_instance_id];
  rec.device_instance_id = device_instance_id;
  rec.open = true;
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
