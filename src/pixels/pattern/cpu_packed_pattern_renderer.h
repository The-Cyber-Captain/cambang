#pragma once

#include <cstdint>
#include <vector>

#include "pixels/pattern/ipattern_renderer.h"

namespace cambang {

// CPU renderer for packed 32-bit RGBA/BGRA buffers.
//
// Design:
// - Base frame cached per PatternBaseKey.
// - Per-frame overlays applied without allocations.
class CpuPackedPatternRenderer final : public IPatternRenderer {
public:
  CpuPackedPatternRenderer() = default;
  ~CpuPackedPatternRenderer() override = default;

  void configure(const PatternSpec& spec) override;

  void render_into(
      const PatternSpec& spec,
      const PatternRenderTarget& dst,
      const PatternOverlayData& overlay) override;

private:
  void ensure_base(const PatternSpec& spec);

  using RenderBaseFn = void (CpuPackedPatternRenderer::*)(
      uint8_t* dst,
      uint32_t dst_stride_bytes,
      const PatternSpec& spec,
      const PatternBaseKey& key,
      const PatternOverlayData& overlay);

  void render_base_into(
      uint8_t* dst,
      uint32_t dst_stride_bytes,
      const PatternSpec& spec,
      const PatternBaseKey& key,
      const PatternOverlayData& overlay);

  void render_base_xy_xor(uint8_t* dst, uint32_t dst_stride_bytes, const PatternSpec& spec, const PatternBaseKey& key, const PatternOverlayData& overlay);
  void render_base_solid(uint8_t* dst, uint32_t dst_stride_bytes, const PatternSpec& spec, const PatternBaseKey& key, const PatternOverlayData& overlay);
  void render_base_checker(uint8_t* dst, uint32_t dst_stride_bytes, const PatternSpec& spec, const PatternBaseKey& key, const PatternOverlayData& overlay);
  void render_base_color_bars(uint8_t* dst, uint32_t dst_stride_bytes, const PatternSpec& spec, const PatternBaseKey& key, const PatternOverlayData& overlay);
  void render_base_radial_gradient(uint8_t* dst, uint32_t dst_stride_bytes, const PatternSpec& spec, const PatternBaseKey& key, const PatternOverlayData& overlay);
  void render_base_corners_rgba(uint8_t* dst, uint32_t dst_stride_bytes, const PatternSpec& spec, const PatternBaseKey& key, const PatternOverlayData& overlay);
  void render_base_noise(uint8_t* dst, uint32_t dst_stride_bytes, const PatternSpec& spec, const PatternBaseKey& key, const PatternOverlayData& overlay);

  void render_base_noise_common(uint8_t* dst, uint32_t dst_stride_bytes, const PatternSpec& spec, const PatternBaseKey& key, uint32_t phase);

  void copy_base_to(const PatternRenderTarget& dst) const;

  void apply_frame_index_offsets(const PatternSpec& spec, const PatternRenderTarget& dst, uint64_t frame_index) const;
  void apply_moving_bar(const PatternSpec& spec, const PatternRenderTarget& dst, uint64_t frame_index) const;

  static inline void write_px(uint8_t* p, PatternSpec::PackedFormat fmt, uint8_t r, uint8_t g, uint8_t b, uint8_t a) noexcept {
    if (fmt == PatternSpec::PackedFormat::RGBA8) {
      p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    } else {
      // BGRA
      p[0] = b; p[1] = g; p[2] = r; p[3] = a;
    }
  }

private:
  PatternBaseKey base_key_{};
  bool base_valid_ = false;

  uint32_t base_stride_bytes_ = 0; // tight
  std::vector<uint8_t> base_pixels_;
};

} // namespace cambang
