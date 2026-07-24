#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace cambang {

class CamBANGServer;
class CamBANGStream;
class CamBANGCaptureResult;

class CamBANGDevice final : public godot::RefCounted {
  GDCLASS(CamBANGDevice, godot::RefCounted)

public:
  CamBANGDevice() = default;

  void set_server_and_instance(CamBANGServer* server, uint64_t device_instance_id) {
    server_ = server;
    device_instance_id_ = device_instance_id;
    hardware_id_ = godot::String();
    display_name_ = godot::String();
    current_capture_id_ = 0;
    live_ = false;
    _register_with_server_();
  }
  void set_server_and_endpoint(CamBANGServer* server,
                               const godot::String& hardware_id,
                               const godot::String& display_name) {
    server_ = server;
    device_instance_id_ = 0;
    hardware_id_ = hardware_id;
    display_name_ = display_name;
    current_capture_id_ = 0;
    live_ = false;
    _register_with_server_();
  }

  uint64_t get_instance_id() const;
  godot::String get_hardware_id() const { return hardware_id_; }
  godot::String get_display_name() const { return display_name_; }
  bool is_live() const { return live_; }
  bool is_endpoint_handle() const { return device_instance_id_ == 0 && !hardware_id_.is_empty(); }
  godot::Error engage();
  godot::Error disengage();
  godot::Ref<CamBANGStream> create_stream(const godot::Variant& definition = godot::Variant());

  godot::Error trigger_capture();
  godot::Ref<CamBANGCaptureResult> get_result() const;
  godot::Error set_warm_policy(const godot::Dictionary& policy);
  godot::Error set_still_capture_profile(const godot::Dictionary& profile);
  godot::Error set_capture_picture(const godot::Dictionary& picture);
  godot::Dictionary get_still_capture_profile() const;

protected:
  static void _bind_methods();

private:
  friend class CamBANGServer;
  void _register_with_server_();
  void _set_live_from_server_(bool live);

  CamBANGServer* server_ = nullptr;
  uint64_t device_instance_id_ = 0;
  godot::String hardware_id_;
  godot::String display_name_;
  uint64_t current_capture_id_ = 0;
  bool live_ = false;
};

} // namespace cambang
