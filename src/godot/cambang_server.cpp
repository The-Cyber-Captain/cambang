#include "godot/cambang_server.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include "godot/cambang_state_snapshot.h"
#include "godot/cambang_server_tick_node.h"

namespace cambang {

CamBANGServer* CamBANGServer::singleton_ = nullptr;

CamBANGServer::CamBANGServer() {
  if (singleton_ && singleton_ != this) {
    // This should never happen: the engine singleton is registered once.
    ERR_PRINT("CamBANGServer: multiple instances detected; only one is supported.");
    return;
  }
  singleton_ = this;
  runtime_.set_snapshot_publisher(&snapshot_buffer_);
}

CamBANGServer::~CamBANGServer() {
  // Ensure graceful stop if the extension is torn down.
  runtime_.stop();
  if (singleton_ == this) {
    singleton_ = nullptr;
  }
}

void CamBANGServer::start() {
  // Ensure the tick node exists so snapshots can be drained + signals emitted.
  _ensure_tick_installed();

  // Explicit user action: do not auto-start on launch.
  runtime_.start();
}

void CamBANGServer::stop() {
  runtime_.stop();
}

  void CamBANGServer::_ensure_tick_installed() {
  // If already present in scene tree, we're done.
  godot::MainLoop* ml = godot::Engine::get_singleton()->get_main_loop();
  godot::SceneTree* tree = godot::Object::cast_to<godot::SceneTree>(ml);
  if (!tree) {
    return;
  }
  godot::Window* root = tree->get_root();
  if (!root) {
    return;
  }

  if (root->get_node_or_null(godot::NodePath("__CamBANGServerTick")) != nullptr) {
    tick_installed_ = true;
    return;
  }

  // If we've already scheduled installation, don't schedule again.
  if (tick_installed_) {
    return;
  }

  auto* tick = memnew(CamBANGServerTickNode);
  tick->set_name("__CamBANGServerTick");
  tick->set_process(true);
  tick->set_process_mode(godot::Node::PROCESS_MODE_ALWAYS);

  // Defer adding to avoid "Parent node is busy setting up children" during _ready().
  root->call_deferred("add_child", tick);

  // Mark as scheduled to avoid duplicates. Presence check above will confirm later.
  tick_installed_ = true;
}

void CamBANGServer::_on_godot_tick() {
  // Poll the core-published snapshot buffer.
  auto snap = snapshot_buffer_.snapshot_copy();
  if (!snap) {
    return;
  }
  if (snap->gen == last_emitted_gen_) {
    return;
  }

  latest_ = snap;
  last_emitted_gen_ = snap->gen;
  emit_signal("state_published", (uint64_t)snap->gen, (uint64_t)snap->topology_gen);
}
#if 0
godot::Ref<cambang::CamBANGStateSnapshotGD> CamBANGServer::get_state_snapshot() const {
  godot::Ref<CamBANGStateSnapshotGD> out;
  out.instantiate();
  out->_init_from_core(latest_);
  return out;
}
#endif

void CamBANGServer::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("start"), &CamBANGServer::start);
  godot::ClassDB::bind_method(godot::D_METHOD("stop"), &CamBANGServer::stop);
  godot::ClassDB::bind_method(godot::D_METHOD("get_state_snapshot"), &CamBANGServer::get_state_snapshot);

  ADD_SIGNAL(godot::MethodInfo(
      "state_published",
      godot::PropertyInfo(godot::Variant::INT, "gen"),
      godot::PropertyInfo(godot::Variant::INT, "topology_gen")));
}

} // namespace cambang
