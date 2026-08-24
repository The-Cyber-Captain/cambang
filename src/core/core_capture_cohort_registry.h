// src/core/core_capture_cohort_registry.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/capture_admission_context.h"
#include "core/core_capture_assembly_registry.h"

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
    // Every member reached a terminal disposition, or the simultaneity window
    // expired. A CLOSED cohort's result set is final; an OPEN one's is a
    // snapshot of work still in progress, and before this state existed the
    // two were indistinguishable to any caller.
    CLOSED = 2,
  };

  // Why a cohort closed (capture_identity_and_lifecycle.md 4.4).
  enum class CohortClosedReason : uint8_t {
    NONE = 0,
    ALL_MEMBERS_TERMINAL = 1,
    WINDOW_EXPIRED = 2,
  };

  // How one member of a closed cohort ended, mirroring the assembly registry's
  // disposition so a closed cohort is self-describing without a second lookup.
  struct MemberOutcome {
    uint64_t device_instance_id = 0;
    std::string hardware_id;
    uint64_t device_capture_id = 0;
    CoreCaptureAssemblyRegistry::TerminalState disposition =
        CoreCaptureAssemblyRegistry::TerminalState::NONE;
    bool has_error_code = false;
    uint32_t error_code = 0;
  };

  enum class CohortFailurePhase : uint8_t {
    NONE = 0,
    SUBMISSION = 1,
    EXECUTION = 2,
  };

  struct Participant {
    uint64_t device_instance_id = 0;
    std::string hardware_id;
    // This member's own Device Capture Id. Distinct from the cohort's
    // rig_capture_id: a rig member is an ordinary Device Capture and is keyed
    // as one in the assembly, result and acquisition-session registries. Both
    // spaces are session-scoped uint64 minted at the Godot boundary; they are
    // NOT interchangeable, and nothing in the type system says so, so read the
    // field name at every call site.
    uint64_t device_capture_id = 0;
  };

  struct CohortRecord {
    // Rig Capture Id -- this registry's key. Never a Device Capture Id.
    uint64_t rig_capture_id = 0;
    uint64_t rig_id = 0;
    // The rig's membership version this cohort was admitted under
    // (capture_identity_and_lifecycle.md 5.2), so a rig capture is
    // self-describing about the membership that produced it. Taken from the
    // preflight that resolved the participants, never re-read at admission --
    // see RigPreflightResult::rig_membership_version for why.
    uint64_t rig_membership_version = 0;
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

    // Closure (section 4.4). admitted_ns is the simultaneity window's origin
    // and is Core's own clock, never an acquisition mark: camera_fact_model.md
    // 12.2 forbids acquisition timing as ordering or latency evidence, and
    // 12.1 notes Capture Date-Time is deliberately SHARED across one rig
    // capture, so marks from separate devices may legitimately be identical
    // and can decide nothing about membership, lateness or ordering.
    uint64_t admitted_ns = 0;
    CohortClosedReason closed_reason = CohortClosedReason::NONE;
    uint64_t closed_ns = 0;
    // Populated when the cohort closes: one entry per expected participant, in
    // membership order. A member that failed appears here WITH its error --
    // the absent-array-entry behaviour section 4.3 calls out is exactly what
    // this replaces.
    std::vector<MemberOutcome> member_outcomes;
  };

  void clear() noexcept;

  // Every id parameter below is a Rig Capture Id.
  bool insert(CohortRecord record);
  bool set_admission_context(uint64_t rig_capture_id, CaptureAdmissionContext context) noexcept;
  bool mark_failed(uint64_t rig_capture_id,
                   uint64_t failed_device_instance_id,
                   uint32_t failure_error_code,
                   CohortFailurePhase phase) noexcept;
  bool contains(uint64_t rig_capture_id) const noexcept;
  std::optional<CohortRecord> find(uint64_t rig_capture_id) const noexcept;

  // Close a cohort, recording why and how each member ended. Idempotent: a
  // cohort already CLOSED or FAILED is left alone and false is returned, so a
  // window sweep racing a final member's arrival cannot rewrite the outcome.
  bool close(uint64_t rig_capture_id,
             CohortClosedReason reason,
             uint64_t closed_ns,
             std::vector<MemberOutcome> member_outcomes) noexcept;

  // Whether this rig already has a capture in flight (an OPEN cohort).
  //
  // Scoped to the rig, NOT global. Section 5.5 gives a device at most one rig,
  // so cohorts never share participants and multiple rigs require no
  // arbitration against one another -- a global denial would block two
  // independent rigs from capturing at once for no reason.
  bool has_open_cohort_for_rig(uint64_t rig_id) const noexcept;

  // Cohorts that closed since the last drain (section 4.2). Same shape as the
  // assembly registry's completion queue, and for the same reason: the
  // boundary drains once per tick and emits, so a rig capture is reported
  // exactly once.
  struct ClosedCohort {
    uint64_t rig_capture_id = 0;
    uint64_t rig_id = 0;
    CohortClosedReason reason = CohortClosedReason::NONE;
  };
  std::vector<ClosedCohort> drain_closed_cohorts();

  // Rig Capture Ids of every cohort still OPEN. Returned by value so the
  // caller can resolve dispositions from the assembly registry without holding
  // this registry's lock -- the cross-registry ordering is documented as never
  // nested.
  std::vector<uint64_t> open_cohort_ids() const;

  // (Rig Capture Id, device_instance_id) for every member of a CLOSED cohort
  // recorded NEVER_ARRIVED -- the only members that can still become
  // LATE_EXCLUDED. Empty in the ordinary case where nothing was given up on.
  std::vector<std::pair<uint64_t, uint64_t>> closed_members_never_arrived() const;

  // Record that a member the cohort closed without (NEVER_ARRIVED) has since
  // reached a terminal disposition: it becomes LATE_EXCLUDED. Deliberately
  // narrow -- it upgrades ONLY from NEVER_ARRIVED, and never touches
  // closed_reason. A member settling after the window is not part of that
  // moment, but the cohort still closed for the reason it closed for, and
  // rewriting that would report a different capture than the one that
  // happened.
  bool mark_member_late_excluded(uint64_t rig_capture_id,
                                 uint64_t device_instance_id) noexcept;

  // Delay until the next OPEN cohort's window expires, for CoreThread's timer
  // deadline scheduling. Mirrors next_cohort_expiry_delay_ns(), which governs
  // retention rather than closure -- the two are separate windows and must not
  // be conflated.
  std::optional<uint64_t> next_window_expiry_delay_ns(uint64_t now_ns,
                                                      uint64_t window_ns) const;

  // Resolve a member's Device Capture Id from its cohort and device. Returns 0
  // when the cohort is unknown or the device is not one of its participants.
  uint64_t device_capture_id_for(uint64_t rig_capture_id,
                                 uint64_t device_instance_id) const noexcept;

  // Reverse direction, and the one the provider-fact path needs. A rig
  // member's facts arrive carrying that MEMBER's Device Capture Id, so a fact
  // can no longer be matched against a cohort key directly -- before the id
  // split it could, because member and cohort shared one id. Returns 0 when
  // the id belongs to no cohort, which is the ordinary answer for a standalone
  // device capture.
  uint64_t rig_capture_id_for_device_capture(uint64_t device_capture_id) const noexcept;

  // Cohort a Device Capture Id belongs to, or nullopt if it is not a member.
  std::optional<CohortRecord> find_by_device_capture_id(
      uint64_t device_capture_id) const noexcept;

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
  // Callers must already hold mutex_.
  uint64_t rig_capture_id_for_device_capture_locked_(uint64_t device_capture_id) const noexcept;

  mutable std::mutex mutex_;
  std::map<uint64_t, CohortRecord> cohorts_;
  // Member Device Capture Id -> owning Rig Capture Id. Maintained alongside
  // cohorts_ under the same lock; retirement removes both.
  std::map<uint64_t, uint64_t> rig_capture_id_by_device_capture_id_;
  std::vector<ClosedCohort> closed_pending_;
};

} // namespace cambang
