// src/core/core_rig_registry.cpp

#include "core/core_rig_registry.h"

#include <algorithm>

namespace cambang {

bool CoreRigRegistry::record_capture_triggered(uint64_t rig_id, uint64_t rig_capture_id) {
  if (rig_id == 0 || rig_capture_id == 0) {
    return false;
  }
  const auto it = rigs_.find(rig_id);
  if (it == rigs_.end()) {
    return false;
  }
  ++it->second.captures_triggered;
  it->second.active_capture_id = rig_capture_id;
  return true;
}

bool CoreRigRegistry::record_capture_finished(uint64_t rig_id,
                                             uint64_t rig_capture_id,
                                             bool failed) {
  if (rig_id == 0 || rig_capture_id == 0) {
    return false;
  }
  const auto it = rigs_.find(rig_id);
  if (it == rigs_.end()) {
    return false;
  }
  if (failed) {
    ++it->second.captures_failed;
  } else {
    ++it->second.captures_completed;
  }
  it->second.last_capture_id = rig_capture_id;
  // Only clear the active id if this is the capture that was active: a later
  // trigger may already have taken the slot while this one was settling.
  if (it->second.active_capture_id == rig_capture_id) {
    it->second.active_capture_id = 0;
  }
  return true;
}

bool CoreRigRegistry::retain_capture_profile(uint64_t rig_id,
                                             uint32_t width,
                                             uint32_t height,
                                             uint32_t format,
                                             uint64_t capture_profile_version) {
  if (rig_id == 0) {
    return false;
  }

  auto& rec = rigs_[rig_id];
  rec.rig_id = rig_id;
  rec.capture_width = width;
  rec.capture_height = height;
  rec.capture_format = format;
  rec.capture_profile_version = capture_profile_version;
  rec.live = true;
  return true;
}

bool CoreRigRegistry::retain_member_hardware_ids(uint64_t rig_id, std::vector<std::string> member_hardware_ids) {
  if (rig_id == 0) {
    return false;
  }
  auto& rec = rigs_[rig_id];
  rec.rig_id = rig_id;
  // Version only on a real change. Re-retaining the same membership is a
  // no-op, not a version event: a caller that re-asserts its configuration
  // every frame would otherwise manufacture a history of changes that never
  // happened, and a cohort's recorded version would stop meaning anything.
  if (rec.rig_membership_version == 0 ||
      rec.member_hardware_ids != member_hardware_ids) {
    ++rec.rig_membership_version;
    rec.member_hardware_ids = std::move(member_hardware_ids);
  }
  rec.live = true;
  return true;
}

uint64_t CoreRigRegistry::rig_owning_member(const std::string& hardware_id,
                                            uint64_t excluding_rig_id) const noexcept {
  if (hardware_id.empty()) {
    return 0;
  }
  for (const auto& [rig_id, rec] : rigs_) {
    if (rig_id == excluding_rig_id) {
      continue;
    }
    if (std::find(rec.member_hardware_ids.begin(), rec.member_hardware_ids.end(),
                  hardware_id) != rec.member_hardware_ids.end()) {
      return rig_id;
    }
  }
  return 0;
}

CoreRigRegistry::MembershipChange CoreRigRegistry::add_member(
    uint64_t rig_id, const std::string& hardware_id) {
  if (rig_id == 0 || hardware_id.empty()) {
    return MembershipChange::RigNotFound;
  }
  const auto it = rigs_.find(rig_id);
  if (it == rigs_.end()) {
    return MembershipChange::RigNotFound;
  }
  auto& members = it->second.member_hardware_ids;
  // Idempotent: adding a device that is already a member is not a change, and
  // must not bump the version. Membership is a set the caller declares, not a
  // sequence of operations to replay.
  if (std::find(members.begin(), members.end(), hardware_id) != members.end()) {
    return MembershipChange::NoChange;
  }
  // Section 5.5. Checked after the idempotence test above, so re-adding a
  // device to the rig it is already in stays a no-op rather than tripping
  // this.
  if (rig_owning_member(hardware_id, rig_id) != 0) {
    return MembershipChange::AlreadyInAnotherRig;
  }
  members.push_back(hardware_id);
  ++it->second.rig_membership_version;
  return MembershipChange::Changed;
}

CoreRigRegistry::MembershipChange CoreRigRegistry::remove_member(
    uint64_t rig_id, const std::string& hardware_id) {
  if (rig_id == 0 || hardware_id.empty()) {
    return MembershipChange::RigNotFound;
  }
  const auto it = rigs_.find(rig_id);
  if (it == rigs_.end()) {
    return MembershipChange::RigNotFound;
  }
  auto& members = it->second.member_hardware_ids;
  const auto found = std::find(members.begin(), members.end(), hardware_id);
  // Removing a non-member is idempotent too: the caller's declared end state
  // is already true.
  if (found == members.end()) {
    return MembershipChange::NoChange;
  }
  // A rig with no members cannot preflight (EmptyMembership) and cannot be
  // triggered, so emptying it by removal would leave an object that exists but
  // can never do anything. Refused rather than silently allowed; create_rig
  // already requires two or more members.
  if (members.size() <= 1) {
    return MembershipChange::WouldEmptyRig;
  }
  members.erase(found);
  ++it->second.rig_membership_version;
  return MembershipChange::Changed;
}

const CoreRigRegistry::RigRecord* CoreRigRegistry::find(uint64_t rig_id) const noexcept {
  const auto it = rigs_.find(rig_id);
  if (it == rigs_.end()) {
    return nullptr;
  }
  return &it->second;
}

} // namespace cambang
