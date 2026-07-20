#include "godot/cambang_result_convert.h"

#include <cstring>
#include <cstddef>
#include <cstdint>

namespace cambang {

namespace {
int to_prov_int(ResultFactProvenance v) {
  return static_cast<int>(v);
}

godot::PackedByteArray payload_rgba_to_pba(
    const CoreResultPayloadCpuPacked& payload,
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
    const CoreResultPayloadCpuPacked& payload,
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

godot::Dictionary to_dict(const ResultCaptureAttributesFacts& v) {
  godot::Dictionary d;
  d["exposure_time_ns"] = v.exposure_time_ns;
  d["aperture_f_number"] = v.aperture_f_number;
  d["focal_length_mm"] = v.focal_length_mm;
  d["focus_distance_m"] = v.focus_distance_m;
  d["sensor_sensitivity_iso_equivalent"] = v.sensor_sensitivity_iso_equivalent;
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

godot::Dictionary to_dict(const ResultCaptureAttributesProvenance& v) {
  godot::Dictionary d;
  d["exposure_time_ns"] = to_prov_int(v.exposure_time_ns);
  d["aperture_f_number"] = to_prov_int(v.aperture_f_number);
  d["focal_length_mm"] = to_prov_int(v.focal_length_mm);
  d["focus_distance_m"] = to_prov_int(v.focus_distance_m);
  d["sensor_sensitivity_iso_equivalent"] = to_prov_int(v.sensor_sensitivity_iso_equivalent);
  return d;
}

godot::Ref<godot::Image> payload_to_image(const CoreResultPayloadCpuPacked& payload) {
  if (payload.width == 0 || payload.height == 0 || payload.empty()) {
    return godot::Ref<godot::Image>();
  }

  if (payload.format_fourcc != FOURCC_RGBA && payload.format_fourcc != FOURCC_BGRA) {
    return godot::Ref<godot::Image>();
  }

  const size_t required_bytes =
      static_cast<size_t>(payload.width) * static_cast<size_t>(payload.height) * 4u;
  if (payload.stride_bytes != payload.width * 4u || payload.size_bytes() < required_bytes) {
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
