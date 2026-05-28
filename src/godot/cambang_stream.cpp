#include "godot/cambang_stream.h"

namespace cambang {

void CamBANGStream::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_stream_id"), &CamBANGStream::get_stream_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_device_instance_id"), &CamBANGStream::get_device_instance_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_hardware_id"), &CamBANGStream::get_hardware_id);
  godot::ClassDB::bind_method(godot::D_METHOD("is_valid_stream_handle"), &CamBANGStream::is_valid_stream_handle);
}

} // namespace cambang
