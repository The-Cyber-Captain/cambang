#include "pixels/pattern/cpu_packed_pattern_renderer.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace cambang {

void CpuPackedPatternRenderer::configure(const PatternSpec& spec) {
  // Dynamic-base algos render the base per-frame; no base-cache precompute needed.
  if (!spec.dynamic_base) {
    ensure_base(spec);
  }
}

void CpuPackedPatternRenderer::render_base_into(
    uint8_t* dst,
    uint32_t dst_stride_bytes,
    const PatternSpec& spec,
    const PatternBaseKey& key,
    const PatternOverlayData& overlay) {
  // Table-driven algorithm dispatch (keeps preset registry as the only pattern list).
  static constexpr RenderBaseFn kAlgoFns[] = {
      &CpuPackedPatternRenderer::render_base_xy_xor,
      &CpuPackedPatternRenderer::render_base_solid,
      &CpuPackedPatternRenderer::render_base_checker,
      &CpuPackedPatternRenderer::render_base_color_bars,
      &CpuPackedPatternRenderer::render_base_radial_gradient,
      &CpuPackedPatternRenderer::render_base_corners_rgba,
      &CpuPackedPatternRenderer::render_base_noise,
      &CpuPackedPatternRenderer::render_base_noise_animated,

  };

  const size_t idx = static_cast<size_t>(key.algo);
  RenderBaseFn fn = &CpuPackedPatternRenderer::render_base_xy_xor;
  if (idx < (sizeof(kAlgoFns) / sizeof(kAlgoFns[0]))) {
    fn = kAlgoFns[idx];
  }
  (this->*fn)(dst, dst_stride_bytes, spec, key, overlay);
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

  // Cacheable-base path: render into tight internal cache buffer.
  PatternOverlayData overlay{};
  const auto t0 = std::chrono::steady_clock::now();
  render_base_into(base_pixels_.data(), base_stride_bytes_, spec, key, overlay);
  const auto t1 = std::chrono::steady_clock::now();
  const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
  debug_stats_.base_render_total_ns += ns;
  debug_stats_.base_render_max_ns = std::max(debug_stats_.base_render_max_ns, ns);
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

  if (spec.dynamic_base) {
    ++debug_stats_.base_cache_miss_count;
    // Dynamic-base path: bypass base cache and render the base into the destination each frame.
    const PatternBaseKey key = PatternBaseKey::from_spec(spec);
    const auto t0 = std::chrono::steady_clock::now();
    render_base_into(static_cast<uint8_t*>(dst.data), dst.stride_bytes, spec, key, overlay);
    const auto t1 = std::chrono::steady_clock::now();
    const uint64_t ns = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    debug_stats_.base_render_total_ns += ns;
    debug_stats_.base_render_max_ns = std::max(debug_stats_.base_render_max_ns, ns);
  } else {
    const PatternBaseKey key = PatternBaseKey::from_spec(spec);
    if (base_valid_ && key == base_key_) {
      ++debug_stats_.base_cache_hit_count;
    } else {
      ++debug_stats_.base_cache_miss_count;
    }
    ensure_base(spec);
    const auto copy_t0 = std::chrono::steady_clock::now();
    copy_base_to(dst);
    const auto copy_t1 = std::chrono::steady_clock::now();
    const uint64_t copy_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(copy_t1 - copy_t0).count());
    debug_stats_.base_copy_total_ns += copy_ns;
    debug_stats_.base_copy_max_ns = std::max(debug_stats_.base_copy_max_ns, copy_ns);
  }

  const auto overlay_t0 = std::chrono::steady_clock::now();
  if (spec.overlay_frame_index_offsets) {
    apply_frame_index_offsets(spec, dst, overlay.frame_index);
  }
  if (spec.overlay_moving_bar) {
    apply_moving_bar(spec, dst, overlay.frame_index);
  }
  const auto overlay_t1 = std::chrono::steady_clock::now();
  const uint64_t overlay_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(overlay_t1 - overlay_t0).count());
  debug_stats_.overlay_total_ns += overlay_ns;
  debug_stats_.overlay_max_ns = std::max(debug_stats_.overlay_max_ns, overlay_ns);
}

