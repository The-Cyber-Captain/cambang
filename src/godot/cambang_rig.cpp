#include "godot/cambang_rig.h"

#include "godot/cambang_capture_result_set.h"
#include "godot/cambang_server.h"

namespace cambang {

godot::Error CamBANGRig::trigger_capture() {
  if (!server_ || rig_id_ == 0) {
    return godot::ERR_UNAVAILABLE;
  }
  if (!server_->is_running()) {
    return godot::ERR_UNAVAILABLE;
  }
  const uint64_t capture_id = server_->trigger_rig_capture_internal_(rig_id_);
  if (capture_id == 0) {
    return godot::ERR_BUSY;
  }
  current_capture_id_ = capture_id;
  return godot::OK;
}


godot::Ref<CamBANGCaptureResultSet> CamBANGRig::get_result() const {
  if (!server_ || rig_id_ == 0 || current_capture_id_ == 0 || !server_->is_running()) {
    return godot::Ref<CamBANGCaptureResultSet>();
  }
  godot::Ref<CamBANGCaptureResultSet> result_set = server_->get_capture_result_set_by_id(current_capture_id_);
  if (result_set.is_null() || result_set->is_empty()) {
    return godot::Ref<CamBANGCaptureResultSet>();
  }
  return result_set;
}

void CamBANGRig::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_id"), &CamBANGRig::get_id);
  godot::ClassDB::bind_method(godot::D_METHOD("trigger_capture"), &CamBANGRig::trigger_capture);
  godot::ClassDB::bind_method(godot::D_METHOD("get_result"), &CamBANGRig::get_result);
}

} // namespace cambang
