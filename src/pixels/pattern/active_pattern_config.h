#pragma once

#include <cstdint>

#include "pixels/pattern/pattern_registry.h"
#include "pixels/pattern/pattern_spec.h"

namespace cambang {

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
  const auto* info = find_preset_info(cfg.preset);
  const bool valid = (info != nullptr);
  if (out_preset_valid) *out_preset_valid = valid;

  // Deterministic fallback.
  if (!info) {
    info = find_preset_info(PatternPreset::XyXor);
  }

  PatternSpec spec{};
  spec.width = width;
  spec.height = height;
  spec.format = fmt;

  spec.seed = cfg.seed;

  spec.overlay_frame_index_offsets = cfg.overlay_frame_index_offsets;
  spec.overlay_moving_bar = cfg.overlay_moving_bar;

  // Table-driven selection.
  spec.algo = info ? info->algo : PatternAlgoId::XyXor;

  // Parameter gating driven by caps.
  const uint32_t caps = info ? info->caps : static_cast<uint32_t>(kCapsNone);

  if ((caps & static_cast<uint32_t>(kCapsRgba)) != 0u) {
    spec.solid_r = cfg.solid_r;
    spec.solid_g = cfg.solid_g;
    spec.solid_b = cfg.solid_b;
    spec.solid_a = cfg.solid_a;
  }

  if ((caps & static_cast<uint32_t>(kCapsCheckerSize)) != 0u) {
    spec.checker_size_px = cfg.checker_size_px;
  }

  return spec;
}

} // namespace cambang
