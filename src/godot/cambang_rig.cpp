#include "godot/cambang_rig.h"

#include "godot/cambang_server.h"

namespace cambang {

uint64_t CamBANGRig::trigger_capture() {
  if (!server_ || rig_id_ == 0) {
    return 0;
  }
  return server_->trigger_rig_capture_internal_(rig_id_);
}

void CamBANGRig::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_id"), &CamBANGRig::get_id);
  godot::ClassDB::bind_method(godot::D_METHOD("trigger_capture"), &CamBANGRig::trigger_capture);
}

} // namespace cambang
