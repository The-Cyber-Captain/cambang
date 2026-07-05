// src/core/core_capture_cohort_registry.cpp

#include "core/core_capture_cohort_registry.h"

namespace cambang {

void CoreCaptureCohortRegistry::clear() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  cohorts_.clear();
}

bool CoreCaptureCohortRegistry::insert(CohortRecord record) {
  if (record.capture_id == 0 || record.rig_id == 0 || record.expected_participants.empty()) {
    return false;
  }
  record.state = CohortState::OPEN;
  record.failure_phase = CohortFailurePhase::NONE;
  record.failed_device_instance_id = 0;
  record.failure_error_code = 0;
  record.has_failure_error_code = false;
  std::lock_guard<std::mutex> lock(mutex_);
  auto [it, inserted] = cohorts_.emplace(record.capture_id, std::move(record));
  (void)it;
  return inserted;
}

bool CoreCaptureCohortRegistry::mark_failed(uint64_t capture_id,
                                            uint64_t failed_device_instance_id,
                                            uint32_t failure_error_code,
                                            CohortFailurePhase phase) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = cohorts_.find(capture_id);
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

bool CoreCaptureCohortRegistry::contains(uint64_t capture_id) const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return cohorts_.find(capture_id) != cohorts_.end();
}

std::optional<CoreCaptureCohortRegistry::CohortRecord> CoreCaptureCohortRegistry::find(
    uint64_t capture_id) const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = cohorts_.find(capture_id);
  if (it == cohorts_.end()) {
    return std::nullopt;
  }
  return it->second;
}

} // namespace cambang
