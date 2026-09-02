#pragma once

// Plane geometry and plane-to-Image extraction, shared by the capture and
// stream compute-texture surfaces.
//
// Shared rather than duplicated because these encode two things that must not
// diverge between the two surfaces: which single-plane format a given plane of
// a YUV family member is exposed as, and how a padded camera plane is de-padded
// on the way out. Independently re-derived copies of facts like these are the
// defect this contract already had to fix once (see 11.2).

#include <cstring>

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include "core/core_result_store.h"
#include "pixels/format/pixel_format_descriptor.h"

namespace cambang {

// Geometry of one plane as it will be exposed: a single-plane texture in the
// member's own format, never a converted one.
struct PlaneShape {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bytes_per_sample = 0;
  godot::Image::Format image_format = godot::Image::FORMAT_R8;
  bool valid = false;
};

inline PlaneShape describe_plane(const CoreResultPayloadCpu& payload, uint32_t plane_index) {
  PlaneShape shape{};
  const PixelFormatDescriptor desc = describe_pixel_format(payload.format_fourcc);
  if (!desc.valid || plane_index >= payload.plane_count) {
    return shape;
  }
  if (plane_index == 0) {
    // Luma, or the sole plane of anything single-plane.
    shape.width = payload.width;
    shape.height = payload.height;
    shape.bytes_per_sample = desc.plane0_bytes_per_sample;
  } else {
    shape.width = payload.width >> desc.chroma_shift_x;
    shape.height = payload.height >> desc.chroma_shift_y;
    // Semi-planar keeps both chroma components interleaved in one plane, so a
    // sample there is two bytes wide; fully planar splits them, one byte each.
    shape.bytes_per_sample = (desc.plane_count == 2) ? 2u : 1u;
  }
  if (shape.width == 0 || shape.height == 0) {
    return shape;
  }
  switch (shape.bytes_per_sample) {
    case 1: shape.image_format = godot::Image::FORMAT_R8; break;
    case 2: shape.image_format = godot::Image::FORMAT_RG8; break;
    default: return shape;  // stays invalid: no single-plane format fits
  }
  shape.valid = true;
  return shape;
}

// Copies one plane out of the retained buffer into a tightly packed image.
//
// The retained stride may exceed the tight row width -- camera buffers are
// commonly padded -- and Image::create_from_data requires tight packing, so a
// padded plane is copied row by row. Even then this moves one plane's bytes
// once, against the whole-frame colour conversion it replaces.
inline godot::Ref<godot::Image> plane_to_image(const CoreResultPayloadCpu& payload,
                                        uint32_t plane_index,
                                        const PlaneShape& shape) {
  const uint8_t* src = payload.plane_data(plane_index);
  if (!src || !shape.valid) {
    return {};
  }
  const CoreResultPayloadCpuPlane& plane = payload.planes[plane_index];
  const uint32_t tight_row = shape.width * shape.bytes_per_sample;
  if (tight_row == 0 || plane.stride_bytes < tight_row || plane.rows < shape.height) {
    return {};
  }
  godot::PackedByteArray bytes;
  bytes.resize(static_cast<int64_t>(tight_row) * static_cast<int64_t>(shape.height));
  uint8_t* dst = bytes.ptrw();
  if (!dst) {
    return {};
  }
  if (plane.stride_bytes == tight_row) {
    std::memcpy(dst, src, static_cast<size_t>(tight_row) * shape.height);
  } else {
    for (uint32_t row = 0; row < shape.height; ++row) {
      std::memcpy(dst + static_cast<size_t>(row) * tight_row,
                  src + static_cast<size_t>(row) * plane.stride_bytes,
                  tight_row);
    }
  }
  return godot::Image::create_from_data(static_cast<int32_t>(shape.width),
                                        static_cast<int32_t>(shape.height),
                                        false,
                                        shape.image_format,
                                        bytes);
}

} // namespace cambang
