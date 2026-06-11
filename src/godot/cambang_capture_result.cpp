#include "godot/cambang_capture_result.h"

#include "godot/cambang_result_convert.h"

namespace cambang {

uint32_t CamBANGCaptureResult::get_width() const { return data_ ? data_->image_width : 0; }
uint32_t CamBANGCaptureResult::get_height() const { return data_ ? data_->image_height : 0; }
uint32_t CamBANGCaptureResult::get_format() const { return data_ ? data_->image_format_fourcc : 0; }
int CamBANGCaptureResult::get_payload_kind() const {
  return data_ ? static_cast<int>(data_->payload_kind) : static_cast<int>(ResultPayloadKind::CPU_PACKED);
}
uint64_t CamBANGCaptureResult::get_capture_timestamp() const { return data_ ? data_->default_image.capture_timestamp_ns : 0; }
uint64_t CamBANGCaptureResult::get_device_instance_id() const { return data_ ? data_->device_instance_id : 0; }
uint64_t CamBANGCaptureResult::get_capture_id() const { return data_ ? data_->capture_id : 0; }

bool CamBANGCaptureResult::has_image_properties() const { return data_ && data_->facts.has_image_properties; }
bool CamBANGCaptureResult::has_capture_attributes() const { return data_ && data_->default_image.has_capture_attributes; }
bool CamBANGCaptureResult::has_location_attributes() const { return data_ && data_->facts.has_location_attributes; }
bool CamBANGCaptureResult::has_optical_calibration() const { return data_ && data_->facts.has_optical_calibration; }

godot::Dictionary CamBANGCaptureResult::get_image_properties() const {
  return has_image_properties() ? to_dict(data_->facts.image_properties) : godot::Dictionary();
}
godot::Dictionary CamBANGCaptureResult::get_capture_attributes() const {
  return has_capture_attributes() ? to_dict(data_->default_image.capture_attributes) : godot::Dictionary();
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
  return has_capture_attributes() ? to_dict(data_->default_image.capture_attributes_provenance) : godot::Dictionary();
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
  // CaptureResult.can_to_image() delegates to
  // CaptureResult.can_to_image_member(index). These are capability/cost
  // classification APIs, not readiness/progress APIs. Future lower-level
  // materialization infrastructure can be shared with StreamResult.to_image(),
  // while CaptureResult.to_image() and
  // CaptureResult.to_image_member(index) must retain capture/member identity
  // validation.
  return can_to_image_member(0);
}

int CamBANGCaptureResult::get_image_count() const {
  return data_ ? static_cast<int>(data_->image_member_count()) : 0;
}

bool CamBANGCaptureResult::has_additional_images() const {
  return data_ && data_->has_additional_images();
}

godot::Dictionary CamBANGCaptureResult::get_image_member(int image_member_index) const {
  if (!data_ || image_member_index < 0) {
    return godot::Dictionary();
  }
  const auto* member = data_->image_member_at(static_cast<uint32_t>(image_member_index));
  if (!member) {
    return godot::Dictionary();
  }
  godot::Dictionary out;
  const int role = static_cast<int>(member->role);
  out["image_member_index"] = static_cast<int64_t>(member->image_member_index);
  out["role"] = role;
  out["role_name"] = (member->role == CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED)
      ? godot::String("DEFAULT_METERED")
      : godot::String("ADDITIONAL_BRACKET");
  out["capture_timestamp"] = static_cast<int64_t>(member->capture_timestamp_ns);
  out["applied_exposure_compensation_milli_ev"] = static_cast<int64_t>(member->applied_exposure_compensation_milli_ev);
  out["has_realized_exposure_compensation_milli_ev"] = member->has_realized_exposure_compensation_milli_ev;
  out["realized_exposure_compensation_milli_ev"] = static_cast<int64_t>(member->realized_exposure_compensation_milli_ev);
  out["is_default"] = (member->role == CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED);
  out["is_additional_bracket"] = (member->role == CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET);
  return out;
}

int CamBANGCaptureResult::can_to_image_member(int image_member_index) const {
  // CaptureResult.can_to_image_member(index) returns CHEAP only when the
  // requested member already has a retained CPU representation. EXPENSIVE is
  // reserved for a later safe explicit materialization route; unsupported
  // members or members with no safe route remain UNSUPPORTED.
  if (!data_ || image_member_index < 0) {
    return CAPABILITY_UNSUPPORTED;
  }
  const auto* member = data_->image_member_at(static_cast<uint32_t>(image_member_index));
  if (!member) {
    return CAPABILITY_UNSUPPORTED;
  }
  if (member->payload.width == 0 || member->payload.height == 0 || member->payload.empty()) {
    return CAPABILITY_UNSUPPORTED;
  }
  if (member->payload.format_fourcc != FOURCC_RGBA && member->payload.format_fourcc != FOURCC_BGRA) {
    return CAPABILITY_UNSUPPORTED;
  }
  return CAPABILITY_CHEAP;
}

godot::Ref<godot::Image> CamBANGCaptureResult::to_image_member(int image_member_index) const {
  if (!data_ || image_member_index < 0) {
    return godot::Ref<godot::Image>();
  }
  const auto* member = data_->image_member_at(static_cast<uint32_t>(image_member_index));
  if (!member) {
    return godot::Ref<godot::Image>();
  }
  return payload_to_image(member->payload);
}

int CamBANGCaptureResult::can_get_encoded_bytes() const {
  return CAPABILITY_UNSUPPORTED;
}

godot::Variant CamBANGCaptureResult::get_display_view() const {
  return to_image();
}

godot::Ref<godot::Image> CamBANGCaptureResult::to_image() const {
  return to_image_member(0);
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
  godot::ClassDB::bind_method(godot::D_METHOD("get_image_count"), &CamBANGCaptureResult::get_image_count);
  godot::ClassDB::bind_method(godot::D_METHOD("has_additional_images"), &CamBANGCaptureResult::has_additional_images);
  godot::ClassDB::bind_method(godot::D_METHOD("get_image_member", "image_member_index"), &CamBANGCaptureResult::get_image_member);
  godot::ClassDB::bind_method(godot::D_METHOD("can_to_image_member", "image_member_index"), &CamBANGCaptureResult::can_to_image_member);
  godot::ClassDB::bind_method(godot::D_METHOD("to_image_member", "image_member_index"), &CamBANGCaptureResult::to_image_member);
  godot::ClassDB::bind_method(godot::D_METHOD("can_get_encoded_bytes"), &CamBANGCaptureResult::can_get_encoded_bytes);

  godot::ClassDB::bind_method(godot::D_METHOD("get_display_view"), &CamBANGCaptureResult::get_display_view);
  godot::ClassDB::bind_method(godot::D_METHOD("to_image"), &CamBANGCaptureResult::to_image);
  godot::ClassDB::bind_method(godot::D_METHOD("get_encoded_bytes"), &CamBANGCaptureResult::get_encoded_bytes);

  BIND_CONSTANT(CAPABILITY_READY);
  BIND_CONSTANT(CAPABILITY_CHEAP);
  BIND_CONSTANT(CAPABILITY_EXPENSIVE);
  BIND_CONSTANT(CAPABILITY_UNSUPPORTED);
  BIND_CONSTANT(IMAGE_ROLE_DEFAULT_METERED);
  BIND_CONSTANT(IMAGE_ROLE_ADDITIONAL_BRACKET);
}

} // namespace cambang