void CpuPackedPatternRenderer::render_base_xy_xor(
    uint8_t* dst,
    uint32_t dst_stride_bytes,
    const PatternSpec& /*spec*/,
    const PatternBaseKey& key,
    const PatternOverlayData& /*overlay*/) {
  // Base: r=x, g=y, b=x^y, a=255.
  for (uint32_t y = 0; y < key.height; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes);
    for (uint32_t x = 0; x < key.width; ++x) {
      const uint8_t r = static_cast<uint8_t>(x & 0xFF);
      const uint8_t g = static_cast<uint8_t>(y & 0xFF);
      const uint8_t b = static_cast<uint8_t>((x ^ y) & 0xFF);
      write_px(row + static_cast<size_t>(x) * 4u, key.format, r, g, b, 0xFF);
    }
  }
}

void CpuPackedPatternRenderer::render_base_solid(
    uint8_t* dst,
    uint32_t dst_stride_bytes,
    const PatternSpec& spec,
    const PatternBaseKey& /*key*/,
    const PatternOverlayData& /*overlay*/) {
  for (uint32_t y = 0; y < spec.height; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes);
    for (uint32_t x = 0; x < spec.width; ++x) {
      write_px(row + static_cast<size_t>(x) * 4u, spec.format, spec.solid_r, spec.solid_g, spec.solid_b, spec.solid_a);
    }
  }
}

void CpuPackedPatternRenderer::render_base_checker(
    uint8_t* dst,
    uint32_t dst_stride_bytes,
    const PatternSpec& spec,
    const PatternBaseKey& /*key*/,
    const PatternOverlayData& /*overlay*/) {
  const uint32_t step = (spec.checker_size_px == 0) ? 16u : spec.checker_size_px;
  for (uint32_t y = 0; y < spec.height; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes);
    const uint32_t cy = (y / step);
    for (uint32_t x = 0; x < spec.width; ++x) {
      const uint32_t cx = (x / step);
      const bool on = ((cx + cy) & 1u) != 0u;
      const uint8_t v = on ? 0xE0 : 0x20;
      write_px(row + static_cast<size_t>(x) * 4u, spec.format, v, v, v, 0xFF);
    }
  }
}

void CpuPackedPatternRenderer::render_base_color_bars(
    uint8_t* dst,
    uint32_t dst_stride_bytes,
    const PatternSpec& spec,
    const PatternBaseKey& /*key*/,
    const PatternOverlayData& /*overlay*/) {
  // Deterministic 7-bar palette (SMPTE-ish). Intended for visual validation.
  struct RGBA { uint8_t r, g, b, a; };
  static constexpr RGBA bars[7] = {
      {0xEB, 0xEB, 0xEB, 0xFF},
      {0xEB, 0xEB, 0x00, 0xFF},
      {0x00, 0xEB, 0xEB, 0xFF},
      {0x00, 0xEB, 0x00, 0xFF},
      {0xEB, 0x00, 0xEB, 0xFF},
      {0xEB, 0x00, 0x00, 0xFF},
      {0x00, 0x00, 0xEB, 0xFF},
  };

  const uint32_t w = (spec.width == 0) ? 1u : spec.width;
  const uint32_t bar_w = std::max<uint32_t>(1u, w / 7u);

  for (uint32_t y = 0; y < spec.height; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes);
    for (uint32_t x = 0; x < spec.width; ++x) {
      uint32_t idx = x / bar_w;
      if (idx > 6u) idx = 6u;
      const RGBA c = bars[idx];
      write_px(row + static_cast<size_t>(x) * 4u, spec.format, c.r, c.g, c.b, c.a);
    }
  }
}

