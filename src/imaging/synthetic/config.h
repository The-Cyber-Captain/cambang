#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "imaging/api/provider_contract_datatypes.h"
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

struct SyntheticStreamCapabilityDowngradeCondition {
  std::string device_hardware_id{};
  bool has_stream_intent = false;
  StreamIntent stream_intent = StreamIntent::PREVIEW;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_fourcc = 0;
  uint32_t target_fps = 0;
};

enum class SyntheticStillImageBundleDiscriminator : std::uint8_t {
  Any = 0,
  DefaultMeteredOnly = 1,
  MultiImage = 2,
};

struct SyntheticCaptureCapabilityDowngradeCondition {
  std::string device_hardware_id{};
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_fourcc = 0;
  SyntheticStillImageBundleDiscriminator still_image_bundle =
      SyntheticStillImageBundleDiscriminator::Any;
};

inline constexpr const char* kSyntheticProducerOutputFormProjectSetting =
    "cambang/maintainer/synthetic_producer_output_form";
inline constexpr const char* kSyntheticProducerOutputFormArg =
    "--cambang-synth-producer-output-form=";
inline constexpr const char* kSyntheticStreamCapabilityDowngradeProjectSetting =
    "cambang/maintainer/synthetic_stream_capability_downgrades";
inline constexpr const char* kSyntheticStreamCapabilityDowngradeArg =
    "--cambang-synth-stream-capability-downgrades=";
inline constexpr const char* kSyntheticCaptureCapabilityDowngradeProjectSetting =
    "cambang/maintainer/synthetic_capture_capability_downgrades";
inline constexpr const char* kSyntheticCaptureCapabilityDowngradeArg =
    "--cambang-synth-capture-capability-downgrades=";

inline std::string_view trim_ascii_whitespace(std::string_view text) noexcept {
  size_t begin = 0;
  while (begin < text.size() &&
         std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(begin, end - begin);
}

inline bool parse_uint32_decimal(std::string_view text, uint32_t& out) noexcept {
  text = trim_ascii_whitespace(text);
  if (text.empty()) {
    return false;
  }
  uint64_t value = 0;
  for (char ch : text) {
    if (ch < '0' || ch > '9') {
      return false;
    }
    value = value * 10u + static_cast<uint64_t>(ch - '0');
    if (value > 0xFFFFFFFFull) {
      return false;
    }
  }
  out = static_cast<uint32_t>(value);
  return true;
}

inline bool parse_fourcc_token(std::string_view text, uint32_t& out) noexcept {
  text = trim_ascii_whitespace(text);
  if (text.size() == 4u) {
    out = make_fourcc(text[0], text[1], text[2], text[3]);
    return true;
  }
  return parse_uint32_decimal(text, out);
}

inline bool parse_stream_intent_token(std::string_view text,
                                      StreamIntent& out) noexcept {
  text = trim_ascii_whitespace(text);
  if (text == "preview") {
    out = StreamIntent::PREVIEW;
    return true;
  }
  if (text == "viewfinder") {
    out = StreamIntent::VIEWFINDER;
    return true;
  }
  return false;
}

inline bool parse_still_image_bundle_discriminator_token(
    std::string_view text,
    SyntheticStillImageBundleDiscriminator& out) noexcept {
  text = trim_ascii_whitespace(text);
  if (text == "default") {
    out = SyntheticStillImageBundleDiscriminator::DefaultMeteredOnly;
    return true;
  }
  if (text == "multi") {
    out = SyntheticStillImageBundleDiscriminator::MultiImage;
    return true;
  }
  return false;
}

inline bool parse_synthetic_stream_capability_downgrade_conditions(
    std::string_view text,
    std::vector<SyntheticStreamCapabilityDowngradeCondition>& out) noexcept {
  out.clear();
  text = trim_ascii_whitespace(text);
  if (text.empty()) {
    return true;
  }

  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t next = text.find(';', pos);
    std::string_view entry = trim_ascii_whitespace(
        text.substr(pos, next == std::string_view::npos ? text.size() - pos
                                                        : next - pos));
    if (entry.empty()) {
      return false;
    }
    SyntheticStreamCapabilityDowngradeCondition condition{};
    bool seen_device = false;
    bool seen_intent = false;
    bool seen_width = false;
    bool seen_height = false;
    bool seen_format = false;
    bool seen_fps = false;
    size_t pair_pos = 0;
    while (pair_pos <= entry.size()) {
      const size_t pair_next = entry.find(',', pair_pos);
      std::string_view pair = trim_ascii_whitespace(
          entry.substr(pair_pos, pair_next == std::string_view::npos
                                     ? entry.size() - pair_pos
                                     : pair_next - pair_pos));
      if (pair.empty()) {
        return false;
      }
      const size_t eq = pair.find('=');
      if (eq == std::string_view::npos) {
        return false;
      }
      const std::string_view key = trim_ascii_whitespace(pair.substr(0, eq));
      const std::string_view value =
          trim_ascii_whitespace(pair.substr(eq + 1));
      if (key.empty() || value.empty()) {
        return false;
      }
      if (key == "device") {
        if (seen_device) {
          return false;
        }
        condition.device_hardware_id.assign(value.begin(), value.end());
        seen_device = true;
      } else if (key == "intent") {
        if (seen_intent ||
            !parse_stream_intent_token(value, condition.stream_intent)) {
          return false;
        }
        condition.has_stream_intent = true;
        seen_intent = true;
      } else if (key == "width") {
        if (seen_width || !parse_uint32_decimal(value, condition.width)) {
          return false;
        }
        seen_width = true;
      } else if (key == "height") {
        if (seen_height || !parse_uint32_decimal(value, condition.height)) {
          return false;
        }
        seen_height = true;
      } else if (key == "format") {
        if (seen_format ||
            !parse_fourcc_token(value, condition.format_fourcc)) {
          return false;
        }
        seen_format = true;
      } else if (key == "fps") {
        if (seen_fps || !parse_uint32_decimal(value, condition.target_fps)) {
          return false;
        }
        seen_fps = true;
      } else {
        return false;
      }
      if (pair_next == std::string_view::npos) {
        break;
      }
      pair_pos = pair_next + 1u;
    }
    if (!seen_device || condition.device_hardware_id.empty()) {
      return false;
    }
    out.push_back(condition);
    if (next == std::string_view::npos) {
      break;
    }
    pos = next + 1u;
  }

  return true;
}

