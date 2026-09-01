#include "godot/cambang_device.h"

#include "godot/cambang_capture_result.h"
#include "godot/cambang_server.h"
#include "godot/cambang_stream.h"

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

void CamBANGDevice::_register_with_server_() {
  if (!server_) {
    return;
  }
  server_->register_tracked_device_wrapper_(
      static_cast<uint64_t>(godot::Object::get_instance_id()));
}

void CamBANGDevice::_set_live_from_server_(bool live) {
  if (live_ == live) {
    return;
  }
  live_ = live;
  emit_signal("live_changed", live_);
}

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

godot::Error CamBANGDevice::disengage() {
  if (!server_) {
    return godot::ERR_UNAVAILABLE;
  }
  if (hardware_id_.is_empty()) {
    return godot::ERR_UNAVAILABLE;
  }
  const godot::Error err = server_->disengage_endpoint_handle(hardware_id_);
  if (err == godot::OK) {
    current_capture_id_ = 0;
  }
  return err;
}

godot::Ref<CamBANGStream> CamBANGDevice::create_stream(const godot::Variant& definition) {
  if (!server_ || hardware_id_.is_empty()) {
    return godot::Ref<CamBANGStream>();
  }
  const uint64_t device_instance_id = get_instance_id();
  if (device_instance_id == 0) {
    return godot::Ref<CamBANGStream>();
  }
  return server_->create_stream_for_endpoint_hardware_id(hardware_id_, definition);
}

godot::Dictionary CamBANGDevice::trigger_capture() {
  // Keys are ALWAYS present, whatever the outcome (section 4.1). A caller
  // branches on `error`, never on whether a key exists -- an absent key is the
  // shape of a bug, not a signal.
  //
  // A refused trigger mints no id: `id` is 0. If a rejection returned an id,
  // the caller's outstanding-work set would accumulate captures that never
  // existed, which is precisely the bookkeeping this return shape exists to
  // make possible.
  godot::Dictionary out;
  out["id"] = godot::String();

  const uint64_t device_instance_id = get_instance_id();
  if (!server_ || device_instance_id == 0 || !server_->is_running()) {
    out["error"] = godot::ERR_UNAVAILABLE;
    return out;
  }
  uint64_t capture_id = 0;
  const godot::Error trigger_error =
      server_->trigger_device_capture(device_instance_id, capture_id);
  if (trigger_error != godot::OK) {
    out["error"] = trigger_error;
    return out;
  }
  current_capture_id_ = capture_id;
  // The PUBLIC id (2.2). current_capture_id_ keeps the internal uint64, which
  // is what Core is keyed by; the caller never sees that form.
  out["id"] = server_->device_capture_public_id(capture_id);
  out["error"] = godot::OK;
  return out;
}


godot::Ref<CamBANGCaptureResult> CamBANGDevice::get_result() const {
  const uint64_t device_instance_id = get_instance_id();
  if (!server_ || device_instance_id == 0 || !server_->is_running()) {
    return godot::Ref<CamBANGCaptureResult>();
  }
  // A capture triggered through THIS handle wins, and only then the device's
  // latest from any path. That order is deliberate and was briefly inverted:
  // "latest on the device" makes a rig member's result satisfy a caller that
  // is waiting on the device capture it triggered itself, which silently
  // mis-attributes one to the other. The soak benchmark polls exactly that way.
  //
  // The fallback is what keeps section 8 honest: a device that has only ever
  // been a rig member has no current_capture_id_ here, and the server's map --
  // now written by the rig path too -- supplies its result rather than null.
  //
  // A caller wanting one specific capture should hold the id trigger_capture()
  // returned and use CamBANGServer::get_capture_result_by_id().
  uint64_t capture_id = current_capture_id_;
  if (capture_id == 0) {
    capture_id = server_->get_latest_capture_id_for_device(device_instance_id);
  }
  if (capture_id == 0) {
    return godot::Ref<CamBANGCaptureResult>();
  }
  return server_->get_capture_result_by_id(server_->device_capture_public_id(capture_id));
}

godot::Error CamBANGDevice::set_warm_policy(const godot::Dictionary& policy) {
  if (!server_) {
    return godot::ERR_UNAVAILABLE;
  }
  if (!policy.has("warm_hold_ms")) {
    return godot::ERR_INVALID_PARAMETER;
  }
  const godot::Variant v = policy.get("warm_hold_ms", godot::Variant());
  if (v.get_type() != godot::Variant::INT) {
    return godot::ERR_INVALID_PARAMETER;
  }
  const int64_t warm_hold_ms_i = int64_t(v);
  if (warm_hold_ms_i < 0 || warm_hold_ms_i > static_cast<int64_t>(UINT32_MAX)) {
    return godot::ERR_INVALID_PARAMETER;
  }

  const uint32_t warm_hold_ms = static_cast<uint32_t>(warm_hold_ms_i);
  const uint64_t device_instance_id = get_instance_id();
  if (device_instance_id == 0) {
    if (!hardware_id_.is_empty()) {
      return server_->set_endpoint_warm_hold_ms_startup_intent(hardware_id_, warm_hold_ms);
    }
    return godot::ERR_UNAVAILABLE;
  }
  return server_->set_device_warm_hold_ms(device_instance_id, warm_hold_ms);
}

