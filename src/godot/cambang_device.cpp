#include "godot/cambang_device.h"

#include "godot/cambang_server.h"

namespace cambang {

uint64_t CamBANGDevice::trigger_capture() {
  if (!server_ || device_instance_id_ == 0) {
    return 0;
  }
  return server_->trigger_device_capture(device_instance_id_);
}

void CamBANGDevice::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_device_instance_id"), &CamBANGDevice::get_device_instance_id);
  godot::ClassDB::bind_method(godot::D_METHOD("trigger_capture"), &CamBANGDevice::trigger_capture);
}

} // namespace cambang
