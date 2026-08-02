// src/pixels/format/pixel_format_descriptor.h
#pragma once

#include <cstddef>
#include <cstdint>

// Canonical CamBANG pixel-format truth table.
//
// This is the single source of layout arithmetic for every pixel format
// CamBANG can name: plane count, per-plane row geometry, component depth, and
// whether the format is a packed RGB-family buffer or a subsampled YUV-family
// one. Provider, Core, and Godot layers all derive their size/stride/bit-depth
// answers from here rather than open-coding `width * 4`.
//
// Header-only and dependency-free on purpose: it sits below the provider
// contract, Core, and the Godot wrappers, and all three include it.
//
// Naming a format here does NOT mean every CamBANG path can retain, display,
// or materialize it. Support for a format is proven by the paths that
// implement it, not by the presence of a descriptor. See
// docs/architecture/pixel_payload_and_result_contract.md.

namespace cambang {

// --- FourCC helpers ---------------------------------------------------------
// Canonical CamBANG pixel formats use a FourCC-style 32-bit tag.
// Use these helpers instead of ad-hoc literals to keep format handling stable.
constexpr uint32_t make_fourcc(char a, char b, char c, char d) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(a))      ) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b)) <<  8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

// Packed RGB-family formats. These are the historical CamBANG display/
// materialization formats and remain the only formats the Godot `Image`
// materialization path accepts directly.
inline constexpr uint32_t FOURCC_RGBA = make_fourcc('R', 'G', 'B', 'A');
inline constexpr uint32_t FOURCC_BGRA = make_fourcc('B', 'G', 'R', 'A');

// Packed YUV 4:2:2 formats (one plane, two bytes per pixel).
inline constexpr uint32_t FOURCC_YUY2 = make_fourcc('Y', 'U', 'Y', '2');
inline constexpr uint32_t FOURCC_UYVY = make_fourcc('U', 'Y', 'V', 'Y');

// Semi-planar YUV 4:2:0 formats (luma plane + interleaved chroma plane).
inline constexpr uint32_t FOURCC_NV12 = make_fourcc('N', 'V', '1', '2');
inline constexpr uint32_t FOURCC_NV21 = make_fourcc('N', 'V', '2', '1');

// Fully planar YUV 4:2:0 formats (luma plane + two chroma planes).
inline constexpr uint32_t FOURCC_I420 = make_fourcc('I', '4', '2', '0');
inline constexpr uint32_t FOURCC_YV12 = make_fourcc('Y', 'V', '1', '2');

// Largest plane count any descriptor below reports.
inline constexpr uint32_t kMaxPixelFormatPlanes = 3u;

enum class PixelLayoutClass : uint8_t {
  // All components interleaved in a single plane.
  Packed = 0,
  // Luma plane plus one interleaved two-component chroma plane.
  SemiPlanar = 1,
  // Luma plane plus two separate single-component chroma planes.
  Planar = 2,
};

// Layout facts for one named pixel format.
//
// `chroma_shift_x/y` are log2 subsampling factors: 0 means chroma is at full
// resolution on that axis, 1 means half. They are 0 for packed RGB formats,
// which have no separate chroma plane.
struct PixelFormatDescriptor {
  uint32_t fourcc = 0;
  PixelLayoutClass layout_class = PixelLayoutClass::Packed;
  uint8_t plane_count = 0;
  uint8_t bits_per_component = 0;
  uint8_t chroma_shift_x = 0;
  uint8_t chroma_shift_y = 0;
  // Bytes occupied by one pixel column in plane 0. For packed formats this is
  // the whole pixel (4 for RGBA, 2 for YUY2); for planar/semi-planar formats
  // it is one luma sample.
  uint8_t plane0_bytes_per_sample = 0;
  bool is_yuv = false;
  // Chroma component order. NV21 and YV12 carry V before U, and nothing in the
  // plane geometry reveals that -- a converter reading them as NV12/I420
  // swaps red and blue, which looks like a plausible image rather than a
  // failure. A real Galaxy S20+ delivers NV21, so this is the common case on
  // Android, not an exotic one.
  bool chroma_v_first = false;
  bool has_alpha = false;
  // False for any FourCC this table does not name.
  bool valid = false;
};

