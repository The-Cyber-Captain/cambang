#pragma once

#include <cstdint>

#include "imaging/api/provider_contract_datatypes.h"
#include "pixels/format/pixel_format_descriptor.h"

namespace cambang {

// The native planes a still image member's GPU compute sidecar holds, derived
// from the member's CPU format and extent.
//
// One derivation, used by the producer that authors a sidecar descriptor and by
// Core when it decides whether to retain one, so the two cannot disagree. It
// reproduces the plane shapes the CPU upload route exposes
// (godot/compute_texture_planes.h): luma and fully planar chroma as R8,
// semi-planar chroma as RG8 in the member's own component order, a packed RGBA
// member as one RGBA8 plane. A format with no plane shape here -- packed BGRA or
// YUY2, which the upload route reaches only through conversion -- has no
// sidecar, and false is the truthful answer.
inline bool describe_gpu_compute_sidecar_planes(uint32_t format_fourcc,
                                                uint32_t width,
                                                uint32_t height,
                                                uint32_t& out_plane_count,
                                                GpuBackingPlaneDescriptor (&out_planes)[kMaxGpuBackingPlanes]) noexcept {
  out_plane_count = 0;
  for (GpuBackingPlaneDescriptor& plane : out_planes) {
    plane = GpuBackingPlaneDescriptor{};
  }
  if (width == 0 || height == 0) {
    return false;
  }
  if (format_fourcc == FOURCC_RGBA) {
    out_plane_count = 1;
    out_planes[0] = {GpuBackingPlaneFormat::RGBA8_UNORM, width, height};
    return true;
  }
  const PixelFormatDescriptor desc = describe_pixel_format(format_fourcc);
  if (!desc.valid || !desc.is_yuv || desc.layout_class == PixelLayoutClass::Packed ||
      desc.plane_count < 2 || desc.plane_count > kMaxGpuBackingPlanes ||
      desc.plane0_bytes_per_sample != 1 || desc.bits_per_component != 8) {
    return false;
  }
  const uint32_t chroma_w = width >> desc.chroma_shift_x;
  const uint32_t chroma_h = height >> desc.chroma_shift_y;
  if (chroma_w == 0 || chroma_h == 0) {
    return false;
  }
  out_plane_count = desc.plane_count;
  out_planes[0] = {GpuBackingPlaneFormat::R8_UNORM, width, height};
  const GpuBackingPlaneFormat chroma_format =
      desc.plane_count == 2 ? GpuBackingPlaneFormat::RG8_UNORM : GpuBackingPlaneFormat::R8_UNORM;
  for (uint32_t i = 1; i < desc.plane_count; ++i) {
    out_planes[i] = {chroma_format, chroma_w, chroma_h};
  }
  return true;
}

// Whether a sidecar descriptor has exactly the planes the member's CPU format
// and extent call for. Core retains a sidecar only when this holds; a sidecar
// whose planes differ would let the two compute-texture routes hand a consumer
// different textures for the same image.
inline bool gpu_compute_sidecar_descriptor_matches(const RetainedGpuBackingDescriptor& descriptor,
                                                   uint32_t format_fourcc,
                                                   uint32_t width,
                                                   uint32_t height) noexcept {
  if (!descriptor.valid || descriptor.plane_count == 0 ||
      !retained_gpu_backing_descriptor_is_self_consistent(descriptor) ||
      descriptor.width != width || descriptor.height != height) {
    return false;
  }
  uint32_t expected_count = 0;
  GpuBackingPlaneDescriptor expected[kMaxGpuBackingPlanes]{};
  if (!describe_gpu_compute_sidecar_planes(format_fourcc, width, height, expected_count, expected) ||
      expected_count != descriptor.plane_count) {
    return false;
  }
  for (uint32_t i = 0; i < expected_count; ++i) {
    if (expected[i].format != descriptor.planes[i].format ||
        expected[i].width != descriptor.planes[i].width ||
        expected[i].height != descriptor.planes[i].height) {
      return false;
    }
  }
  return true;
}

} // namespace cambang
