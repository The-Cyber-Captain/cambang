#pragma once

#include <cstdint>

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
};

struct SyntheticScheduledEvent {
  std::uint64_t at_ns = 0;    // Scenario/virtual timestamp (ns) in SyntheticProvider's virtual clock domain.
  std::uint64_t seq = 0;      // Monotonic tie-breaker for stable ordering.
  SyntheticEventType type{};
  std::uint64_t stream_id = 0;
};

} // namespace cambang
