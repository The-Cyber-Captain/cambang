#include "godot/cambang_rig.h"

#include "godot/cambang_capture_result.h"
#include "godot/cambang_device.h"
#include "godot/cambang_server.h"

namespace cambang {

namespace {

// Both handle kinds are legitimate to hand to a rig, and the caller should not
// have to know which they are holding. The resolution itself lives on the
// server, so rig creation and rig membership cannot drift apart on what counts
// as a valid device handle.
godot::String resolve_member_hardware_id(CamBANGServer* server,
                                         const godot::Ref<CamBANGDevice>& device) {
  if (!server) {
    return godot::String();
  }
  return server->resolve_device_hardware_id(device);
}

} // namespace

void CamBANGRig::_register_with_server_() {
  if (!server_) {
    return;
  }
  server_->register_tracked_rig_wrapper_(
      static_cast<uint64_t>(godot::Object::get_instance_id()));
}

godot::Error CamBANGRig::add_member(const godot::Ref<CamBANGDevice>& device) {
  if (!server_ || rig_id_ == 0) {
    return godot::ERR_UNAVAILABLE;
  }
  const godot::String hardware_id = resolve_member_hardware_id(server_, device);
  if (hardware_id.is_empty()) {
    return godot::ERR_INVALID_PARAMETER;
  }
  return server_->add_rig_member_by_hardware_id(rig_id_, hardware_id);
}

godot::Error CamBANGRig::remove_member(const godot::Ref<CamBANGDevice>& device) {
  if (!server_ || rig_id_ == 0) {
    return godot::ERR_UNAVAILABLE;
  }
  const godot::String hardware_id = resolve_member_hardware_id(server_, device);
  if (hardware_id.is_empty()) {
    return godot::ERR_INVALID_PARAMETER;
  }
  return server_->remove_rig_member_by_hardware_id(rig_id_, hardware_id);
}

godot::Dictionary CamBANGRig::trigger_capture() {
  // All three keys always present, whatever the outcome (section 4.1). Branch
  // on `error`, never on a key's absence.
  godot::Dictionary out;
  out["id"] = static_cast<uint64_t>(0);
  out["members"] = godot::Dictionary();

  if (!server_ || rig_id_ == 0 || !server_->is_running()) {
    out["error"] = godot::ERR_UNAVAILABLE;
    return out;
  }
  const CamBANGServer::RigTriggerInternalResult result =
      server_->trigger_rig_capture_internal_(rig_id_);
  if (result.rig_capture_id == 0) {
    // Refused: no rig capture id, and no member map. Nothing was minted, so
    // there is nothing for the caller to correlate against or wait for.
    out["error"] = result.error;
    return out;
  }
  current_rig_capture_id_ = result.rig_capture_id;

  godot::Dictionary members;
  for (const auto& member : result.members) {
    members[godot::String(member.hardware_id.c_str())] =
        static_cast<uint64_t>(member.device_capture_id);
  }
  out["id"] = static_cast<uint64_t>(result.rig_capture_id);
  out["members"] = members;
  out["error"] = godot::OK;
  return out;
}


godot::TypedArray<CamBANGCaptureResult> CamBANGRig::get_result() const {
  if (!server_ || rig_id_ == 0 || current_rig_capture_id_ == 0 || !server_->is_running()) {
    return godot::TypedArray<CamBANGCaptureResult>();
  }
  godot::TypedArray<CamBANGCaptureResult> results =
      server_->get_capture_result_set_by_id(current_rig_capture_id_);
  if (results.is_empty()) {
    return godot::TypedArray<CamBANGCaptureResult>();
  }
  return results;
}

godot::Array CamBANGRig::get_member_outcomes() const {
  if (!server_ || rig_id_ == 0 || current_rig_capture_id_ == 0 || !server_->is_running()) {
    return godot::Array();
  }
  return server_->get_capture_member_outcomes_by_id(current_rig_capture_id_);
}

void CamBANGRig::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_id"), &CamBANGRig::get_id);
  godot::ClassDB::bind_method(godot::D_METHOD("add_member", "device"), &CamBANGRig::add_member);
  godot::ClassDB::bind_method(godot::D_METHOD("remove_member", "device"), &CamBANGRig::remove_member);
  godot::ClassDB::bind_method(godot::D_METHOD("trigger_capture"), &CamBANGRig::trigger_capture);
  godot::ClassDB::bind_method(godot::D_METHOD("get_result"), &CamBANGRig::get_result);
  godot::ClassDB::bind_method(godot::D_METHOD("get_member_outcomes"), &CamBANGRig::get_member_outcomes);
  // Emitted when a rig capture THIS rig triggered closes (section 4.2).
  // rig_capture_id matches the id trigger_capture() returned; closed_reason is
  // ALL_MEMBERS_TERMINAL or WINDOW_EXPIRED.
  ADD_SIGNAL(godot::MethodInfo(
      "capture_finished",
      godot::PropertyInfo(godot::Variant::INT, "rig_capture_id"),
      godot::PropertyInfo(godot::Variant::INT, "closed_reason")));
}

} // namespace cambang
