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

  bool on_device_opened(uint64_t device_instance_id, uint64_t now_ns);
  bool on_device_closed(uint64_t device_instance_id, uint64_t now_ns);
  void clear();

  const std::map<uint64_t, AcquisitionSessionEntry>& all() const noexcept { return sessions_; }

private:
  uint64_t next_acquisition_session_id_ = 1;
  std::map<uint64_t, uint64_t> active_session_by_device_;
  std::map<uint64_t, AcquisitionSessionEntry> sessions_;
};

} // namespace cambang
