#pragma once

#include <cstdint>

#include "pixels/pattern/pattern_algo.h"

namespace cambang {

// Provider-agnostic description of a pattern to render.
//
// v1: CPU renderer targets packed 32-bit RGBA/BGRA buffers.
struct PatternSpec final {
  enum class PackedFormat : uint8_t {
    RGBA8 = 0,
    BGRA8 = 1,
  };

  uint32_t width = 0;
  uint32_t height = 0;
  PackedFormat format = PackedFormat::RGBA8;

  // Internal algorithm id (selected by preset registry).
  PatternAlgoId algo = PatternAlgoId::XyXor;

  // Base parameters (affect cache key).
  uint32_t seed = 0;

  // Overlay behaviour toggles (do not affect cache key).
  bool overlay_frame_index_offsets = true; // Apply per-frame constant offsets to RGB channels.
  bool overlay_moving_bar = true;          // Apply a moving vertical bar.

  // For patterns that need a constant color.
  uint8_t solid_r = 0;
  uint8_t solid_g = 0;
  uint8_t solid_b = 0;
  uint8_t solid_a = 0xFF;

  // For checker pattern.
  uint32_t checker_size_px = 16;

  // For color bars (reserved for future variants; affects cache key).
  uint32_t bars_variant = 0;
};

struct PatternOverlayData final {
  uint64_t frame_index = 0;
  uint64_t timestamp_ns = 0; // caller-provided; domain is intentionally opaque at this layer.
  uint64_t stream_id = 0;
};

// Cache key for base-frame generation.
struct PatternBaseKey final {
  uint32_t width = 0;
  uint32_t height = 0;
  PatternSpec::PackedFormat format = PatternSpec::PackedFormat::RGBA8;
  PatternAlgoId algo = PatternAlgoId::XyXor;

  // Base parameters (must include any fields that can change the base pixels).
  uint32_t seed = 0;
  uint32_t solid_rgba = 0;      // r | (g<<8) | (b<<16) | (a<<24)
  uint32_t checker_size_px = 0;
  uint32_t bars_variant = 0;

  bool operator==(const PatternBaseKey& o) const noexcept {
    return width == o.width &&
           height == o.height &&
           format == o.format &&
           algo == o.algo &&
           seed == o.seed &&
           solid_rgba == o.solid_rgba &&
           checker_size_px == o.checker_size_px &&
           bars_variant == o.bars_variant;
  }
  bool operator!=(const PatternBaseKey& o) const noexcept { return !(*this == o); }

  static PatternBaseKey from_spec(const PatternSpec& spec) noexcept {
    PatternBaseKey k;
    k.width = spec.width;
    k.height = spec.height;
    k.format = spec.format;
    k.algo = spec.algo;

    k.seed = spec.seed;
    k.solid_rgba = static_cast<uint32_t>(spec.solid_r) |
                   (static_cast<uint32_t>(spec.solid_g) << 8) |
                   (static_cast<uint32_t>(spec.solid_b) << 16) |
                   (static_cast<uint32_t>(spec.solid_a) << 24);
    k.checker_size_px = spec.checker_size_px;
    k.bars_variant = spec.bars_variant;
    return k;
  }
};

} // namespace cambang
