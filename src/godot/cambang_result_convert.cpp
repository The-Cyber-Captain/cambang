#include "godot/cambang_result_convert.h"

#include "pixels/format/yuv_convert.h"

#include <cstring>
#include <cstddef>
#include <cstdint>

namespace cambang {

namespace {
int to_prov_int(ResultFactProvenance v) {
  return static_cast<int>(v);
}

godot::PackedByteArray payload_rgba_to_pba(
    const CoreResultPayloadCpu& payload,
    size_t required_bytes) {
  godot::PackedByteArray out;
  out.resize(static_cast<int64_t>(required_bytes));
  if (required_bytes == 0) {
    return out;
  }

  std::memcpy(out.ptrw(), payload.data(), required_bytes);
  return out;
}

godot::PackedByteArray payload_bgra_to_rgba_pba(
    const CoreResultPayloadCpu& payload,
    size_t required_bytes) {
  godot::PackedByteArray out;
  out.resize(static_cast<int64_t>(required_bytes));
  if (required_bytes == 0) {
    return out;
  }

  uint8_t* dst = out.ptrw();
  const uint8_t* src = payload.data();
  for (size_t i = 0; i + 3 < required_bytes; i += 4) {
    dst[i] = src[i + 2];
    dst[i + 1] = src[i + 1];
    dst[i + 2] = src[i];
    dst[i + 3] = 255;
  }
  return out;
}

} // namespace

godot::Dictionary to_dict(const ResultImagePropertiesFacts& v) {
  godot::Dictionary d;
  d["width"] = static_cast<int64_t>(v.width);
  d["height"] = static_cast<int64_t>(v.height);
  d["format"] = static_cast<int64_t>(v.format);
  d["orientation"] = v.orientation;
  d["bit_depth"] = static_cast<int64_t>(v.bit_depth);
  return d;
}

godot::Dictionary to_dict(const ResultImagePropertiesProvenance& v) {
  godot::Dictionary d;
  d["width"] = to_prov_int(v.width);
  d["height"] = to_prov_int(v.height);
  d["format"] = to_prov_int(v.format);
  d["orientation"] = to_prov_int(v.orientation);
  d["bit_depth"] = to_prov_int(v.bit_depth);
  return d;
}

// NV12 -> a packed RGBA8 Image, for explicit materialization.
//
// Uses the same shared BT.601 maths as the display path, so to_image() and
// get_display_view() cannot disagree about colour. This is deliberately a
// full-frame conversion performed on demand: it is why planar to_image()
// classifies as EXPENSIVE rather than CHEAP.
godot::Ref<godot::Image> planar_payload_to_image(const CoreResultPayloadCpu& payload) {
  if (payload.format_fourcc != FOURCC_NV12) {
    return godot::Ref<godot::Image>();
  }
  if (!is_convertible_colorimetry(payload.colorimetry)) {
    return godot::Ref<godot::Image>();
  }
  const uint8_t* y_plane = payload.plane_data(0);
  const uint8_t* uv_plane = payload.plane_data(1);
  if (!y_plane || !uv_plane) {
    return godot::Ref<godot::Image>();
  }
  const uint32_t w = payload.width;
  const uint32_t h = payload.height;
  const uint32_t y_stride = payload.planes[0].stride_bytes;
  const uint32_t uv_stride = payload.planes[1].stride_bytes;
  if (y_stride < w || uv_stride < w ||
      payload.planes[0].rows < h ||
      payload.planes[1].rows < ((h + 1u) / 2u)) {
    return godot::Ref<godot::Image>();
  }

  godot::PackedByteArray out;
  out.resize(static_cast<int64_t>(w) * static_cast<int64_t>(h) * 4);
  uint8_t* dst = out.ptrw();
  if (!dst) {
    return godot::Ref<godot::Image>();
  }

  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* y_row = y_plane + static_cast<size_t>(y_stride) * y;
    const uint8_t* uv_row = uv_plane + static_cast<size_t>(uv_stride) * (y / 2u);
    uint8_t* row_out = dst + static_cast<size_t>(w) * 4u * y;
    for (uint32_t x = 0; x < w; ++x) {
      const size_t uv_index = static_cast<size_t>(x / 2u) * 2u;
      const RgbSample s =
          yuv_to_rgb_bt601_limited(y_row[x], uv_row[uv_index], uv_row[uv_index + 1u]);
      row_out[static_cast<size_t>(x) * 4u + 0u] = s.r;
      row_out[static_cast<size_t>(x) * 4u + 1u] = s.g;
      row_out[static_cast<size_t>(x) * 4u + 2u] = s.b;
      row_out[static_cast<size_t>(x) * 4u + 3u] = 255u;
    }
  }

  return godot::Image::create_from_data(
      static_cast<int>(w), static_cast<int>(h), false, godot::Image::FORMAT_RGBA8, out);
}

godot::Ref<godot::Image> payload_to_image(const CoreResultPayloadCpu& payload) {
  if (payload.width == 0 || payload.height == 0 || payload.empty()) {
    return godot::Ref<godot::Image>();
  }

  // Planar payloads have no directly usable byte representation; they are
  // converted on demand above.
  if (payload.is_planar()) {
    return planar_payload_to_image(payload);
  }

  // FORMAT_RGBA8 is built directly from these bytes, so only a packed
  // RGB-family payload is admissible here. Anything else -- including a packed
  // YUV payload such as YUY2 -- must fail closed rather than be reinterpreted.
  if (!is_packed_rgb_format(payload.format_fourcc)) {
    return godot::Ref<godot::Image>();
  }

  const PixelFormatDescriptor desc = describe_pixel_format(payload.format_fourcc);
  const size_t required_bytes = min_tight_size_bytes(desc, payload.width, payload.height);
  if (required_bytes == 0 ||
      payload.stride_bytes != plane_row_bytes(desc, 0, payload.width) ||
      payload.size_bytes() < required_bytes) {
    return godot::Ref<godot::Image>();
  }

  godot::PackedByteArray bytes = payload.format_fourcc == FOURCC_RGBA
      ? payload_rgba_to_pba(payload, required_bytes)
      : payload_bgra_to_rgba_pba(payload, required_bytes);

  return godot::Image::create_from_data(
      static_cast<int>(payload.width),
      static_cast<int>(payload.height),
      false,
      godot::Image::FORMAT_RGBA8,
      bytes);
}

} // namespace cambang
