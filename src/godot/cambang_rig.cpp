#include "godot/cambang_rig.h"

#include "godot/cambang_capture_result.h"
#include "godot/cambang_device.h"
#include "godot/cambang_server.h"

namespace cambang {

namespace {

// A CamBANGDevice carries a hardware id only when it was built as an endpoint
// handle. One from get_device(instance_id) has it explicitly cleared (see
// CamBANGDevice::set_server_and_instance), so the instance must be resolved
// through the server. Both handle kinds are legitimate to hand to a rig, and
// the caller should not have to know which they are holding.
godot::String resolve_member_hardware_id(CamBANGServer* server,
                                         const godot::Ref<CamBANGDevice>& device) {
  if (!server || device.is_null()) {
    return godot::String();
  }
  const godot::String direct = device->get_hardware_id();
  if (!direct.is_empty()) {
    return direct;
  }
  return server->resolve_hardware_id_for_instance(device->get_instance_id());
}

} // namespace

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

godot::Error CamBANGRig::trigger_capture() {
  if (!server_ || rig_id_ == 0) {
    return godot::ERR_UNAVAILABLE;
  }
  if (!server_->is_running()) {
    return godot::ERR_UNAVAILABLE;
  }
  const CamBANGServer::RigTriggerInternalResult result =
      server_->trigger_rig_capture_internal_(rig_id_);
  if (result.rig_capture_id == 0) {
    return result.error;
  }
  current_rig_capture_id_ = result.rig_capture_id;
  return godot::OK;
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

void CamBANGRig::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_id"), &CamBANGRig::get_id);
  godot::ClassDB::bind_method(godot::D_METHOD("add_member", "device"), &CamBANGRig::add_member);
  godot::ClassDB::bind_method(godot::D_METHOD("remove_member", "device"), &CamBANGRig::remove_member);
  godot::ClassDB::bind_method(godot::D_METHOD("trigger_capture"), &CamBANGRig::trigger_capture);
  godot::ClassDB::bind_method(godot::D_METHOD("get_result"), &CamBANGRig::get_result);
}

} // namespace cambang
