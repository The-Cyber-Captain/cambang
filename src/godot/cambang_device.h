#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace cambang {

class CamBANGServer;

class CamBANGDevice final : public godot::RefCounted {
  GDCLASS(CamBANGDevice, godot::RefCounted)

public:
  CamBANGDevice() = default;

  void set_server_and_instance(CamBANGServer* server, uint64_t device_instance_id) {
    server_ = server;
    device_instance_id_ = device_instance_id;
  }

  uint64_t get_device_instance_id() const { return device_instance_id_; }

  uint64_t trigger_capture();

protected:
  static void _bind_methods();

private:
  CamBANGServer* server_ = nullptr;
  uint64_t device_instance_id_ = 0;
};

} // namespace cambang
