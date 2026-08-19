#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
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
  // Per-member terminal disposition (capture_identity_and_lifecycle.md 4.3).
  // A member capture must always reach one of these; it must never simply
  // disappear from a cohort.
  //
  // The full vocabulary is defined here so the shape settles once, but two
  // values have NO PRODUCING PATH yet and must not be faked into existence:
  //   PREEMPTED_BY_RIG  needs rig-preempts-member arbitration (section 3).
  //   DEVICE_LOST       needs membership lifecycle (section 5.3).
  // A disposition nothing can produce is honest; one produced by guessing is
  // not. capture_completion_verify asserts both remain unreachable.
  enum class TerminalState : uint8_t {
    NONE = 0,
    // Settled with its payload. The old COMPLETED.
    DELIVERED = 1,
    // Settled with a provider error, which travels with it. The old FAILED.
    FAILED = 2,
    // Terminal, but outside its cohort's simultaneity window, so not part of
    // the same moment. Correct behaviour, not a failure to wait (section 4.4).
    LATE_EXCLUDED = 3,
    // A rig capture preempted this device's in-flight standalone capture.
    PREEMPTED_BY_RIG = 4,
    // The device was disengaged, closed or otherwise lost while this member
    // was in flight. A resource event, not a configuration change.
    DEVICE_LOST = 5,
    // The cohort closed and this member produced nothing at all -- no payload
    // and no error. Distinct from FAILED, which knows why.
    NEVER_ARRIVED = 6,
  };

  // Whether a disposition means the member finished with its payload intact.
  // Everything else is terminal without a usable image.
  static constexpr bool disposition_delivered(TerminalState s) noexcept {
    return s == TerminalState::DELIVERED;
  }
  static constexpr bool disposition_is_terminal(TerminalState s) noexcept {
    return s != TerminalState::NONE;
  }

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

  // A member's disposition and, where it has one, the error that travels with
  // it. Deliberately NOT smoke-gated like find_for_smoke(): cohort closure
  // needs this in shipping builds, and section 4.3 requires the error code to
  // reach the caller rather than being reduced to an absent entry.
  // An unknown (capture_id, device) is NONE, not an error -- a device admitted
  // but not yet reporting is exactly the non-terminal case.
  struct MemberDisposition {
    TerminalState state = TerminalState::NONE;
    bool has_error_code = false;
    uint32_t error_code = 0;
  };
  MemberDisposition disposition_for(uint64_t capture_id,
                                    uint64_t device_instance_id) const;

  // Captures that reached a terminal disposition since the last drain
  // (capture_identity_and_lifecycle.md 4.2). Queued at every transition site
  // rather than discovered by scanning: the boundary drains this once per tick
  // and emits, so the cost is proportional to what actually changed, and a
  // capture is reported exactly once no matter how it finished.
  struct FinishedCapture {
    uint64_t capture_id = 0;
    uint64_t device_instance_id = 0;
    TerminalState disposition = TerminalState::NONE;
    bool has_error_code = false;
    uint32_t error_code = 0;
  };
  std::vector<FinishedCapture> drain_finished_captures();

  // Whether this device has a capture Core has ADMITTED that has not reached a
  // terminal disposition. This is the per-device single-capture rule's source
  // of truth, and it is deliberately not
  // CoreAcquisitionSessionRegistry::has_capture_in_flight_for_device(), which
  // is populated from provider-reported capture_started facts.
  //
  // The difference is a real window, not a nicety: between Core admitting a
  // capture and the provider acknowledging it, the session registry knows
  // nothing, so a guard built on it would admit a second capture on a device
  // that already has one -- precisely under the rapid double-trigger the rule
  // exists to refuse. Admission is Core's own act and is known immediately.
  bool has_admitted_non_terminal_capture_for_device(uint64_t device_instance_id) const;

  // The Device Capture Id of this device's admitted, non-terminal capture, or
  // 0. Companion to the predicate above, for the caller that must act on the
  // capture rather than merely refuse because of it -- rig preemption needs to
  // name what it is displacing.
  uint64_t admitted_non_terminal_capture_id_for_device(uint64_t device_instance_id) const;

  // Terminalise a capture as displaced by a rig capture (section 3). Distinct
  // from mark_capture_failed: the capture did not fail, it lost arbitration,
  // and its subscriber is owed that distinction rather than a generic error.
  // Preemption must never be silent.
  void mark_capture_preempted_by_rig(uint64_t capture_id, uint64_t device_instance_id);

  // Terminalise a capture whose device was closed, disengaged or otherwise
  // lost while it was in flight (capture_identity_and_lifecycle.md 5.3).
  //
  // A resource event, not a failure and not a configuration change, so it
  // carries no error code. Distinct from the admission watchdog, which would
  // eventually report the same capture as FAILED(ERR_TIMEOUT) 30s later --
  // that is a different claim: the capture did not run out of time, its
  // device went away. A member capture must always reach a terminal
  // disposition; it must never simply vanish from a cohort.
  void mark_capture_device_lost(uint64_t capture_id, uint64_t device_instance_id);

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

  // Retention (unbounded-growth fix, ledger #52). Deliberately time-based
  // only, not tied to any other system event. Two earlier attempts at this
  // were each falsified by real, deliberate provider_compliance_verify
  // checks: (1) retiring a device's assembly the moment that same device was
  // admitted into a newer capture broke a check that reports a retained-
  // to-image observation for an older, already-superseded capture on the
  // same device; (2) retiring on device close broke a check that reports
  // such an observation for a capture on an *already-closed* device (Core's
  // own capture_retained_plan_evaluators_ orphan-retention mechanism already
  // keeps evaluator state alive across close for exactly this reason -- see
  // try_close_device()'s retain_capture_orphans handling). Core's
  // result-access-cost calibration feedback loop
  // (CoreRuntime::handle_capture_retained_to_image_observation_) can
  // legitimately reference an old capture regardless of newer captures or
  // device close, so neither event is a safe retirement signal -- only
  // elapsed time is. This does not affect any already-issued Godot wrapper
  // (CamBANGCaptureResult holds its own independent SharedCaptureResultData
  // reference) -- only future lookups of the retired capture_id are
  // affected.
  struct RetiredAssembly {
    uint64_t capture_id = 0;
    uint64_t device_instance_id = 0;
  };

  // Retires every assembly that has already reached a terminal state
  // (COMPLETED or FAILED -- never a still-pending NONE entry; those are
  // solely the capture-admission watchdog's concern) and is older than
  // admitted_ns + retention_window_ns. Returns the retired (capture_id,
  // device_instance_id) pairs so the caller can also retire the
  // corresponding CoreResultStore entries.
  std::vector<RetiredAssembly> retire_terminal_older_than(
      uint64_t now_ns, uint64_t retention_window_ns);
  std::optional<uint64_t> next_terminal_retirement_delay_ns(
      uint64_t now_ns, uint64_t retention_window_ns) const;

  // Single-entry counterpart to retire_terminal_older_than(), for the reverse
  // propagation direction: CoreResultStore::evict_over_byte_budget() (ledger
  // #53) decides which (capture_id, device_instance_id) entries to drop based
  // on retained-result byte size, and CoreRuntime uses this to keep assembly
  // bookkeeping consistent with that decision.
  void remove_assembly(uint64_t capture_id, uint64_t device_instance_id);

  // Queried by CoreRuntime to build the is_evictable predicate it passes to
  // CoreResultStore::evict_over_byte_budget() (ledger #53): only entries
  // already in a terminal state (COMPLETED or FAILED) may be evicted for
  // size, for the same reason terminal state gates
  // retire_terminal_older_than() -- a still-pending (NONE) assembly must
  // never have its result data dropped out from under it (that data is still
  // being actively appended to / finalized by the capture pipeline itself,
  // not merely retained for later lookup).
  //
  // Returns a snapshot (not a live predicate) so CoreRuntime can take this
  // registry's lock, copy out the answer, and release it BEFORE ever taking
  // CoreResultStore::mutex_ for the eviction walk. Every other cross-registry
  // access in this codebase (e.g. get_capture_result_set()'s assembly-then-
  // result reads) locks the two registries sequentially, never nested; a
  // live per-candidate predicate here would instead nest
  // CoreCaptureAssemblyRegistry::mutex_ inside CoreResultStore::mutex_,
  // the one cross-registry lock order this codebase does not otherwise use.
  // The snapshot cannot go stale between being taken and the eviction walk
  // that consults it: terminal_state only ever moves one way (NONE ->
  // COMPLETED/FAILED), both this snapshot and the eviction it feeds run
  // synchronously within the same core-thread timer tick with no
  // interleaving work, and only the core thread ever mutates terminal_state.
  std::vector<std::pair<uint64_t, uint64_t>> terminal_capture_device_pairs() const;

  void clear();

#if defined(CAMBANG_INTERNAL_SMOKE)
  std::optional<DeviceCaptureAssembly> find_for_smoke(uint64_t capture_id,
                                                      uint64_t device_instance_id) const;
#endif

private:
  // Callers must already hold mutex_. Queues a completion exactly once, at the
  // moment the disposition becomes terminal.
  void note_finished_locked_(const DeviceCaptureAssembly& assembly);

  mutable std::mutex mutex_;
  std::map<uint64_t, std::map<uint64_t, DeviceCaptureAssembly>> assemblies_by_capture_id_;
  std::vector<FinishedCapture> finished_pending_;
};

} // namespace cambang
