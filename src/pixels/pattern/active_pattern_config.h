#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

#include "pixels/pattern/pattern_spec.h"

namespace cambang {

// Zero-indexed, stable enum used by code surfaces (synthetic config, GDScript bindings, etc).
enum class PatternPreset : uint8_t {
  XyXor = 0,
  Solid = 1,
  Checker = 2,
};

// Capability flags (used by CLI/UI surfaces to validate parameter combinations).
enum PatternPresetCaps : uint32_t {
  kCapsNone        = 0,
  kCapsSeed        = 1u << 0,
  kCapsRgba        = 1u << 1,
  kCapsCheckerSize = 1u << 2,
};

struct PatternPresetInfo final {
  PatternPreset preset;
  const char* name;        // canonical token for CLI/UI config (e.g. "xy_xor")
  const char* display;     // optional nicer label for UI
  uint32_t caps;           // PatternPresetCaps bitmask
};

// Canonical registry. Keep this as the only authority.
inline constexpr PatternPresetInfo kPatternPresets[] = {
  { PatternPreset::XyXor,  "xy_xor",  "XY XOR",       kCapsSeed },
  { PatternPreset::Solid,  "solid",   "Solid",        kCapsSeed | kCapsRgba },
  { PatternPreset::Checker,"checker", "Checkerboard", kCapsSeed | kCapsCheckerSize },
};

inline constexpr size_t pattern_preset_count() noexcept {
  return sizeof(kPatternPresets) / sizeof(kPatternPresets[0]);
}


inline constexpr const PatternPresetInfo* pattern_presets(size_t* out_n = nullptr) noexcept {
  if (out_n) *out_n = pattern_preset_count();
  return kPatternPresets;
}


inline constexpr const PatternPresetInfo* find_preset_info(PatternPreset p) noexcept {
  for (const auto& info : kPatternPresets) {
    if (info.preset == p) return &info;
  }
  return nullptr;
}

inline constexpr const PatternPresetInfo* find_preset_info_by_name(std::string_view name) noexcept {
  for (const auto& info : kPatternPresets) {
    if (std::string_view(info.name) == name) return &info;
  }
  return nullptr;
}

// Index-based selection helper (zero-indexed).
inline constexpr PatternPreset preset_from_index_or_default(uint32_t index, PatternPreset def = PatternPreset::XyXor) noexcept {
  return (index < pattern_preset_count()) ? kPatternPresets[index].preset : def;
}

// Provider-facing selection (swappable at runtime via shared_ptr).
struct ActivePatternConfig final {
  PatternPreset preset = PatternPreset::XyXor;
  uint32_t seed = 0;

  // Implemented renderer overlays
  bool overlay_frame_index_offsets = true;
  bool overlay_moving_bar = true;

  // Params used by some presets
  uint8_t solid_r = 0;
  uint8_t solid_g = 0;
  uint8_t solid_b = 0;
  uint8_t solid_a = 0xFF;

  uint32_t checker_size_px = 16;
};

// Convert ActivePatternConfig + geometry to renderer PatternSpec.
// If out_preset_valid is provided, it is set to whether cfg.preset existed in registry.
// Invalid presets deterministically fall back to XyXor.
inline PatternSpec to_pattern_spec(const ActivePatternConfig& cfg,
                                  uint32_t width,
                                  uint32_t height,
                                  PatternSpec::PackedFormat fmt,
                                  bool* out_preset_valid = nullptr) noexcept {
  bool valid = (find_preset_info(cfg.preset) != nullptr);
  if (out_preset_valid) *out_preset_valid = valid;

  const PatternPreset effective = valid ? cfg.preset : PatternPreset::XyXor;

  PatternSpec spec{};
  spec.width = width;
  spec.height = height;
  spec.format = fmt;

  spec.seed = cfg.seed;

  spec.overlay_frame_index_offsets = cfg.overlay_frame_index_offsets;
  spec.overlay_moving_bar = cfg.overlay_moving_bar;

  switch (effective) {
    case PatternPreset::XyXor:
      spec.base = PatternSpec::BasePattern::XY_XOR;
      break;
    case PatternPreset::Solid:
      spec.base = PatternSpec::BasePattern::Solid;
      spec.solid_r = cfg.solid_r;
      spec.solid_g = cfg.solid_g;
      spec.solid_b = cfg.solid_b;
      spec.solid_a = cfg.solid_a;
      break;
    case PatternPreset::Checker:
      spec.base = PatternSpec::BasePattern::Checker;
      spec.checker_size_px = cfg.checker_size_px;
      break;
    default:
      spec.base = PatternSpec::BasePattern::XY_XOR;
      break;
  }

  return spec;
}

} // namespace cambang
