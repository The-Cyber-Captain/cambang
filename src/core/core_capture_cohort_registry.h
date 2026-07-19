// src/core/core_capture_cohort_registry.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/capture_admission_context.h"

namespace cambang {

// Internal maintainer/Core terminology:
// A "cohort" is an admitted rig-triggered capture group (one capture_id, one
// rig_id, and fixed expected participant devices). This registry intentionally
// stores admission metadata only (no payload ownership).
//
// Deliberately self-locking, unlike most CoreRuntime registries (see the
// threading-model note on CoreResultStore): find() is read directly from
// CoreRuntime::get_capture_result_set() on the calling (e.g. Godot) thread
// without a core-thread round trip.
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
    bool has_admission_context = false;
    CaptureAdmissionContext admission_context{};
    std::vector<Participant> expected_participants;
    CohortState state = CohortState::OPEN;
    CohortFailurePhase failure_phase = CohortFailurePhase::NONE;
    uint64_t failed_device_instance_id = 0;
    uint32_t failure_error_code = 0;
    bool has_failure_error_code = false;
    // Core-monotonic creation timestamp (CoreRuntime::ns_since_epoch_()),
    // set by the caller before insert(); drives retire_expired_cohorts()
    // (ledger #52). Not reset by insert().
    uint64_t created_ns = 0;
  };

  void clear() noexcept;

  bool insert(CohortRecord record);
  bool set_admission_context(uint64_t capture_id, CaptureAdmissionContext context) noexcept;
  bool mark_failed(uint64_t capture_id,
                   uint64_t failed_device_instance_id,
                   uint32_t failure_error_code,
                   CohortFailurePhase phase) noexcept;
  bool contains(uint64_t capture_id) const noexcept;
  std::optional<CohortRecord> find(uint64_t capture_id) const noexcept;

  // Retention (ledger #52): this registry holds no payload/image data (see
  // class doc comment), so unlike CoreCaptureAssemblyRegistry/CoreResultStore
  // it doesn't need supersession/close-driven retirement -- a flat, generous
  // time-since-creation window is sufficient and simpler. Safe even for a
  // cohort whose participants are still resolving: get_capture_result_set()'s
  // non-cohort fallback path independently recovers any already-completed
  // participant's result directly from CoreResultStore/CoreCaptureAssemblyRegistry
  // once the cohort record itself is gone.
  size_t retire_expired_cohorts(uint64_t now_ns, uint64_t retention_window_ns);
  std::optional<uint64_t> next_cohort_expiry_delay_ns(
      uint64_t now_ns, uint64_t retention_window_ns) const;

private:
  mutable std::mutex mutex_;
  std::map<uint64_t, CohortRecord> cohorts_;
};

} // namespace cambang
