#pragma once

#include <memory>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

namespace cambang {

struct RenderThreadTaskState;

class RenderThreadDrainHelper : public godot::RefCounted {
  GDCLASS(RenderThreadDrainHelper, godot::RefCounted);

public:
  static void _bind_methods() {}
  void drain_pending_releases_on_render_thread();
};

class RenderThreadTaskHelper : public godot::RefCounted {
  GDCLASS(RenderThreadTaskHelper, godot::RefCounted);

public:
  static void _bind_methods() {}
  void init(const std::shared_ptr<RenderThreadTaskState>& state);
  void run_on_render_thread();

private:
  std::shared_ptr<RenderThreadTaskState> state_;
};

// Internal-only: registers bridge helper classes required for Godot ClassDB/
// RefCounted instantiation (e.g., RenderThreadDrainHelper and RenderThreadTaskHelper). Not public API.
void register_synthetic_gpu_backing_internal_classes();

} // namespace cambang