inline bool parse_synthetic_capture_capability_downgrade_conditions(
    std::string_view text,
    std::vector<SyntheticCaptureCapabilityDowngradeCondition>& out) noexcept {
  out.clear();
  text = trim_ascii_whitespace(text);
  if (text.empty()) {
    return true;
  }

  size_t pos = 0;
  while (pos <= text.size()) {
    const size_t next = text.find(';', pos);
    std::string_view entry = trim_ascii_whitespace(
        text.substr(pos, next == std::string_view::npos ? text.size() - pos
                                                        : next - pos));
    if (entry.empty()) {
      return false;
    }
    SyntheticCaptureCapabilityDowngradeCondition condition{};
    bool seen_device = false;
    bool seen_width = false;
    bool seen_height = false;
    bool seen_format = false;
    bool seen_bundle = false;
    size_t pair_pos = 0;
    while (pair_pos <= entry.size()) {
      const size_t pair_next = entry.find(',', pair_pos);
      std::string_view pair = trim_ascii_whitespace(
          entry.substr(pair_pos, pair_next == std::string_view::npos
                                     ? entry.size() - pair_pos
                                     : pair_next - pair_pos));
      if (pair.empty()) {
        return false;
      }
      const size_t eq = pair.find('=');
      if (eq == std::string_view::npos) {
        return false;
      }
      const std::string_view key = trim_ascii_whitespace(pair.substr(0, eq));
      const std::string_view value =
          trim_ascii_whitespace(pair.substr(eq + 1));
      if (key.empty() || value.empty()) {
        return false;
      }
      if (key == "device") {
        if (seen_device) {
          return false;
        }
        condition.device_hardware_id.assign(value.begin(), value.end());
        seen_device = true;
      } else if (key == "width") {
        if (seen_width || !parse_uint32_decimal(value, condition.width)) {
          return false;
        }
        seen_width = true;
      } else if (key == "height") {
        if (seen_height || !parse_uint32_decimal(value, condition.height)) {
          return false;
        }
        seen_height = true;
      } else if (key == "format") {
        if (seen_format ||
            !parse_fourcc_token(value, condition.format_fourcc)) {
          return false;
        }
        seen_format = true;
      } else if (key == "bundle") {
        if (seen_bundle ||
            !parse_still_image_bundle_discriminator_token(
                value, condition.still_image_bundle)) {
          return false;
        }
        seen_bundle = true;
      } else {
        return false;
      }
      if (pair_next == std::string_view::npos) {
        break;
      }
      pair_pos = pair_next + 1u;
    }
    if (!seen_device || condition.device_hardware_id.empty()) {
      return false;
    }
    out.push_back(condition);
    if (next == std::string_view::npos) {
      break;
    }
    pos = next + 1u;
  }

  return true;
}

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
  std::vector<SyntheticStreamCapabilityDowngradeCondition>
      verification_stream_capability_downgrade_conditions{};
  std::vector<SyntheticCaptureCapabilityDowngradeCondition>
      verification_capture_capability_downgrade_conditions{};

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
