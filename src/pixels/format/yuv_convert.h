// src/pixels/format/yuv_convert.h
#pragma once

#include <cstdint>

// One definition of CamBANG's YUV <-> RGB maths.
//
// Every path that converts uses these: the SyntheticProvider's forward
// transform, the CPU display path's inverse, and any future GPU shader, which
// must be written to match. Two hand-written coefficient sets in different
// translation units cannot be kept in agreement by comment alone, and a
// mismatch produces a plausible-looking image rather than an obvious failure.
//
// Currently only BT.601 limited range ("video" range) is implemented, which is
// what Camera2's YUV_420_888 output uses and what SyntheticProvider emits. The
// names say so explicitly: a caller holding content in another colour space
// must not reach for these by default. Add a second pair, and a colorimetry
// dispatcher, when a second colour space actually arrives -- not before.
//
// Fixed point at 1/256 so a full frame stays integer-only and therefore
// bit-reproducible across platforms and between the CPU and GPU paths.

namespace cambang {

namespace yuv_detail {

constexpr uint8_t clamp_u8(int32_t v) noexcept {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

} // namespace yuv_detail

struct YuvSample {
  uint8_t y = 0;
  uint8_t u = 0;
  uint8_t v = 0;
};

struct RgbSample {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

// RGB -> YUV, BT.601 limited range.
//
// For in-gamut 8-bit RGB this yields Y in [16,235] and U/V in [16,240]; the
// clamps guard the arithmetic rather than the expected range.
constexpr YuvSample rgb_to_yuv_bt601_limited(uint8_t r, uint8_t g, uint8_t b) noexcept {
  const int32_t ri = static_cast<int32_t>(r);
  const int32_t gi = static_cast<int32_t>(g);
  const int32_t bi = static_cast<int32_t>(b);
  YuvSample out{};
  out.y = yuv_detail::clamp_u8(((66 * ri + 129 * gi + 25 * bi + 128) >> 8) + 16);
  out.u = yuv_detail::clamp_u8(((-38 * ri - 74 * gi + 112 * bi + 128) >> 8) + 128);
  out.v = yuv_detail::clamp_u8(((112 * ri - 94 * gi - 18 * bi + 128) >> 8) + 128);
  return out;
}

// YUV -> RGB, BT.601 limited range. Inverse of the above.
//
// Not an exact inverse: both directions quantize to 8 bits and 4:2:0 chroma is
// subsampled, so a round trip is lossy by construction. Tests must compare
// within a tolerance, never for equality.
constexpr RgbSample yuv_to_rgb_bt601_limited(uint8_t y, uint8_t u, uint8_t v) noexcept {
  const int32_t c = static_cast<int32_t>(y) - 16;
  const int32_t d = static_cast<int32_t>(u) - 128;
  const int32_t e = static_cast<int32_t>(v) - 128;
  RgbSample out{};
  out.r = yuv_detail::clamp_u8((298 * c + 409 * e + 128) >> 8);
  out.g = yuv_detail::clamp_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
  out.b = yuv_detail::clamp_u8((298 * c + 516 * d + 128) >> 8);
  return out;
}

// YUV -> RGB, BT.601 FULL range (JFIF).
//
// Distinct from the limited-range function above, not a parameterisation of it,
// for the reason this file already follows: a caller holding content in one
// range must not be able to reach for the other by default.
//
// The difference is not cosmetic. Limited range maps Y 16..235 onto 0..255, so
// applying it to full-range data clamps everything outside that window --
// crushed blacks and blown highlights. Camera2 on measured hardware declares
// ADATASPACE_JFIF, which is this function's domain, not the one above's; see
// pixel_payload_and_result_contract.md 6.3.1.
//
// Full range needs no black-level offset and no 255/219 scaling: Y passes
// through, and only the chroma terms are weighted. Fixed point at 1/256 to
// match the limited-range path, so both stay integer-only and reproducible.
constexpr RgbSample yuv_to_rgb_bt601_full(uint8_t y, uint8_t u, uint8_t v) noexcept {
  const int32_t yi = static_cast<int32_t>(y);
  const int32_t d = static_cast<int32_t>(u) - 128;
  const int32_t e = static_cast<int32_t>(v) - 128;
  RgbSample out{};
  // 1.402, 0.344136, 0.714136, 1.772 at 1/256.
  out.r = yuv_detail::clamp_u8(yi + ((359 * e + 128) >> 8));
  out.g = yuv_detail::clamp_u8(yi + ((-88 * d - 183 * e + 128) >> 8));
  out.b = yuv_detail::clamp_u8(yi + ((454 * d + 128) >> 8));
  return out;
}

} // namespace cambang
