// src/core/core_capture_cohort_registry.cpp

#include "core/core_capture_cohort_registry.h"

namespace cambang {

void CoreCaptureCohortRegistry::clear() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  cohorts_.clear();
  rig_capture_id_by_device_capture_id_.clear();
}

bool CoreCaptureCohortRegistry::insert(CohortRecord record) {
  if (record.rig_capture_id == 0 || record.rig_id == 0 || record.expected_participants.empty()) {
    return false;
  }
  // Two ways a member id can collapse back into the shared-id behaviour this
  // registry exists to forbid, and both are rejected here rather than trusted
  // to the caller: no id of its own, or the cohort's own id reused as a
  // member's. The second is the exact signature of a single-counter
  // regression, and rejecting it is what makes that regression catchable
  // host-native -- the boundary that mints these is Godot-side and cannot be
  // driven by the maintainer verifiers.
  for (const auto& participant : record.expected_participants) {
    if (participant.device_capture_id == 0 ||
        participant.device_capture_id == record.rig_capture_id) {
      return false;
    }
  }
  // Members must also be distinct from each other: two members sharing one id
  // is the same collision one level down.
  for (size_t i = 0; i < record.expected_participants.size(); ++i) {
    for (size_t j = i + 1; j < record.expected_participants.size(); ++j) {
      if (record.expected_participants[i].device_capture_id ==
          record.expected_participants[j].device_capture_id) {
        return false;
      }
    }
  }
  record.state = CohortState::OPEN;
  record.failure_phase = CohortFailurePhase::NONE;
  record.failed_device_instance_id = 0;
  record.failure_error_code = 0;
  record.has_failure_error_code = false;
  std::lock_guard<std::mutex> lock(mutex_);
  // A member id already owned by another live cohort would make the reverse
  // index ambiguous, which is the collision this split exists to remove.
  for (const auto& participant : record.expected_participants) {
    if (rig_capture_id_by_device_capture_id_.count(participant.device_capture_id) != 0) {
      return false;
    }
  }
  const uint64_t rig_capture_id = record.rig_capture_id;
  auto [it, inserted] = cohorts_.emplace(rig_capture_id, std::move(record));
  if (!inserted) {
    return false;
  }
  for (const auto& participant : it->second.expected_participants) {
    rig_capture_id_by_device_capture_id_[participant.device_capture_id] = rig_capture_id;
  }
  return true;
}

bool CoreCaptureCohortRegistry::set_admission_context(
    uint64_t rig_capture_id, CaptureAdmissionContext context) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = cohorts_.find(rig_capture_id);
  if (it == cohorts_.end() || it->second.has_admission_context) {
    return false;
  }
  it->second.admission_context = std::move(context);
  it->second.has_admission_context = true;
  return true;
}

bool CoreCaptureCohortRegistry::mark_failed(uint64_t rig_capture_id,
                                            uint64_t failed_device_instance_id,
                                            uint32_t failure_error_code,
                                            CohortFailurePhase phase) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = cohorts_.find(rig_capture_id);
  if (it == cohorts_.end()) {
    return false;
  }
  it->second.state = CohortState::FAILED;
  it->second.failed_device_instance_id = failed_device_instance_id;
  it->second.failure_phase = phase;
  it->second.failure_error_code = failure_error_code;
  it->second.has_failure_error_code = (failure_error_code != 0);
  return true;
}

bool CoreCaptureCohortRegistry::contains(uint64_t rig_capture_id) const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return cohorts_.find(rig_capture_id) != cohorts_.end();
}

std::optional<CoreCaptureCohortRegistry::CohortRecord> CoreCaptureCohortRegistry::find(
    uint64_t rig_capture_id) const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = cohorts_.find(rig_capture_id);
  if (it == cohorts_.end()) {
    return std::nullopt;
  }
  return it->second;
}

uint64_t CoreCaptureCohortRegistry::rig_capture_id_for_device_capture_locked_(
    uint64_t device_capture_id) const noexcept {
  const auto it = rig_capture_id_by_device_capture_id_.find(device_capture_id);
  return it == rig_capture_id_by_device_capture_id_.end() ? 0 : it->second;
}

uint64_t CoreCaptureCohortRegistry::rig_capture_id_for_device_capture(
    uint64_t device_capture_id) const noexcept {
  if (device_capture_id == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return rig_capture_id_for_device_capture_locked_(device_capture_id);
}

std::optional<CoreCaptureCohortRegistry::CohortRecord>
CoreCaptureCohortRegistry::find_by_device_capture_id(
    uint64_t device_capture_id) const noexcept {
  if (device_capture_id == 0) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t rig_capture_id =
      rig_capture_id_for_device_capture_locked_(device_capture_id);
  if (rig_capture_id == 0) {
    return std::nullopt;
  }
  const auto it = cohorts_.find(rig_capture_id);
  if (it == cohorts_.end()) {
    return std::nullopt;
  }
  return it->second;
}

uint64_t CoreCaptureCohortRegistry::device_capture_id_for(
    uint64_t rig_capture_id, uint64_t device_instance_id) const noexcept {
  if (rig_capture_id == 0 || device_instance_id == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = cohorts_.find(rig_capture_id);
  if (it == cohorts_.end()) {
    return 0;
  }
  for (const auto& participant : it->second.expected_participants) {
    if (participant.device_instance_id == device_instance_id) {
      return participant.device_capture_id;
    }
  }
  return 0;
}

size_t CoreCaptureCohortRegistry::retire_expired_cohorts(
    uint64_t now_ns, uint64_t retention_window_ns) {
  size_t retired = 0;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = cohorts_.begin(); it != cohorts_.end();) {
    if (now_ns < it->second.created_ns + retention_window_ns) {
      ++it;
      continue;
    }
    for (const auto& participant : it->second.expected_participants) {
      rig_capture_id_by_device_capture_id_.erase(participant.device_capture_id);
    }
    it = cohorts_.erase(it);
    ++retired;
  }
  return retired;
}

std::optional<uint64_t> CoreCaptureCohortRegistry::next_cohort_expiry_delay_ns(
    uint64_t now_ns, uint64_t retention_window_ns) const {
  std::optional<uint64_t> min_delay;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [rig_capture_id, record] : cohorts_) {
    (void)rig_capture_id;
    const uint64_t expiry_ns = record.created_ns + retention_window_ns;
    uint64_t delay = 0;
    if (expiry_ns > now_ns) {
      delay = expiry_ns - now_ns;
    }
    if (!min_delay.has_value() || delay < *min_delay) {
      min_delay = delay;
    }
  }
  return min_delay;
}

} // namespace cambang
