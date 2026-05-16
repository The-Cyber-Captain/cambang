// src/core/core_capture_cohort_registry.h
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cambang {

// Internal maintainer/Core terminology:
// A "cohort" is an admitted rig-triggered capture group (one capture_id, one
// rig_id, and fixed expected participant devices). This registry intentionally
// stores admission metadata only (no payload ownership).
class CoreCaptureCohortRegistry final {
public:
  struct Participant {
    uint64_t device_instance_id = 0;
    std::string hardware_id;
  };

  struct CohortRecord {
    uint64_t capture_id = 0;
    uint64_t rig_id = 0;
    std::vector<Participant> expected_participants;
  };

  void clear() noexcept { cohorts_.clear(); }

  bool insert(CohortRecord record);
  bool contains(uint64_t capture_id) const noexcept;
  const CohortRecord* find(uint64_t capture_id) const noexcept;
  const std::map<uint64_t, CohortRecord>& all() const noexcept { return cohorts_; }

private:
  std::map<uint64_t, CohortRecord> cohorts_;
};

} // namespace cambang
