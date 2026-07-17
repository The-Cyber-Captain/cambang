#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "core/capture_admission_context.h"
#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

// Deliberately self-locking, unlike most CoreRuntime registries (see the
// threading-model note on CoreResultStore): is_assembly_successful() and
// is_result_safe() are read directly from CoreRuntime::get_capture_result_set()
// on the calling (e.g. Godot) thread without a core-thread round trip.
class CoreCaptureAssemblyRegistry final {
public:
  enum class TerminalState : uint8_t {
    NONE = 0,
    COMPLETED = 1,
    FAILED = 2,
  };

  struct DeviceCaptureAssembly {
    uint64_t capture_id = 0;
    uint64_t device_instance_id = 0;
    bool has_admission_context = false;
    CaptureAdmissionContext admission_context{};
    std::vector<uint32_t> expected_image_member_indices{};
    bool has_default_image_retained = false;
    TerminalState terminal_state = TerminalState::NONE;
    bool has_failure_error_code = false;
    uint32_t failure_error_code = 0;
    // Core-monotonic timestamp (CoreRuntime::ns_since_epoch_()) when this
    // device was admitted; drives the capture-admission watchdog (see
    // sweep_admission_timeouts()). Not a wall-clock/EXIF timestamp -- see
    // admission_context.capture_date_time for that.
    uint64_t admitted_ns = 0;
  };

  struct TimedOutAssembly {
    uint64_t capture_id = 0;
    uint64_t device_instance_id = 0;
  };

  void mark_default_image_retained(uint64_t capture_id, uint64_t device_instance_id);
  void record_admission_context(uint64_t capture_id, uint64_t device_instance_id,
                                CaptureAdmissionContext context,
                                const CaptureStillImageBundle& still_image_bundle,
                                uint64_t admitted_ns);
  bool has_admitted_capture_member(uint64_t capture_id,
                                   uint64_t device_instance_id,
                                   uint32_t image_member_index) const;
  std::optional<CaptureAdmissionContext> admission_context_for(
      uint64_t capture_id, uint64_t device_instance_id) const;
  void mark_capture_completed(uint64_t capture_id, uint64_t device_instance_id);
  void mark_capture_failed(uint64_t capture_id, uint64_t device_instance_id, uint32_t error_code);
  bool is_assembly_successful(uint64_t capture_id, uint64_t device_instance_id) const;
  bool is_result_safe(uint64_t capture_id, uint64_t device_instance_id) const;

  // Capture-admission watchdog (icamera_provider.h's
  // capture_admission_watchdog_timeout_ns() contract): finds every device
  // assembly that is admitted (has_admission_context) but still non-terminal
  // (terminal_state == NONE) and past admitted_ns + timeout_ns, transitions
  // each to FAILED with ProviderError::ERR_TIMEOUT, and returns them. Scoped
  // strictly per-device: does not touch sibling devices in the same capture,
  // and callers must not use this to fail a whole cohort.
  std::vector<TimedOutAssembly> sweep_admission_timeouts(uint64_t now_ns, uint64_t timeout_ns);

  // Delay until the next admission would time out, for CoreThread's timer
  // deadline scheduling (mirrors CoreNativeObjectRegistry::next_retirement_delay_ns).
  std::optional<uint64_t> next_admission_timeout_delay_ns(uint64_t now_ns, uint64_t timeout_ns) const;

  void clear();

#if defined(CAMBANG_INTERNAL_SMOKE)
  std::optional<DeviceCaptureAssembly> find_for_smoke(uint64_t capture_id,
                                                      uint64_t device_instance_id) const;
#endif

private:
  mutable std::mutex mutex_;
  std::map<uint64_t, std::map<uint64_t, DeviceCaptureAssembly>> assemblies_by_capture_id_;
};

} // namespace cambang
