#include "godot/cambang_stream_result.h"

#include <cstdlib>

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "godot/cambang_result_convert.h"
#include "godot/synthetic_gpu_backing_bridge.h"

namespace cambang {

namespace {

bool stream_display_trace_enabled() {
  const char* value = std::getenv("CAMBANG_DEV_SYNTH_GPU_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}

void trace_stream_display_path(const char* path) {
  if (!stream_display_trace_enabled()) {
    return;
  }
  godot::UtilityFunctions::print("[CamBANG][StreamResult] display_view_path=", path);
}

} // namespace

uint32_t CamBANGStreamResult::get_width() const { return data_ ? data_->payload.width : 0; }
uint32_t CamBANGStreamResult::get_height() const { return data_ ? data_->payload.height : 0; }
uint32_t CamBANGStreamResult::get_format() const { return data_ ? data_->payload.format_fourcc : 0; }
int CamBANGStreamResult::get_payload_kind() const {
  return data_ ? static_cast<int>(data_->payload_kind) : static_cast<int>(ResultPayloadKind::CPU_PACKED);
}
uint64_t CamBANGStreamResult::get_capture_timestamp() const { return data_ ? data_->capture_timestamp_ns : 0; }
uint64_t CamBANGStreamResult::get_stream_id() const { return data_ ? data_->stream_id : 0; }
uint64_t CamBANGStreamResult::get_device_instance_id() const { return data_ ? data_->device_instance_id : 0; }
int CamBANGStreamResult::get_intent() const { return data_ ? static_cast<int>(data_->intent) : 0; }

bool CamBANGStreamResult::has_image_properties() const { return data_ && data_->facts.has_image_properties; }
bool CamBANGStreamResult::has_capture_attributes() const { return data_ && data_->facts.has_capture_attributes; }
bool CamBANGStreamResult::has_location_attributes() const { return data_ && data_->facts.has_location_attributes; }
bool CamBANGStreamResult::has_optical_calibration() const { return data_ && data_->facts.has_optical_calibration; }

godot::Dictionary CamBANGStreamResult::get_image_properties() const {
  return has_image_properties() ? to_dict(data_->facts.image_properties) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_capture_attributes() const {
  return has_capture_attributes() ? to_dict(data_->facts.capture_attributes) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_location_attributes() const {
  return has_location_attributes() ? to_dict(data_->facts.location_attributes) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_optical_calibration() const {
  return has_optical_calibration() ? to_dict(data_->facts.optical_calibration) : godot::Dictionary();
}

godot::Dictionary CamBANGStreamResult::get_image_properties_provenance() const {
  return has_image_properties() ? to_dict(data_->facts.image_properties_provenance) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_capture_attributes_provenance() const {
  return has_capture_attributes() ? to_dict(data_->facts.capture_attributes_provenance) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_location_attributes_provenance() const {
  return has_location_attributes() ? to_dict(data_->facts.location_attributes_provenance) : godot::Dictionary();
}
godot::Dictionary CamBANGStreamResult::get_optical_calibration_provenance() const {
  return has_optical_calibration() ? to_dict(data_->facts.optical_calibration_provenance) : godot::Dictionary();
}

int CamBANGStreamResult::can_get_display_view() const {
  if (!data_) {
    return CAPABILITY_UNSUPPORTED;
  }
  if (data_->payload_kind == ResultPayloadKind::GPU_SURFACE && data_->retained_gpu_backing) {
    return CAPABILITY_READY;
  }
  return CAPABILITY_CHEAP;
}

int CamBANGStreamResult::can_to_image() const {
  if (!data_) {
    return CAPABILITY_UNSUPPORTED;
  }
  return CAPABILITY_CHEAP;
}

godot::Variant CamBANGStreamResult::get_display_view() const {
  if (!data_) {
    return godot::Variant();
  }
  if (data_->payload_kind == ResultPayloadKind::GPU_SURFACE && data_->retained_gpu_backing) {
    godot::Ref<godot::Texture2D> retained = synthetic_gpu_backing_display_texture(data_->retained_gpu_backing);
    if (retained.is_valid()) {
      trace_stream_display_path("retained_gpu_backing");
      return retained;
    }
  }
  if (!cached_display_view_.is_valid()) {
    godot::Ref<godot::Image> image = to_image();
    if (image.is_valid()) {
      cached_display_view_ = godot::ImageTexture::create_from_image(image);
    }
  }
  if (cached_display_view_.is_valid()) {
    trace_stream_display_path("cpu_texture_fallback");
    return cached_display_view_;
  }
  return godot::Variant();
}

godot::Ref<godot::Image> CamBANGStreamResult::to_image() const {
  if (!data_) {
    return godot::Ref<godot::Image>();
  }
  return payload_to_image(data_->payload);
}

void CamBANGStreamResult::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_width"), &CamBANGStreamResult::get_width);
  godot::ClassDB::bind_method(godot::D_METHOD("get_height"), &CamBANGStreamResult::get_height);
  godot::ClassDB::bind_method(godot::D_METHOD("get_format"), &CamBANGStreamResult::get_format);
  godot::ClassDB::bind_method(godot::D_METHOD("get_payload_kind"), &CamBANGStreamResult::get_payload_kind);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_timestamp"), &CamBANGStreamResult::get_capture_timestamp);
  godot::ClassDB::bind_method(godot::D_METHOD("get_stream_id"), &CamBANGStreamResult::get_stream_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_device_instance_id"), &CamBANGStreamResult::get_device_instance_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_intent"), &CamBANGStreamResult::get_intent);

  godot::ClassDB::bind_method(godot::D_METHOD("has_image_properties"), &CamBANGStreamResult::has_image_properties);
  godot::ClassDB::bind_method(godot::D_METHOD("has_capture_attributes"), &CamBANGStreamResult::has_capture_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("has_location_attributes"), &CamBANGStreamResult::has_location_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("has_optical_calibration"), &CamBANGStreamResult::has_optical_calibration);

  godot::ClassDB::bind_method(godot::D_METHOD("get_image_properties"), &CamBANGStreamResult::get_image_properties);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_attributes"), &CamBANGStreamResult::get_capture_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("get_location_attributes"), &CamBANGStreamResult::get_location_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("get_optical_calibration"), &CamBANGStreamResult::get_optical_calibration);

  godot::ClassDB::bind_method(godot::D_METHOD("get_image_properties_provenance"), &CamBANGStreamResult::get_image_properties_provenance);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_attributes_provenance"), &CamBANGStreamResult::get_capture_attributes_provenance);
  godot::ClassDB::bind_method(godot::D_METHOD("get_location_attributes_provenance"), &CamBANGStreamResult::get_location_attributes_provenance);
  godot::ClassDB::bind_method(godot::D_METHOD("get_optical_calibration_provenance"), &CamBANGStreamResult::get_optical_calibration_provenance);

  godot::ClassDB::bind_method(godot::D_METHOD("can_get_display_view"), &CamBANGStreamResult::can_get_display_view);
  godot::ClassDB::bind_method(godot::D_METHOD("can_to_image"), &CamBANGStreamResult::can_to_image);

  godot::ClassDB::bind_method(godot::D_METHOD("get_display_view"), &CamBANGStreamResult::get_display_view);
  godot::ClassDB::bind_method(godot::D_METHOD("to_image"), &CamBANGStreamResult::to_image);

  BIND_CONSTANT(CAPABILITY_READY);
  BIND_CONSTANT(CAPABILITY_CHEAP);
  BIND_CONSTANT(CAPABILITY_EXPENSIVE);
  BIND_CONSTANT(CAPABILITY_UNSUPPORTED);
}

} // namespace cambang
