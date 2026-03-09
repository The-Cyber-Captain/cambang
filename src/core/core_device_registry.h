// src/core/core_device_registry.h
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace cambang {

// Minimal per-device core state registry.
//
// Purpose (this build slice):
// - Track which device_instance_ids are known/open according to provider facts.
// - Provide deterministic ordering for teardown (ascending instance_id).
//
// Threading:
// - Core-thread-only. Not atomic. Determinism-first.
class CoreDeviceRegistry final {
public:
  struct DeviceRecord {
    uint64_t device_instance_id = 0;
    std::string hardware_id;
    uint64_t camera_spec_version = 0;
    bool open = false;
    uint32_t last_error_code = 0;
    uint64_t errors_count = 0;
  };

  bool note_device_identity(uint64_t device_instance_id, const std::string& hardware_id);
  bool set_camera_spec_version(uint64_t device_instance_id, uint64_t camera_spec_version);

  bool on_device_opened(uint64_t device_instance_id);
  bool on_device_closed(uint64_t device_instance_id);
  bool on_device_error(uint64_t device_instance_id, uint32_t error_code);

  const DeviceRecord* find(uint64_t device_instance_id) const noexcept;
  const std::map<uint64_t, DeviceRecord>& all() const noexcept { return devices_; }

private:
  std::map<uint64_t, DeviceRecord> devices_; // key: device_instance_id
};

} // namespace cambang
