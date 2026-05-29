#include "godot/cambang_stream.h"
#include "godot/cambang_server.h"
#include "godot/cambang_stream_result.h"

namespace cambang {

godot::Error CamBANGStream::start() {
  if (destroy_requested_ || !server_ || stream_id_ == 0 || device_instance_id_ == 0) {
    return godot::ERR_UNAVAILABLE;
  }
  return server_->start_direct_stream_handle(stream_id_, hardware_id_, device_instance_id_);
}

godot::Error CamBANGStream::stop() {
  if (destroy_requested_ || !server_ || stream_id_ == 0 || device_instance_id_ == 0) {
    return godot::ERR_UNAVAILABLE;
  }
  return server_->stop_direct_stream_handle(stream_id_, hardware_id_, device_instance_id_);
}

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


godot::Ref<CamBANGStreamResult> CamBANGStream::get_result() const {
  if (!is_valid_stream_handle() || !server_->is_running()) {
    return godot::Ref<CamBANGStreamResult>();
  }
  return server_->get_stream_result_by_stream_id(stream_id_);
}

void CamBANGStream::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_stream_id"), &CamBANGStream::get_stream_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_device_instance_id"), &CamBANGStream::get_device_instance_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_hardware_id"), &CamBANGStream::get_hardware_id);
  godot::ClassDB::bind_method(godot::D_METHOD("is_valid_stream_handle"), &CamBANGStream::is_valid_stream_handle);
  godot::ClassDB::bind_method(godot::D_METHOD("start"), &CamBANGStream::start);
  godot::ClassDB::bind_method(godot::D_METHOD("stop"), &CamBANGStream::stop);
  godot::ClassDB::bind_method(godot::D_METHOD("destroy"), &CamBANGStream::destroy);
  godot::ClassDB::bind_method(godot::D_METHOD("get_result"), &CamBANGStream::get_result);
}

} // namespace cambang