godot::Error CamBANGDevice::set_still_capture_profile(const godot::Dictionary& profile) {
  if (!server_) {
    return godot::ERR_BUSY;
  }

  const uint64_t device_instance_id = get_instance_id();
  CaptureProfile next_profile{};
  if (device_instance_id == 0) {
    if (hardware_id_.is_empty()) {
      return godot::ERR_BUSY;
    }
    if (!server_->get_endpoint_capture_template_profile(hardware_id_, next_profile)) {
      return godot::ERR_BUSY;
    }
  } else {
    const godot::Dictionary current = server_->get_device_still_capture_profile(device_instance_id);
    if (current.is_empty()) {
      return godot::ERR_BUSY;
    }
    next_profile.width = static_cast<uint32_t>(int64_t(current.get("width", 0)));
    next_profile.height = static_cast<uint32_t>(int64_t(current.get("height", 0)));
    next_profile.format_fourcc = static_cast<uint32_t>(int64_t(current.get("format_fourcc", 0)));
  }

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
  if (device_instance_id == 0) {
    return server_->set_endpoint_still_capture_profile_startup_intent(hardware_id_, next_profile, sequence);
  }
  return server_->set_device_still_capture_profile(device_instance_id, next_profile, sequence);
}

godot::Error CamBANGDevice::set_capture_picture(const godot::Dictionary& picture) {
  if (!server_) {
    return godot::ERR_BUSY;
  }
  const uint64_t device_instance_id = get_instance_id();
  if (device_instance_id == 0) {
    // Capture picture is device-scoped: the device must be engaged/live.
    return godot::ERR_BUSY;
  }
  return server_->set_device_capture_picture(device_instance_id, picture);
}

godot::Dictionary CamBANGDevice::get_still_capture_profile() const {
  const uint64_t device_instance_id = get_instance_id();
  if (!server_ || device_instance_id == 0) {
    return godot::Dictionary();
  }
  return server_->get_device_still_capture_profile(device_instance_id);
}

// Convenience wrappers over the endpoint-scoped pair on CamBANGServer. The
// answer describes the camera behind this device, not this device's state, so
// it is the same before and after engage().
godot::Dictionary CamBANGDevice::get_supported_stream_profiles() const {
  if (!server_) {
    return godot::Dictionary();
  }
  return server_->get_supported_stream_profiles(get_hardware_id());
}

godot::Dictionary CamBANGDevice::get_supported_capture_profiles() const {
  if (!server_) {
    return godot::Dictionary();
  }
  return server_->get_supported_capture_profiles(get_hardware_id());
}

void CamBANGDevice::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_instance_id"), &CamBANGDevice::get_instance_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_hardware_id"), &CamBANGDevice::get_hardware_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_display_name"), &CamBANGDevice::get_display_name);
  godot::ClassDB::bind_method(godot::D_METHOD("is_live"), &CamBANGDevice::is_live);
  godot::ClassDB::bind_method(godot::D_METHOD("is_endpoint_handle"), &CamBANGDevice::is_endpoint_handle);
  godot::ClassDB::bind_method(godot::D_METHOD("engage"), &CamBANGDevice::engage);
  godot::ClassDB::bind_method(godot::D_METHOD("disengage"), &CamBANGDevice::disengage);
  godot::ClassDB::bind_method(
      godot::D_METHOD("create_stream", "definition"),
      &CamBANGDevice::create_stream,
      DEFVAL(godot::Variant()));
  godot::ClassDB::bind_method(godot::D_METHOD("trigger_capture"), &CamBANGDevice::trigger_capture);
  godot::ClassDB::bind_method(godot::D_METHOD("get_result"), &CamBANGDevice::get_result);
  godot::ClassDB::bind_method(godot::D_METHOD("set_warm_policy", "policy"), &CamBANGDevice::set_warm_policy);
  godot::ClassDB::bind_method(godot::D_METHOD("set_still_capture_profile", "profile"), &CamBANGDevice::set_still_capture_profile);
  godot::ClassDB::bind_method(godot::D_METHOD("set_capture_picture", "picture"), &CamBANGDevice::set_capture_picture);
  godot::ClassDB::bind_method(godot::D_METHOD("get_still_capture_profile"), &CamBANGDevice::get_still_capture_profile);
  godot::ClassDB::bind_method(godot::D_METHOD("get_supported_stream_profiles"), &CamBANGDevice::get_supported_stream_profiles);
  godot::ClassDB::bind_method(godot::D_METHOD("get_supported_capture_profiles"), &CamBANGDevice::get_supported_capture_profiles);
  ADD_PROPERTY(godot::PropertyInfo(godot::Variant::BOOL, "live"), "", "is_live");
  ADD_SIGNAL(godot::MethodInfo(
      "live_changed",
      godot::PropertyInfo(godot::Variant::BOOL, "live")));
  // Emitted for every capture on this device, when each finishes
  // (capture_identity_and_lifecycle.md 4.2). capture_id matches the id
  // trigger_capture() returned, so a caller clears its outstanding entry here.
  //
  // Fires for rig-member captures too, which 4.2's wording ("the Device
  // Captures it initiated") does not describe -- a member is an ordinary
  // Device Capture on this device, and section 8 requires device-level
  // surfaces not to be blind to rig-originated ones. Recorded in 9.5.
  ADD_SIGNAL(godot::MethodInfo(
      "capture_finished",
      godot::PropertyInfo(godot::Variant::STRING, "capture_id"),
      godot::PropertyInfo(godot::Variant::INT, "disposition"),
      godot::PropertyInfo(godot::Variant::INT, "error_code")));
}

} // namespace cambang
