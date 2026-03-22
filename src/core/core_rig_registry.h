// src/core/core_rig_registry.h
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cambang {

class CoreRigRegistry final {
public:
  struct RigRecord {
    uint64_t rig_id = 0;
    std::string name;
    std::vector<std::string> member_hardware_ids;

    uint64_t active_capture_id = 0;
    uint64_t capture_profile_version = 0;
    uint32_t capture_width = 0;
    uint32_t capture_height = 0;
    uint32_t capture_format = 0;

    uint64_t captures_triggered = 0;
    uint64_t captures_completed = 0;
    uint64_t captures_failed = 0;

    uint64_t last_capture_id = 0;
    uint64_t last_capture_latency_ns = 0;
    uint64_t last_sync_skew_ns = 0;

    int32_t error_code = 0;
    bool live = false;
  };

  bool retain_capture_profile(uint64_t rig_id,
                              uint32_t width,
                              uint32_t height,
                              uint32_t format,
                              uint64_t capture_profile_version);

  void clear() noexcept { rigs_.clear(); }

  const RigRecord* find(uint64_t rig_id) const noexcept;
  const std::map<uint64_t, RigRecord>& all() const noexcept { return rigs_; }

private:
  std::map<uint64_t, RigRecord> rigs_;
};

} // namespace cambang
