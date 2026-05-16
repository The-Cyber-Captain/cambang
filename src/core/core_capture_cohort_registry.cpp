// src/core/core_capture_cohort_registry.cpp

#include "core/core_capture_cohort_registry.h"

namespace cambang {

bool CoreCaptureCohortRegistry::insert(CohortRecord record) {
  if (record.capture_id == 0 || record.rig_id == 0 || record.expected_participants.empty()) {
    return false;
  }
  auto [it, inserted] = cohorts_.emplace(record.capture_id, std::move(record));
  (void)it;
  return inserted;
}

bool CoreCaptureCohortRegistry::contains(uint64_t capture_id) const noexcept {
  return cohorts_.find(capture_id) != cohorts_.end();
}

const CoreCaptureCohortRegistry::CohortRecord* CoreCaptureCohortRegistry::find(uint64_t capture_id) const noexcept {
  const auto it = cohorts_.find(capture_id);
  if (it == cohorts_.end()) {
    return nullptr;
  }
  return &it->second;
}

} // namespace cambang
