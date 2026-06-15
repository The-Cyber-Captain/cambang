#pragma once

#include <cstdint>
#include <map>
#include <string_view>

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

// Internal production/retention output-form mode for SyntheticProvider.
// This controls truthful Synthetic-produced stream and still-capture backing
// behavior; it is not a public API surface. Auto is the explicit
// runtime_default/no-forcing selection: keep Synthetic on its normal runtime
// policy instead of disguising another forced mode as the default.
enum class SyntheticProducerOutputFormMode : std::uint8_t {
  Auto = 0,
  CpuOnly = 1,
  GpuOnly = 2,
  CpuAndGpu = 3,
};

inline constexpr const char* kSyntheticProducerOutputFormProjectSetting =
    "cambang/maintainer/synthetic_producer_output_form";
inline constexpr const char* kSyntheticProducerOutputFormArg =
    "--cambang-synth-producer-output-form=";

inline bool parse_synthetic_producer_output_form_mode(
    std::string_view mode,
    SyntheticProducerOutputFormMode& out) noexcept {
  if (mode == "runtime_default") {
    out = SyntheticProducerOutputFormMode::Auto;
    return true;
  }
  if (mode == "cpu_only") {
    out = SyntheticProducerOutputFormMode::CpuOnly;
    return true;
  }
  if (mode == "cpu_gpu") {
    out = SyntheticProducerOutputFormMode::CpuAndGpu;
    return true;
  }
  if (mode == "gpu_only") {
    out = SyntheticProducerOutputFormMode::GpuOnly;
    return true;
  }
  return false;
}

inline const char* synthetic_producer_output_form_mode_setting_value(
    SyntheticProducerOutputFormMode mode) noexcept {
  switch (mode) {
    case SyntheticProducerOutputFormMode::Auto:
      return "runtime_default";
    case SyntheticProducerOutputFormMode::CpuOnly:
      return "cpu_only";
    case SyntheticProducerOutputFormMode::GpuOnly:
      return "gpu_only";
    case SyntheticProducerOutputFormMode::CpuAndGpu:
      return "cpu_gpu";
    default:
      return "runtime_default";
  }
}

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

  SyntheticProducerOutputFormMode producer_output_form_mode = SyntheticProducerOutputFormMode::Auto;

  // Verification-only realized EV override by image_member_index for still
  // capture metadata emission. This is intentionally non-release behavior used
  // by deterministic verifier fixtures to prove applied-vs-realized divergence
  // representation without changing normal synthetic realization semantics.
  std::map<std::uint32_t, std::int32_t> verification_realized_exposure_compensation_override_by_member_index{};

  // Verification-only realized-known presence override by image_member_index.
  // When present and false, emitted still-member metadata reports realized EV
  // unknown (`has_realized_exposure_compensation_milli_ev=false`) and does not
  // rely on sentinel numeric values. Non-release verifier seam only.
  std::map<std::uint32_t, bool> verification_has_realized_exposure_compensation_override_by_member_index{};
};

} // namespace cambang
