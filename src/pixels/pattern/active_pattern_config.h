#pragma once

#include <cstdint>
#include <string_view>

#include "pixels/pattern/pattern_spec.h"

namespace cambang {

// Canonical preset vocabulary for pattern generation.
//
// Requirements:
// - Values are zero-indexed (stable for int-based selection surfaces).
// - 1:1 mapping with preset "name" tokens for UI/CLI.
// - Invalid requests must have a defined fallback behavior at the call site.
enum class PatternPreset : std::uint8_t {
  XyXor = 0,
  Solid = 1,
  Checker = 2,
};

// Registry entry for UI/CLI selection and diagnostics.
struct PatternPresetInfo final {
  PatternPreset preset{};
  const char* name = nullptr;        // stable token, e.g. "xy_xor"
  const char* display_name = nullptr; // optional pretty label
};

// Canonical registry (single source of truth).
inline constexpr PatternPresetInfo kPatternPresetRegistry[] = {
    {PatternPreset::XyXor, "xy_xor", "XY XOR"},
    {PatternPreset::Solid, "solid", "Solid"},
    {PatternPreset::Checker, "checker", "Checker"},
};

inline constexpr std::uint32_t pattern_preset_count() noexcept {
  return static_cast<std::uint32_t>(sizeof(kPatternPresetRegistry) / sizeof(kPatternPresetRegistry[0]));
}

// Returns nullptr if not found.
inline constexpr const PatternPresetInfo* find_pattern_preset(PatternPreset p) noexcept {
  for (std::uint32_t i = 0; i < pattern_preset_count(); ++i) {
    if (kPatternPresetRegistry[i].preset == p) return &kPatternPresetRegistry[i];
  }
  return nullptr;
}

// Returns nullptr if not found (case sensitive; tokens are lower snake_case).
inline const PatternPresetInfo* find_pattern_preset(std::string_view name) noexcept {
  for (std::uint32_t i = 0; i < pattern_preset_count(); ++i) {
    const auto* n = kPatternPresetRegistry[i].name;
    if (n && name == n) return &kPatternPresetRegistry[i];
  }
  return nullptr;
}

// Returns nullptr if out of range.
inline constexpr const PatternPresetInfo* find_pattern_preset_by_index(std::uint32_t index_zero_based) noexcept {
  return (index_zero_based < pattern_preset_count()) ? &kPatternPresetRegistry[index_zero_based] : nullptr;
}

// Provider-facing "active selection" that may change at runtime.
// Geometry/format come from the caller per render.
struct ActivePatternConfig final {
  PatternPreset preset = PatternPreset::XyXor;

  std::uint32_t seed = 0;

  // Renderer overlays currently implemented.
  bool overlay_frame_index_offsets = true;
  bool overlay_moving_bar = true;

  // Params for specific presets.
  std::uint8_t solid_r = 0;
  std::uint8_t solid_g = 0;
  std::uint8_t solid_b = 0;
  std::uint8_t solid_a = 0xFF;

  std::uint32_t checker_size_px = 16;
};

// Maps ActivePatternConfig -> renderer-facing PatternSpec.
inline PatternSpec to_pattern_spec(const ActivePatternConfig& cfg,
                                  std::uint32_t width,
                                  std::uint32_t height,
                                  PatternSpec::PackedFormat fmt) noexcept {
  PatternSpec spec{};
  spec.width = width;
  spec.height = height;
  spec.format = fmt;
  spec.seed = cfg.seed;

  spec.overlay_frame_index_offsets = cfg.overlay_frame_index_offsets;
  spec.overlay_moving_bar = cfg.overlay_moving_bar;

  // Defined behavior: if cfg.preset is unknown (not in registry), fall back to XY_XOR.
  PatternPreset p = cfg.preset;
  if (!find_pattern_preset(p)) {
    p = PatternPreset::XyXor;
  }

  switch (p) {
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
