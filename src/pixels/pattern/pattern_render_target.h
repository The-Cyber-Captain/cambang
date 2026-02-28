#pragma once

#include <cstddef>
#include <cstdint>

#include "pixels/pattern/pattern_spec.h"

namespace cambang {

// Caller-owned render destination.
struct PatternRenderTarget final {
  void* data = nullptr;
  size_t size_bytes = 0;

  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0; // bytes per row

  PatternSpec::PackedFormat format = PatternSpec::PackedFormat::RGBA8;

  static constexpr uint32_t bytes_per_pixel() noexcept { return 4u; }

  uint8_t* row_ptr(uint32_t y) const noexcept {
    return static_cast<uint8_t*>(data) + static_cast<size_t>(y) * static_cast<size_t>(stride_bytes);
  }

  bool is_valid() const noexcept {
    if (!data) return false;
    if (width == 0 || height == 0) return false;
    if (stride_bytes < width * bytes_per_pixel()) return false;
    const size_t min_bytes = static_cast<size_t>(stride_bytes) * static_cast<size_t>(height);
    return size_bytes >= min_bytes;
  }
};

} // namespace cambang
