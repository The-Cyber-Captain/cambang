#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

#include "pixels/pattern/pattern_algo.h"

namespace cambang {

// Zero-indexed, stable enum used by code surfaces (synthetic config, GDScript bindings, etc).
enum class PatternPreset : uint8_t {
#define X(preset_id, token_name, display_name, caps_bitmask, algo_id) preset_id,
#include "pixels/pattern/pattern_defs.inc"
#undef X
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
  const char* name;     // canonical token for CLI/UI config (e.g. "xy_xor")
  const char* display;  // optional nicer label for UI
  uint32_t caps;        // PatternPresetCaps bitmask
  PatternAlgoId algo;   // internal algorithm id (renderer dispatch)
};

// Canonical registry. Do not create parallel lists elsewhere.
inline constexpr PatternPresetInfo kPatternPresets[] = {
#define X(preset_id, token_name, display_name, caps_bitmask, algo_id)   { PatternPreset::preset_id, token_name, display_name, static_cast<uint32_t>(caps_bitmask), PatternAlgoId::algo_id },
#include "pixels/pattern/pattern_defs.inc"
#undef X
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

inline constexpr PatternAlgoId algo_for_preset_or_default(PatternPreset p, PatternAlgoId def = PatternAlgoId::XyXor) noexcept {
  const auto* info = find_preset_info(p);
  return info ? info->algo : def;
}

} // namespace cambang
