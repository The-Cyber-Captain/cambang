#pragma once

#include <cstdint>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

namespace cambang {

class CamBANGServer;
class CamBANGCaptureResultSet;

class CamBANGRig final : public godot::RefCounted {
  GDCLASS(CamBANGRig, godot::RefCounted)

public:
  CamBANGRig() = default;

  void set_server_and_id(CamBANGServer* server, uint64_t rig_id) {
    server_ = server;
    rig_id_ = rig_id;
    current_capture_id_ = 0;
  }

  uint64_t get_id() const { return rig_id_; }
  godot::Error trigger_capture();
  godot::Ref<CamBANGCaptureResultSet> get_result() const;

protected:
  static void _bind_methods();

private:
  CamBANGServer* server_ = nullptr;
  uint64_t rig_id_ = 0;
  uint64_t current_capture_id_ = 0;
};

} // namespace cambang
