#pragma once

#include <cstdint>
#include <map>

#include "core/snapshot/state_snapshot.h"

namespace cambang {

class CoreAcquisitionSessionRegistry final {
public:
  struct AcquisitionSessionEntry {
    uint64_t acquisition_session_id = 0;
    uint64_t device_instance_id = 0;

    CBLifecyclePhase phase = CBLifecyclePhase::CREATED;

    uint64_t capture_profile_version = 0;
    uint32_t capture_width = 0;
    uint32_t capture_height = 0;
    uint32_t capture_format = 0;

    uint64_t captures_triggered = 0;
    uint64_t captures_completed = 0;
    uint64_t captures_failed = 0;

    uint64_t last_capture_id = 0;
    uint64_t last_capture_latency_ns = 0;

    int32_t error_code = 0;
    uint64_t started_ns = 0;
    uint64_t ended_ns = 0;
  };

  bool on_native_object_created(uint64_t native_id,
                                uint32_t type,
                                uint64_t owner_device_instance_id,
                                uint64_t created_ns);
  bool on_native_object_destroyed(uint64_t native_id, uint64_t destroyed_ns);
  void clear();

  const std::map<uint64_t, AcquisitionSessionEntry>& all() const noexcept { return sessions_; }

private:
  std::map<uint64_t, AcquisitionSessionEntry> sessions_;
};

} // namespace cambang
