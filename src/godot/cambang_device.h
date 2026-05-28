#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

namespace cambang {

class CamBANGServer;
class CamBANGStream;

class CamBANGDevice final : public godot::RefCounted {
  GDCLASS(CamBANGDevice, godot::RefCounted)

public:
  CamBANGDevice() = default;

  void set_server_and_instance(CamBANGServer* server, uint64_t device_instance_id) {
    server_ = server;
    device_instance_id_ = device_instance_id;
    hardware_id_ = godot::String();
    display_name_ = godot::String();
  }
  void set_server_and_endpoint(CamBANGServer* server,
                               const godot::String& hardware_id,
                               const godot::String& display_name) {
    server_ = server;
    device_instance_id_ = 0;
    hardware_id_ = hardware_id;
    display_name_ = display_name;
  }

  uint64_t get_instance_id() const;
  godot::String get_hardware_id() const { return hardware_id_; }
  godot::String get_display_name() const { return display_name_; }
  bool is_endpoint_handle() const { return device_instance_id_ == 0 && !hardware_id_.is_empty(); }
  godot::Error engage();
  godot::Error disengage();
  godot::Ref<CamBANGStream> create_stream();

  uint64_t trigger_capture();
  godot::Error set_still_capture_profile(const godot::Dictionary& profile);
  godot::Dictionary get_still_capture_profile() const;

protected:
  static void _bind_methods();

private:
  CamBANGServer* server_ = nullptr;
  uint64_t device_instance_id_ = 0;
  godot::String hardware_id_;
  godot::String display_name_;
};

} // namespace cambang
