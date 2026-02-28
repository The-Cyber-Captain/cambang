#pragma once

#include <cstdint>

namespace cambang {

// Provider-agnostic description of a pattern to render.
//
// v1: CPU renderer targets packed 32-bit RGBA/BGRA buffers.
struct PatternSpec final {
  enum class PackedFormat : uint8_t {
    RGBA8 = 0,
    BGRA8 = 1,
  };

  enum class BasePattern : uint8_t {
    // A base that is deterministic per (x,y):
    //   r = x
    //   g = y
    //   b = x ^ y
    //   a = 255
    //
    // This is the historical stub pattern base (without the per-frame offsets).
    XY_XOR = 0,

    // Simple test patterns (implemented by CpuPackedPatternRenderer v1).
    Solid = 1,
    Checker = 2,
  };

  uint32_t width = 0;
  uint32_t height = 0;
  PackedFormat format = PackedFormat::RGBA8;

  BasePattern base = BasePattern::XY_XOR;

  // Base parameters (affect cache key)
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
  PatternSpec::BasePattern base = PatternSpec::BasePattern::XY_XOR;
  uint32_t seed = 0;

  bool operator==(const PatternBaseKey& o) const noexcept {
    return width == o.width && height == o.height && format == o.format && base == o.base && seed == o.seed;
  }
  bool operator!=(const PatternBaseKey& o) const noexcept { return !(*this == o); }

  static PatternBaseKey from_spec(const PatternSpec& spec) noexcept {
    PatternBaseKey k;
    k.width = spec.width;
    k.height = spec.height;
    k.format = spec.format;
    k.base = spec.base;
    k.seed = spec.seed;
    return k;
  }
};

} // namespace cambang
