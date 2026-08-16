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

    // Bumped whenever member_hardware_ids actually changes
    // (capture_identity_and_lifecycle.md 5.2). Membership is declarative
    // configuration: accepted while live, versioned forward, applied from the
    // next trigger. The version is what makes that transition observable, and
    // what lets a stored result set describe the membership that produced it.
    //
    // Starts at 1 once membership is first retained, so 0 means "never set"
    // and is distinguishable from "set once". Bumped only on a real change --
    // retaining the same members again is not a version event, or a caller
    // re-asserting its configuration each frame would invent history.
    uint64_t rig_membership_version = 0;

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

  // Rig capture accounting. Before these existed, active_capture_id,
  // last_capture_id and the three counters were declared, projected into the
  // snapshot, and never written -- so a rig reported 0 captures after a
  // successful one, and a consumer could not tell "none yet" from "never
  // recorded". Ids here are Rig Capture Ids.
  bool record_capture_triggered(uint64_t rig_id, uint64_t rig_capture_id);
  // A cohort reaching a terminal outcome. `failed` distinguishes a cohort that
  // failed outright (submission/execution) from one that closed having run;
  // a closed cohort counts as completed even if individual members did not
  // deliver, because the rig capture itself ran to a truthful conclusion.
  bool record_capture_settled(uint64_t rig_id, uint64_t rig_capture_id, bool failed);

  bool retain_capture_profile(uint64_t rig_id,
                              uint32_t width,
                              uint32_t height,
                              uint32_t format,
                              uint64_t capture_profile_version);
  bool retain_member_hardware_ids(uint64_t rig_id, std::vector<std::string> member_hardware_ids);

  // Membership mutation (capture_identity_and_lifecycle.md 5.1). Declarative
  // configuration: accepted while live, versioned forward, applied from the
  // next trigger. Never touches a cohort already in flight -- a cohort
  // snapshots its participants at admission.
  enum class MembershipChange : uint8_t {
    Changed = 0,      // membership differs now; version bumped
    NoChange = 1,     // already in this state; idempotent, version untouched
    RigNotFound = 2,
    WouldEmptyRig = 3,  // removal refused: a rig needs at least one member
    AlreadyInAnotherRig = 4,  // section 5.5: a device belongs to at most one rig
  };

  // The rig that already has this device as a member, ignoring `excluding_rig_id`,
  // or 0 if none (capture_identity_and_lifecycle.md 5.5).
  //
  // This is load-bearing beyond tidiness: tranche 3 scopes its second-rig
  // denial per rig *because* cohorts cannot share participants. If a device
  // could sit in two rigs, two cohorts could contend for it and that denial
  // would be looking at the wrong rig.
  uint64_t rig_owning_member(const std::string& hardware_id,
                             uint64_t excluding_rig_id) const noexcept;
  MembershipChange add_member(uint64_t rig_id, const std::string& hardware_id);
  MembershipChange remove_member(uint64_t rig_id, const std::string& hardware_id);

  void clear() noexcept { rigs_.clear(); }

  const RigRecord* find(uint64_t rig_id) const noexcept;
  const std::map<uint64_t, RigRecord>& all() const noexcept { return rigs_; }

private:
  std::map<uint64_t, RigRecord> rigs_;
};

} // namespace cambang
