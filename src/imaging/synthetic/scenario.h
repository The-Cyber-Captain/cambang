#pragma once

#include <cstdint>
#include <vector>

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

// Phase 2 (Timeline foundation): minimal in-memory scheduled-event model.
//
// JSON Scenario parsing is explicitly out of scope for now.
// Timeline role uses these events to drive the same lifecycle + frame emission
// paths as the Nominal role, but keyed to explicit scenario timestamps.

enum class SyntheticEventType : std::uint32_t {
  EmitFrame = 0,
  StartStream = 1,
  StopStream = 2,
  OpenDevice = 3,
  CloseDevice = 4,
  CreateStream = 5,
  DestroyStream = 6,
  UpdateStreamPicture = 7,
};

struct SyntheticScheduledEvent {
  std::uint64_t at_ns = 0;    // Scenario/virtual timestamp (ns) in SyntheticProvider's virtual clock domain.
  std::uint64_t seq = 0;      // Monotonic tie-breaker for stable ordering.
  SyntheticEventType type{};
  std::uint32_t endpoint_index = 0; // Used by OpenDevice for synthetic:<index>.
  std::uint64_t device_instance_id = 0;
  std::uint64_t root_id = 0;
  std::uint64_t stream_id = 0;
  bool has_picture = false;
  PictureConfig picture{};
};

struct SyntheticTimelineScenario {
  std::vector<SyntheticScheduledEvent> events;
};

} // namespace cambang