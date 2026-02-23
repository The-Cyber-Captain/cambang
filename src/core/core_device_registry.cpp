// src/core/core_device_registry.cpp

#include "core/core_device_registry.h"

namespace cambang {

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
    // Best-effort: create a record so snapshots/diagnostics can reflect closure.
    DeviceRecord rec;
    rec.device_instance_id = device_instance_id;
    rec.open = false;
    devices_.emplace(device_instance_id, rec);
    return true;
  }

  it->second.open = false;
  return true;
}

bool CoreDeviceRegistry::on_device_error(uint64_t device_instance_id, uint32_t error_code) {
  if (device_instance_id == 0) {
    return false;
  }
  auto& rec = devices_[device_instance_id];
  rec.device_instance_id = device_instance_id;
  rec.last_error_code = error_code;
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
