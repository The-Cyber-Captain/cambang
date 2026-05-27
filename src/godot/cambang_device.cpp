#include "godot/cambang_device.h"

#include "godot/cambang_server.h"

#include "imaging/api/provider_contract_datatypes.h"

namespace {

using namespace cambang;

bool parse_role(const godot::Variant& role_variant, CaptureStillImageMemberRole& out_role) {
  if (role_variant.get_type() == godot::Variant::INT) {
    const int role_i = static_cast<int>(int64_t(role_variant));
    if (role_i == static_cast<int>(CaptureStillImageMemberRole::DEFAULT_METERED)) {
      out_role = CaptureStillImageMemberRole::DEFAULT_METERED;
      return true;
    }
    if (role_i == static_cast<int>(CaptureStillImageMemberRole::ADDITIONAL_BRACKET)) {
      out_role = CaptureStillImageMemberRole::ADDITIONAL_BRACKET;
      return true;
    }
    return false;
  }
  if (role_variant.get_type() == godot::Variant::STRING) {
    const godot::String role_s = role_variant;
    if (role_s == "DEFAULT_METERED") {
      out_role = CaptureStillImageMemberRole::DEFAULT_METERED;
      return true;
    }
    if (role_s == "ADDITIONAL_BRACKET") {
      out_role = CaptureStillImageMemberRole::ADDITIONAL_BRACKET;
      return true;
    }
  }
  return false;
}

bool parse_still_image_bundle_dict(const godot::Dictionary& profile,
                               CaptureStillImageBundle& out_sequence,
                               godot::Error& out_error) {
  out_sequence = make_default_metered_still_image_bundle();
  if (!profile.has("still_image_bundle")) {
    out_error = godot::OK;
    return true;
  }
  const godot::Variant seq_v = profile.get("still_image_bundle", godot::Variant());
  if (seq_v.get_type() != godot::Variant::DICTIONARY) {
    out_error = godot::ERR_INVALID_PARAMETER;
    return false;
  }
  const godot::Dictionary seq_d = seq_v;
  const godot::Variant members_v = seq_d.get("members", godot::Variant());
  if (members_v.get_type() != godot::Variant::ARRAY) {
    out_error = godot::ERR_INVALID_PARAMETER;
    return false;
  }
  const godot::Array members = members_v;
  if (members.is_empty()) {
    out_error = godot::ERR_INVALID_PARAMETER;
    return false;
  }
  out_sequence.members.clear();
  out_sequence.members.reserve(static_cast<size_t>(members.size()));
  for (int i = 0; i < members.size(); ++i) {
    const godot::Variant mv = members[i];
    if (mv.get_type() != godot::Variant::DICTIONARY) {
      out_error = godot::ERR_INVALID_PARAMETER;
      return false;
    }
    const godot::Dictionary md = mv;
    if (!md.has("image_member_index") || !md.has("role") || !md.has("intended_exposure_compensation_milli_ev")) {
      out_error = godot::ERR_INVALID_PARAMETER;
      return false;
    }
    const godot::Variant idx_v = md.get("image_member_index", godot::Variant());
    const godot::Variant ev_v = md.get("intended_exposure_compensation_milli_ev", godot::Variant());
    if (idx_v.get_type() != godot::Variant::INT || ev_v.get_type() != godot::Variant::INT) {
      out_error = godot::ERR_INVALID_PARAMETER;
      return false;
    }
    CaptureStillImageMemberRole role{};
    if (!parse_role(md.get("role", godot::Variant()), role)) {
      out_error = godot::ERR_INVALID_PARAMETER;
      return false;
    }
    CaptureStillImageMember member{};
    member.image_member_index = static_cast<uint32_t>(int64_t(idx_v));
    member.role = role;
    member.intended_exposure_compensation_milli_ev = static_cast<int32_t>(int64_t(ev_v));
    out_sequence.members.push_back(member);
  }
  out_error = godot::OK;
  return true;
}

} // namespace

