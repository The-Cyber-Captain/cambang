#include "godot/cambang_capture_result.h"

#include "godot/cambang_result_convert.h"

namespace cambang {

uint32_t CamBANGCaptureResult::get_width() const { return data_ ? data_->payload.width : 0; }
uint32_t CamBANGCaptureResult::get_height() const { return data_ ? data_->payload.height : 0; }
uint32_t CamBANGCaptureResult::get_format() const { return data_ ? data_->payload.format_fourcc : 0; }
int CamBANGCaptureResult::get_payload_kind() const {
  return data_ ? static_cast<int>(data_->payload_kind) : static_cast<int>(ResultPayloadKind::CPU_PACKED);
}
uint64_t CamBANGCaptureResult::get_capture_timestamp() const { return data_ ? data_->capture_timestamp_ns : 0; }
uint64_t CamBANGCaptureResult::get_device_instance_id() const { return data_ ? data_->device_instance_id : 0; }
uint64_t CamBANGCaptureResult::get_capture_id() const { return data_ ? data_->capture_id : 0; }

bool CamBANGCaptureResult::has_image_properties() const { return data_ && data_->facts.has_image_properties; }
bool CamBANGCaptureResult::has_capture_attributes() const { return data_ && data_->facts.has_capture_attributes; }
bool CamBANGCaptureResult::has_location_attributes() const { return data_ && data_->facts.has_location_attributes; }
bool CamBANGCaptureResult::has_optical_calibration() const { return data_ && data_->facts.has_optical_calibration; }

godot::Dictionary CamBANGCaptureResult::get_image_properties() const {
  return has_image_properties() ? to_dict(data_->facts.image_properties) : godot::Dictionary();
}
godot::Dictionary CamBANGCaptureResult::get_capture_attributes() const {
  return has_capture_attributes() ? to_dict(data_->facts.capture_attributes) : godot::Dictionary();
}
godot::Dictionary CamBANGCaptureResult::get_location_attributes() const {
  return has_location_attributes() ? to_dict(data_->facts.location_attributes) : godot::Dictionary();
}
godot::Dictionary CamBANGCaptureResult::get_optical_calibration() const {
  return has_optical_calibration() ? to_dict(data_->facts.optical_calibration) : godot::Dictionary();
}

godot::Dictionary CamBANGCaptureResult::get_image_properties_provenance() const {
  return has_image_properties() ? to_dict(data_->facts.image_properties_provenance) : godot::Dictionary();
}
godot::Dictionary CamBANGCaptureResult::get_capture_attributes_provenance() const {
  return has_capture_attributes() ? to_dict(data_->facts.capture_attributes_provenance) : godot::Dictionary();
}
godot::Dictionary CamBANGCaptureResult::get_location_attributes_provenance() const {
  return has_location_attributes() ? to_dict(data_->facts.location_attributes_provenance) : godot::Dictionary();
}
godot::Dictionary CamBANGCaptureResult::get_optical_calibration_provenance() const {
  return has_optical_calibration() ? to_dict(data_->facts.optical_calibration_provenance) : godot::Dictionary();
}

int CamBANGCaptureResult::can_get_display_view() const {
  return data_ ? CAPABILITY_READY : CAPABILITY_UNSUPPORTED;
}

int CamBANGCaptureResult::can_to_image() const {
  return data_ ? CAPABILITY_CHEAP : CAPABILITY_UNSUPPORTED;
}

int CamBANGCaptureResult::can_get_encoded_bytes() const {
  return CAPABILITY_UNSUPPORTED;
}

godot::Variant CamBANGCaptureResult::get_display_view() const {
  return to_image();
}

godot::Ref<godot::Image> CamBANGCaptureResult::to_image() const {
  if (!data_) {
    return godot::Ref<godot::Image>();
  }
  return payload_to_image(data_->payload);
}

godot::PackedByteArray CamBANGCaptureResult::get_encoded_bytes() const {
  return godot::PackedByteArray();
}

void CamBANGCaptureResult::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_width"), &CamBANGCaptureResult::get_width);
  godot::ClassDB::bind_method(godot::D_METHOD("get_height"), &CamBANGCaptureResult::get_height);
  godot::ClassDB::bind_method(godot::D_METHOD("get_format"), &CamBANGCaptureResult::get_format);
  godot::ClassDB::bind_method(godot::D_METHOD("get_payload_kind"), &CamBANGCaptureResult::get_payload_kind);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_timestamp"), &CamBANGCaptureResult::get_capture_timestamp);
  godot::ClassDB::bind_method(godot::D_METHOD("get_device_instance_id"), &CamBANGCaptureResult::get_device_instance_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_id"), &CamBANGCaptureResult::get_capture_id);

  godot::ClassDB::bind_method(godot::D_METHOD("has_image_properties"), &CamBANGCaptureResult::has_image_properties);
  godot::ClassDB::bind_method(godot::D_METHOD("has_capture_attributes"), &CamBANGCaptureResult::has_capture_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("has_location_attributes"), &CamBANGCaptureResult::has_location_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("has_optical_calibration"), &CamBANGCaptureResult::has_optical_calibration);

  godot::ClassDB::bind_method(godot::D_METHOD("get_image_properties"), &CamBANGCaptureResult::get_image_properties);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_attributes"), &CamBANGCaptureResult::get_capture_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("get_location_attributes"), &CamBANGCaptureResult::get_location_attributes);
  godot::ClassDB::bind_method(godot::D_METHOD("get_optical_calibration"), &CamBANGCaptureResult::get_optical_calibration);

  godot::ClassDB::bind_method(godot::D_METHOD("get_image_properties_provenance"), &CamBANGCaptureResult::get_image_properties_provenance);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_attributes_provenance"), &CamBANGCaptureResult::get_capture_attributes_provenance);
  godot::ClassDB::bind_method(godot::D_METHOD("get_location_attributes_provenance"), &CamBANGCaptureResult::get_location_attributes_provenance);
  godot::ClassDB::bind_method(godot::D_METHOD("get_optical_calibration_provenance"), &CamBANGCaptureResult::get_optical_calibration_provenance);

  godot::ClassDB::bind_method(godot::D_METHOD("can_get_display_view"), &CamBANGCaptureResult::can_get_display_view);
  godot::ClassDB::bind_method(godot::D_METHOD("can_to_image"), &CamBANGCaptureResult::can_to_image);
  godot::ClassDB::bind_method(godot::D_METHOD("can_get_encoded_bytes"), &CamBANGCaptureResult::can_get_encoded_bytes);

  godot::ClassDB::bind_method(godot::D_METHOD("get_display_view"), &CamBANGCaptureResult::get_display_view);
  godot::ClassDB::bind_method(godot::D_METHOD("to_image"), &CamBANGCaptureResult::to_image);
  godot::ClassDB::bind_method(godot::D_METHOD("get_encoded_bytes"), &CamBANGCaptureResult::get_encoded_bytes);

  BIND_CONSTANT(CAPABILITY_READY);
  BIND_CONSTANT(CAPABILITY_CHEAP);
  BIND_CONSTANT(CAPABILITY_EXPENSIVE);
  BIND_CONSTANT(CAPABILITY_UNSUPPORTED);
}

} // namespace cambang