void CpuPackedPatternRenderer::render_base_radial_gradient(
    uint8_t* dst,
    uint32_t dst_stride_bytes,
    const PatternSpec& spec,
    const PatternBaseKey& /*key*/,
    const PatternOverlayData& /*overlay*/) {
  // Smooth radial gradient: bright center -> darker edges, with blue edge cue.
  const uint32_t w = spec.width;
  const uint32_t h = spec.height;
  if (w == 0 || h == 0) {
    return;
  }

  const int32_t cx = static_cast<int32_t>(w / 2u);
  const int32_t cy = static_cast<int32_t>(h / 2u);

  const int64_t rx = static_cast<int64_t>(cx);
  const int64_t ry = static_cast<int64_t>(cy);
  const int64_t max_r2 = std::max<int64_t>(1, rx * rx + ry * ry);

  for (uint32_t y = 0; y < h; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes);
    const int32_t dy = static_cast<int32_t>(y) - cy;
    for (uint32_t x = 0; x < w; ++x) {
      const int32_t dx = static_cast<int32_t>(x) - cx;
      const int64_t r2 = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;

      uint32_t t = static_cast<uint32_t>((r2 * 255) / max_r2);
      if (t > 255u) t = 255u;

      const uint8_t v = static_cast<uint8_t>(255u - t);
      const uint8_t b = static_cast<uint8_t>(std::min<uint32_t>(255u, t));
      const uint8_t g = static_cast<uint8_t>((static_cast<uint32_t>(v) * 3u) / 4u);
      const uint8_t r = v;

      write_px(row + static_cast<size_t>(x) * 4u, spec.format, r, g, b, 0xFF);
    }
  }
}

void CpuPackedPatternRenderer::render_base_corners_rgba(
    uint8_t* dst,
    uint32_t dst_stride_bytes,
    const PatternSpec& spec,
    const PatternBaseKey& key,
    const PatternOverlayData& /*overlay*/) {
  const uint32_t w = key.width;
  const uint32_t h = key.height;
  if (w == 0 || h == 0) {
    return;
  }

  // Corner patch size: 1/8 of frame (minimum 1 px).
  const uint32_t cw = std::max<uint32_t>(1u, w / 8u);
  const uint32_t ch = std::max<uint32_t>(1u, h / 8u);

  // Background colour uses spec.solid_* so callers can exercise --rgba.
  const uint8_t bg_r = spec.solid_r;
  const uint8_t bg_g = spec.solid_g;
  const uint8_t bg_b = spec.solid_b;
  const uint8_t bg_a = spec.solid_a;

  // Canonical corner swatches (alpha=255):
  // TL red, TR green, BL blue, BR white.
  constexpr uint8_t TL[4] = {0xFF, 0x00, 0x00, 0xFF};
  constexpr uint8_t TR[4] = {0x00, 0xFF, 0x00, 0xFF};
  constexpr uint8_t BL[4] = {0x00, 0x00, 0xFF, 0xFF};
  constexpr uint8_t BR[4] = {0xFF, 0xFF, 0xFF, 0xFF};

  // Optional border/crosshair colours.
  constexpr uint8_t BORDER[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  constexpr uint8_t CROSS[4]  = {0xFF, 0xFF, 0x00, 0xFF}; // yellow

  // 1) Fill background.
  for (uint32_t y = 0; y < h; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes);
    for (uint32_t x = 0; x < w; ++x) {
      write_px(row + static_cast<size_t>(x) * 4u, key.format, bg_r, bg_g, bg_b, bg_a);
    }
  }

  // Helper lambda for solid rect fill.
  auto fill_rect = [&](uint32_t x0, uint32_t y0, uint32_t rw, uint32_t rh, const uint8_t c[4]) {
    const uint32_t x1 = std::min<uint32_t>(w, x0 + rw);
    const uint32_t y1 = std::min<uint32_t>(h, y0 + rh);
    for (uint32_t y = y0; y < y1; ++y) {
      uint8_t* row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes);
      for (uint32_t x = x0; x < x1; ++x) {
        write_px(row + static_cast<size_t>(x) * 4u, key.format, c[0], c[1], c[2], c[3]);
      }
    }
  };

  // 2) Corner swatches.
  fill_rect(0,      0,      cw, ch, TL);
  fill_rect(w - cw, 0,      cw, ch, TR);
  fill_rect(0,      h - ch, cw, ch, BL);
  fill_rect(w - cw, h - ch, cw, ch, BR);

  // 3) 1px border.
  {
    uint8_t* top = dst;
    uint8_t* bot = dst + static_cast<size_t>(h - 1u) * static_cast<size_t>(dst_stride_bytes);
    for (uint32_t x = 0; x < w; ++x) {
      write_px(top + static_cast<size_t>(x) * 4u, key.format, BORDER[0], BORDER[1], BORDER[2], BORDER[3]);
      write_px(bot + static_cast<size_t>(x) * 4u, key.format, BORDER[0], BORDER[1], BORDER[2], BORDER[3]);
    }
  }
  for (uint32_t y = 0; y < h; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes);
    write_px(row + 0u, key.format, BORDER[0], BORDER[1], BORDER[2], BORDER[3]);
    write_px(row + static_cast<size_t>(w - 1u) * 4u, key.format, BORDER[0], BORDER[1], BORDER[2], BORDER[3]);
  }

  // 4) 1px crosshair at center.
  const uint32_t mid_x = w / 2u;
  const uint32_t mid_y = h / 2u;

  for (uint32_t y = 0; y < h; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes);
    write_px(row + static_cast<size_t>(mid_x) * 4u, key.format, CROSS[0], CROSS[1], CROSS[2], CROSS[3]);
  }
  {
    uint8_t* row = dst + static_cast<size_t>(mid_y) * static_cast<size_t>(dst_stride_bytes);
    for (uint32_t x = 0; x < w; ++x) {
      write_px(row + static_cast<size_t>(x) * 4u, key.format, CROSS[0], CROSS[1], CROSS[2], CROSS[3]);
    }
  }
}


