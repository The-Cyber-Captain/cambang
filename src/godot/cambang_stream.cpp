#include "godot/cambang_stream.h"
#include "godot/cambang_server.h"

namespace cambang {

godot::Error CamBANGStream::destroy() {
  if (destroy_requested_) {
    return godot::OK;
  }
  if (!server_ || stream_id_ == 0) {
    return godot::ERR_UNAVAILABLE;
  }
  const godot::Error rc = server_->destroy_direct_stream_handle(stream_id_, hardware_id_, device_instance_id_);
  if (rc == godot::OK) {
    destroy_requested_ = true;
  }
  return rc;
}

void CamBANGStream::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_stream_id"), &CamBANGStream::get_stream_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_device_instance_id"), &CamBANGStream::get_device_instance_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_hardware_id"), &CamBANGStream::get_hardware_id);
  godot::ClassDB::bind_method(godot::D_METHOD("is_valid_stream_handle"), &CamBANGStream::is_valid_stream_handle);
  godot::ClassDB::bind_method(godot::D_METHOD("destroy"), &CamBANGStream::destroy);
}

} // namespace cambang
