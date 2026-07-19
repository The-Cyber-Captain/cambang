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

bool CoreCaptureCohortRegistry::set_admission_context(
    uint64_t capture_id, CaptureAdmissionContext context) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = cohorts_.find(capture_id);
  if (it == cohorts_.end() || it->second.has_admission_context) {
    return false;
  }
  it->second.admission_context = std::move(context);
  it->second.has_admission_context = true;
  return true;
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

size_t CoreCaptureCohortRegistry::retire_expired_cohorts(
    uint64_t now_ns, uint64_t retention_window_ns) {
  size_t retired = 0;
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = cohorts_.begin(); it != cohorts_.end();) {
    if (now_ns < it->second.created_ns + retention_window_ns) {
      ++it;
      continue;
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
  for (const auto& [capture_id, record] : cohorts_) {
    (void)capture_id;
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
