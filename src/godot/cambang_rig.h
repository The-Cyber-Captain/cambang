#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace cambang {

class CamBANGServer;

class CamBANGRig final : public godot::RefCounted {
  GDCLASS(CamBANGRig, godot::RefCounted)

public:
  CamBANGRig() = default;

  void set_server_and_id(CamBANGServer* server, uint64_t rig_id) {
    server_ = server;
    rig_id_ = rig_id;
  }

  uint64_t get_id() const { return rig_id_; }
  uint64_t trigger_capture();

protected:
  static void _bind_methods();

private:
  CamBANGServer* server_ = nullptr;
  uint64_t rig_id_ = 0;
};

} // namespace cambang