namespace cambang {

uint64_t CamBANGDevice::get_instance_id() const {
  if (!hardware_id_.is_empty() && server_) {
    return server_->resolve_endpoint_instance_id(hardware_id_);
  }
  return device_instance_id_;
}

godot::Error CamBANGDevice::engage() {
  if (!server_) {
    return godot::ERR_UNAVAILABLE;
  }
  if (hardware_id_.is_empty()) {
    return godot::ERR_UNAVAILABLE;
  }
  return server_->engage_endpoint_handle(hardware_id_, display_name_);
}

uint64_t CamBANGDevice::trigger_capture() {
  const uint64_t device_instance_id = get_instance_id();
  if (!server_ || device_instance_id == 0) {
    return 0;
  }
  return server_->trigger_device_capture(device_instance_id);
}

godot::Error CamBANGDevice::set_still_capture_profile(const godot::Dictionary& profile) {
  const uint64_t device_instance_id = get_instance_id();
  if (!server_ || device_instance_id == 0) {
    return godot::ERR_BUSY;
  }

  const godot::Dictionary current = server_->get_device_still_capture_profile(device_instance_id);
  if (current.is_empty()) {
    return godot::ERR_BUSY;
  }
  CaptureProfile next_profile{};
  next_profile.width = static_cast<uint32_t>(int64_t(current.get("width", 0)));
  next_profile.height = static_cast<uint32_t>(int64_t(current.get("height", 0)));
  next_profile.format_fourcc = static_cast<uint32_t>(int64_t(current.get("format_fourcc", 0)));

  if (profile.has("width")) {
    const godot::Variant v = profile.get("width", godot::Variant());
    if (v.get_type() != godot::Variant::INT || int64_t(v) <= 0) return godot::ERR_INVALID_PARAMETER;
    next_profile.width = static_cast<uint32_t>(int64_t(v));
  }
  if (profile.has("height")) {
    const godot::Variant v = profile.get("height", godot::Variant());
    if (v.get_type() != godot::Variant::INT || int64_t(v) <= 0) return godot::ERR_INVALID_PARAMETER;
    next_profile.height = static_cast<uint32_t>(int64_t(v));
  }
  if (profile.has("format_fourcc")) {
    const godot::Variant v = profile.get("format_fourcc", godot::Variant());
    if (v.get_type() != godot::Variant::INT || int64_t(v) == 0) return godot::ERR_INVALID_PARAMETER;
    next_profile.format_fourcc = static_cast<uint32_t>(int64_t(v));
  }

  CaptureStillImageBundle sequence{};
  godot::Error parse_err = godot::OK;
  if (!parse_still_image_bundle_dict(profile, sequence, parse_err)) {
    return parse_err;
  }
  return server_->set_device_still_capture_profile(device_instance_id, next_profile, sequence);
}

godot::Dictionary CamBANGDevice::get_still_capture_profile() const {
  const uint64_t device_instance_id = get_instance_id();
  if (!server_ || device_instance_id == 0) {
    return godot::Dictionary();
  }
  return server_->get_device_still_capture_profile(device_instance_id);
}

void CamBANGDevice::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_instance_id"), &CamBANGDevice::get_instance_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_hardware_id"), &CamBANGDevice::get_hardware_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_display_name"), &CamBANGDevice::get_display_name);
  godot::ClassDB::bind_method(godot::D_METHOD("is_endpoint_handle"), &CamBANGDevice::is_endpoint_handle);
  godot::ClassDB::bind_method(godot::D_METHOD("engage"), &CamBANGDevice::engage);
  godot::ClassDB::bind_method(godot::D_METHOD("trigger_capture"), &CamBANGDevice::trigger_capture);
  godot::ClassDB::bind_method(godot::D_METHOD("set_still_capture_profile", "profile"), &CamBANGDevice::set_still_capture_profile);
  godot::ClassDB::bind_method(godot::D_METHOD("get_still_capture_profile"), &CamBANGDevice::get_still_capture_profile);
}

} // namespace cambang
