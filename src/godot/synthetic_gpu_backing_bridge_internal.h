#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace cambang {

class RenderThreadDrainHelper : public godot::RefCounted {
  GDCLASS(RenderThreadDrainHelper, godot::RefCounted);

public:
  bool drain_pending_releases_on_render_thread();

  static void _bind_methods() {
    godot::ClassDB::bind_method(
        godot::D_METHOD("drain_pending_releases_on_render_thread"),
        &RenderThreadDrainHelper::drain_pending_releases_on_render_thread);
  }
};

// Internal-only: registers bridge helper classes required for Godot ClassDB/
// RefCounted instantiation (e.g., RenderThreadDrainHelper). Not public API.
void register_synthetic_gpu_backing_internal_classes();

} // namespace cambang
