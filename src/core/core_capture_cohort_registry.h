// src/core/core_capture_cohort_registry.h
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cambang {

// Internal maintainer/Core terminology:
// A "cohort" is an admitted rig-triggered capture group (one capture_id, one
// rig_id, and fixed expected participant devices). This registry intentionally
// stores admission metadata only (no payload ownership).
class CoreCaptureCohortRegistry final {
public:
  enum class CohortState : uint8_t {
    OPEN = 0,
    FAILED = 1,
  };

  enum class CohortFailurePhase : uint8_t {
    NONE = 0,
    SUBMISSION = 1,
    EXECUTION = 2,
  };

  struct Participant {
    uint64_t device_instance_id = 0;
    std::string hardware_id;
  };

  struct CohortRecord {
    uint64_t capture_id = 0;
    uint64_t rig_id = 0;
    std::vector<Participant> expected_participants;
    CohortState state = CohortState::OPEN;
    CohortFailurePhase failure_phase = CohortFailurePhase::NONE;
    uint64_t failed_device_instance_id = 0;
    uint32_t failure_error_code = 0;
    bool has_failure_error_code = false;
  };

  void clear() noexcept;

  bool insert(CohortRecord record);
  bool mark_failed(uint64_t capture_id,
                   uint64_t failed_device_instance_id,
                   uint32_t failure_error_code,
                   CohortFailurePhase phase) noexcept;
  bool contains(uint64_t capture_id) const noexcept;
  std::optional<CohortRecord> find(uint64_t capture_id) const noexcept;

private:
  mutable std::mutex mutex_;
  std::map<uint64_t, CohortRecord> cohorts_;
};

} // namespace cambang