void CpuPackedPatternRenderer::render_base_noise_common(
      uint8_t* dst,
      uint32_t dst_stride_bytes,
      const PatternSpec& spec,
      const PatternBaseKey& key,
      uint32_t phase) {
  const uint32_t w = key.width;
  const uint32_t h = key.height;
  if (w == 0 || h == 0) {
    return;
  }

  // Deterministic hash; no state. (Splitmix32-ish.)
  auto hash32 = [](uint32_t v) -> uint32_t {
    v += 0x9E3779B9u;
    v ^= v >> 16;
    v *= 0x85EBCA6Bu;
    v ^= v >> 13;
    v *= 0xC2B2AE35u;
    v ^= v >> 16;
    return v;
  };

  const uint32_t seed = spec.seed;

  for (uint32_t y = 0; y < h; ++y) {
    uint8_t* row = dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes);
    for (uint32_t x = 0; x < w; ++x) {
      const uint32_t v =
          seed ^
          (x * 0x1E35A7BDu) ^
          (y * 0x94D049BBu) ^
          (phase * 0xD1B54A35u);

      const uint32_t r = hash32(v);

      const uint8_t R = static_cast<uint8_t>(r >> 0);
      const uint8_t G = static_cast<uint8_t>(r >> 8);
      const uint8_t B = static_cast<uint8_t>(r >> 16);
      const uint8_t A = 0xFF;

      write_px(row + static_cast<size_t>(x) * 4u, key.format, R, G, B, A);
    }
  }
}

void CpuPackedPatternRenderer::render_base_noise(
      uint8_t* dst,
      uint32_t dst_stride_bytes,
      const PatternSpec& spec,
      const PatternBaseKey& key,
      const PatternOverlayData& /*overlay*/) {
  // Static noise is cacheable: phase is constant.
  render_base_noise_common(dst, dst_stride_bytes, spec, key, /*phase=*/0u);
}

void CpuPackedPatternRenderer::render_base_noise_animated(
      uint8_t* dst,
      uint32_t dst_stride_bytes,
      const PatternSpec& spec,
      const PatternBaseKey& key,
      const PatternOverlayData& overlay) {
  // Dynamic noise: phase is per-frame -> dynamic_base preset bypasses base cache.
  const uint32_t phase = static_cast<uint32_t>(overlay.frame_index);
  render_base_noise_common(dst, dst_stride_bytes, spec, key, phase);
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
