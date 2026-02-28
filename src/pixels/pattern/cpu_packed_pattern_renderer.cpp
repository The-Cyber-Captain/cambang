#include "pixels/pattern/cpu_packed_pattern_renderer.h"

#include <algorithm>
#include <cstring>

namespace cambang {

void CpuPackedPatternRenderer::configure(const PatternSpec& spec) {
  ensure_base(spec);
}

void CpuPackedPatternRenderer::ensure_base(const PatternSpec& spec) {
  const PatternBaseKey key = PatternBaseKey::from_spec(spec);

  if (base_valid_ && key == base_key_) {
    return;
  }

  base_key_ = key;
  base_valid_ = true;

  base_stride_bytes_ = key.width * PatternRenderTarget::bytes_per_pixel();
  base_pixels_.assign(static_cast<size_t>(base_stride_bytes_) * static_cast<size_t>(key.height), 0);

  switch (key.base) {
    case PatternSpec::BasePattern::XY_XOR:
      render_base_xy_xor(key);
      break;
    case PatternSpec::BasePattern::Solid:
      render_base_solid(spec);
      break;
    case PatternSpec::BasePattern::Checker:
      render_base_checker(spec);
      break;
    default:
      render_base_xy_xor(key);
      break;
  }
}

void CpuPackedPatternRenderer::render_into(
    const PatternSpec& spec,
    const PatternRenderTarget& dst,
    const PatternOverlayData& overlay) {
  if (!dst.is_valid()) {
    return;
  }
  if (spec.width != dst.width || spec.height != dst.height || spec.format != dst.format) {
    // Caller mismatch; do nothing deterministically.
    return;
  }

  ensure_base(spec);
  copy_base_to(dst);

  if (spec.overlay_frame_index_offsets) {
    apply_frame_index_offsets(spec, dst, overlay.frame_index);
  }
  if (spec.overlay_moving_bar) {
    apply_moving_bar(spec, dst, overlay.frame_index);
  }
}

void CpuPackedPatternRenderer::render_base_xy_xor(const PatternBaseKey& key) {
  // Base: r=x, g=y, b=x^y, a=255.
  for (uint32_t y = 0; y < key.height; ++y) {
    uint8_t* row = base_pixels_.data() + static_cast<size_t>(y) * base_stride_bytes_;
    for (uint32_t x = 0; x < key.width; ++x) {
      const uint8_t r = static_cast<uint8_t>(x & 0xFF);
      const uint8_t g = static_cast<uint8_t>(y & 0xFF);
      const uint8_t b = static_cast<uint8_t>((x ^ y) & 0xFF);
      write_px(row + static_cast<size_t>(x) * 4u, key.format, r, g, b, 0xFF);
    }
  }
}

void CpuPackedPatternRenderer::render_base_solid(const PatternSpec& spec) {
  for (uint32_t y = 0; y < spec.height; ++y) {
    uint8_t* row = base_pixels_.data() + static_cast<size_t>(y) * base_stride_bytes_;
    for (uint32_t x = 0; x < spec.width; ++x) {
      write_px(row + static_cast<size_t>(x) * 4u, spec.format, spec.solid_r, spec.solid_g, spec.solid_b, spec.solid_a);
    }
  }
}

void CpuPackedPatternRenderer::render_base_checker(const PatternSpec& spec) {
  const uint32_t step = (spec.checker_size_px == 0) ? 16u : spec.checker_size_px;
  for (uint32_t y = 0; y < spec.height; ++y) {
    uint8_t* row = base_pixels_.data() + static_cast<size_t>(y) * base_stride_bytes_;
    const uint32_t cy = (y / step);
    for (uint32_t x = 0; x < spec.width; ++x) {
      const uint32_t cx = (x / step);
      const bool on = ((cx + cy) & 1u) != 0u;
      const uint8_t v = on ? 0xE0 : 0x20;
      write_px(row + static_cast<size_t>(x) * 4u, spec.format, v, v, v, 0xFF);
    }
  }
}

void CpuPackedPatternRenderer::copy_base_to(const PatternRenderTarget& dst) const {
  // Row copy (dst stride may differ from tight base stride).
  const uint32_t row_bytes = base_stride_bytes_;
  for (uint32_t y = 0; y < dst.height; ++y) {
    const uint8_t* src_row = base_pixels_.data() + static_cast<size_t>(y) * row_bytes;
    uint8_t* dst_row = dst.row_ptr(y);
    std::memcpy(dst_row, src_row, row_bytes);
  }
}

void CpuPackedPatternRenderer::apply_frame_index_offsets(
    const PatternSpec& spec,
    const PatternRenderTarget& dst,
    uint64_t frame_index) const {
  // Historical stub behaviour:
  //   r = (x + fi) & 0xFF
  //   g = (y + (fi>>1)) & 0xFF
  //   b = ((x^y) + (fi>>2)) & 0xFF
  //
  // Base already has r=x, g=y, b=x^y.
  const uint8_t ro = static_cast<uint8_t>(frame_index & 0xFFu);
  const uint8_t go = static_cast<uint8_t>((frame_index >> 1) & 0xFFu);
  const uint8_t bo = static_cast<uint8_t>((frame_index >> 2) & 0xFFu);

  const bool rgba = (spec.format == PatternSpec::PackedFormat::RGBA8);

  for (uint32_t y = 0; y < dst.height; ++y) {
    uint8_t* row = dst.row_ptr(y);
    for (uint32_t x = 0; x < dst.width; ++x) {
      uint8_t* p = row + static_cast<size_t>(x) * 4u;
      if (rgba) {
        p[0] = static_cast<uint8_t>(p[0] + ro);
        p[1] = static_cast<uint8_t>(p[1] + go);
        p[2] = static_cast<uint8_t>(p[2] + bo);
      } else {
        // BGRA layout: [b,g,r,a]
        p[2] = static_cast<uint8_t>(p[2] + ro);
        p[1] = static_cast<uint8_t>(p[1] + go);
        p[0] = static_cast<uint8_t>(p[0] + bo);
      }
    }
  }
}

void CpuPackedPatternRenderer::apply_moving_bar(
    const PatternSpec& spec,
    const PatternRenderTarget& dst,
    uint64_t frame_index) const {
  if (dst.width == 0) return;

  const uint32_t bar_x = static_cast<uint32_t>((frame_index * 4u) % static_cast<uint64_t>(dst.width));

  for (uint32_t y = 0; y < dst.height; ++y) {
    uint8_t* p = dst.row_ptr(y) + static_cast<size_t>(bar_x) * 4u;
    write_px(p, spec.format, 0xFF, 0xFF, 0xFF, 0xFF);
  }
}

} // namespace cambang
