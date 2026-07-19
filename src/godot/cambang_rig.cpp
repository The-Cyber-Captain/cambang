#include "godot/cambang_rig.h"

#include "godot/cambang_capture_result.h"
#include "godot/cambang_server.h"

namespace cambang {

godot::Error CamBANGRig::trigger_capture() {
  if (!server_ || rig_id_ == 0) {
    return godot::ERR_UNAVAILABLE;
  }
  if (!server_->is_running()) {
    return godot::ERR_UNAVAILABLE;
  }
  const CamBANGServer::RigTriggerInternalResult result =
      server_->trigger_rig_capture_internal_(rig_id_);
  if (result.capture_id == 0) {
    return result.error;
  }
  current_capture_id_ = result.capture_id;
  return godot::OK;
}


godot::TypedArray<CamBANGCaptureResult> CamBANGRig::get_result() const {
  if (!server_ || rig_id_ == 0 || current_capture_id_ == 0 || !server_->is_running()) {
    return godot::TypedArray<CamBANGCaptureResult>();
  }
  godot::TypedArray<CamBANGCaptureResult> results =
      server_->get_capture_result_set_by_id(current_capture_id_);
  if (results.is_empty()) {
    return godot::TypedArray<CamBANGCaptureResult>();
  }
  return results;
}

void CamBANGRig::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_id"), &CamBANGRig::get_id);
  godot::ClassDB::bind_method(godot::D_METHOD("trigger_capture"), &CamBANGRig::trigger_capture);
  godot::ClassDB::bind_method(godot::D_METHOD("get_result"), &CamBANGRig::get_result);
}

} // namespace cambang