// Look up layout facts for a format tag.
//
// An unknown tag yields a descriptor with `valid == false`; callers must treat
// that as "CamBANG cannot reason about this payload's geometry" and fail
// closed rather than guessing.
constexpr PixelFormatDescriptor describe_pixel_format(uint32_t fourcc) noexcept {
  PixelFormatDescriptor d{};
  d.fourcc = fourcc;

  if (fourcc == FOURCC_RGBA || fourcc == FOURCC_BGRA) {
    d.layout_class = PixelLayoutClass::Packed;
    d.plane_count = 1;
    d.bits_per_component = 8;
    d.plane0_bytes_per_sample = 4;
    d.is_yuv = false;
    d.has_alpha = true;
    d.valid = true;
    return d;
  }

  if (fourcc == FOURCC_YUY2 || fourcc == FOURCC_UYVY) {
    d.layout_class = PixelLayoutClass::Packed;
    d.plane_count = 1;
    d.bits_per_component = 8;
    // 4:2:2 packed: two pixels share one chroma pair, so two bytes per pixel.
    d.chroma_shift_x = 1;
    d.chroma_shift_y = 0;
    d.plane0_bytes_per_sample = 2;
    d.is_yuv = true;
    d.valid = true;
    return d;
  }

  if (fourcc == FOURCC_NV12 || fourcc == FOURCC_NV21) {
    d.chroma_v_first = (fourcc == FOURCC_NV21);
    d.layout_class = PixelLayoutClass::SemiPlanar;
    d.plane_count = 2;
    d.bits_per_component = 8;
    d.chroma_shift_x = 1;
    d.chroma_shift_y = 1;
    d.plane0_bytes_per_sample = 1;
    d.is_yuv = true;
    d.valid = true;
    return d;
  }

  if (fourcc == FOURCC_I420 || fourcc == FOURCC_YV12) {
    d.chroma_v_first = (fourcc == FOURCC_YV12);
    d.layout_class = PixelLayoutClass::Planar;
    d.plane_count = 3;
    d.bits_per_component = 8;
    d.chroma_shift_x = 1;
    d.chroma_shift_y = 1;
    d.plane0_bytes_per_sample = 1;
    d.is_yuv = true;
    d.valid = true;
    return d;
  }

  return d;
}

// Convenience: a format CamBANG names at all.
constexpr bool is_known_pixel_format(uint32_t fourcc) noexcept {
  return describe_pixel_format(fourcc).valid;
}

// Convenience: a packed RGB-family buffer. This is the narrow set the Godot
// `Image` materialization path and the current GPU display bridge accept
// directly, so it is the gate those fail-closed checks use.
constexpr bool is_packed_rgb_format(uint32_t fourcc) noexcept {
  const PixelFormatDescriptor d = describe_pixel_format(fourcc);
  return d.valid && d.layout_class == PixelLayoutClass::Packed && !d.is_yuv;
}

namespace pixel_format_detail {

// Ceiling division by 2^shift. Subsampled planes round up so odd dimensions
// keep a full chroma sample rather than silently dropping the last column/row.
constexpr uint32_t shift_ceil(uint32_t value, uint8_t shift) noexcept {
  const uint32_t divisor = 1u << shift;
  return (value + divisor - 1u) / divisor;
}

} // namespace pixel_format_detail

// Minimum bytes in one row of `plane` at the given image width, ignoring any
// provider padding. Returns 0 for an invalid descriptor or out-of-range plane.
constexpr uint32_t plane_row_bytes(const PixelFormatDescriptor& d,
                                   uint32_t plane,
                                   uint32_t width) noexcept {
  if (!d.valid || plane >= d.plane_count) {
    return 0;
  }
  if (plane == 0) {
    return width * static_cast<uint32_t>(d.plane0_bytes_per_sample);
  }
  const uint32_t chroma_width = pixel_format_detail::shift_ceil(width, d.chroma_shift_x);
  // A semi-planar chroma plane interleaves both components in one row.
  return (d.layout_class == PixelLayoutClass::SemiPlanar) ? (chroma_width * 2u) : chroma_width;
}

// Number of rows in `plane` at the given image height. Returns 0 for an
// invalid descriptor or out-of-range plane.
constexpr uint32_t plane_rows(const PixelFormatDescriptor& d,
                              uint32_t plane,
                              uint32_t height) noexcept {
  if (!d.valid || plane >= d.plane_count) {
    return 0;
  }
  if (plane == 0) {
    return height;
  }
  return pixel_format_detail::shift_ceil(height, d.chroma_shift_y);
}

// Total bytes for a tightly packed (zero padding) buffer of this format at
// width x height. Returns 0 for an invalid descriptor.
//
// Overflow-safe: accumulates in size_t and reports 0 if the product would
// exceed representable size on this platform.
constexpr size_t min_tight_size_bytes(const PixelFormatDescriptor& d,
                                      uint32_t width,
                                      uint32_t height) noexcept {
  if (!d.valid || width == 0 || height == 0) {
    return 0;
  }
  // plane_row_bytes() multiplies within uint32_t; reject widths where that
  // could wrap before trusting any row figure derived from it.
  if (d.plane0_bytes_per_sample == 0 ||
      width > (static_cast<uint32_t>(-1) / d.plane0_bytes_per_sample)) {
    return 0;
  }
  size_t total = 0;
  for (uint32_t plane = 0; plane < d.plane_count; ++plane) {
    const size_t row_bytes = static_cast<size_t>(plane_row_bytes(d, plane, width));
    const size_t rows = static_cast<size_t>(plane_rows(d, plane, height));
    if (row_bytes == 0 || rows == 0) {
      return 0;
    }
    if (row_bytes > (static_cast<size_t>(-1) / rows)) {
      return 0;
    }
    const size_t plane_bytes = row_bytes * rows;
    if (plane_bytes > (static_cast<size_t>(-1) - total)) {
      return 0;
    }
    total += plane_bytes;
  }
  return total;
}

// Overload taking a raw tag, for call sites that have not already resolved a
// descriptor.
constexpr size_t min_tight_size_bytes(uint32_t fourcc,
                                      uint32_t width,
                                      uint32_t height) noexcept {
  return min_tight_size_bytes(describe_pixel_format(fourcc), width, height);
}

} // namespace cambang
