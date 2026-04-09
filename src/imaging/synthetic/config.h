#pragma once

#include <cstdint>

#include "imaging/synthetic/scenario.h"
#include "pixels/pattern/pattern_registry.h"

namespace cambang {

// Locked synthetic roles.
enum class SyntheticRole : std::uint8_t {
  Nominal = 0,
  Timeline = 1,
};

// Timing driver for synthetic provider.
// First landing implements VirtualTime only.
enum class TimingDriver : std::uint8_t {
  RealTime = 0,
  VirtualTime = 1,
};

// Reconciliation policy for clustered destructive timeline actions in
// synthetic timeline / virtual_time mode.
enum class TimelineReconciliation : std::uint8_t {
  CompletionGated = 0,
  Strict = 1,
};

struct SyntheticNominalDefaults {
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t format_fourcc = 0; // 0 => FOURCC_RGBA default inside provider

  uint32_t fps_num = 30;
  uint32_t fps_den = 1;

  uint64_t start_stream_warmup_ns = 0;
};

struct SyntheticPatternDefaults {
  // Canonical preset selection (zero-indexed enum).
  PatternPreset preset = PatternPreset::XyXor;
  uint64_t seed = 1;
  bool overlay_frame_index = true;
  bool overlay_timestamp = true;
  bool overlay_stream_id = true;
};

struct SyntheticProviderConfig {
  SyntheticRole synthetic_role = SyntheticRole::Nominal;
  TimingDriver timing_driver = TimingDriver::VirtualTime;
  TimelineReconciliation timeline_reconciliation = TimelineReconciliation::CompletionGated;

  uint32_t endpoint_count = 2;

  SyntheticNominalDefaults nominal{};
  SyntheticPatternDefaults pattern{};
  SyntheticTimelineScenario timeline_scenario{};
};

} // namespace cambang
