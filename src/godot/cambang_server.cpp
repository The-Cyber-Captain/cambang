#include "godot/cambang_server.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include "godot/cambang_state_snapshot.h"
#include "godot/cambang_server_tick_node.h"

#include "imaging/broker/provider_broker.h"
#include "imaging/broker/banner_info.h"

#include <cstdlib>

namespace cambang {

static bool banners_enabled() noexcept {
  const char* v = std::getenv("CAMBANG_BANNERS");
  return !(v && v[0] == '0' && v[1] == '\0');
}

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

  // Ensure a provider is attached + initialized (latched selection).
  // This is the canonical linkage point between Godot and the core runtime.
  (void)_ensure_provider_attached_and_initialized();
}

void CamBANGServer::stop() {
  // Deterministic provider shutdown before stopping the core runtime.
  if (provider_) {
    (void)provider_->shutdown();
    runtime_.attach_provider(nullptr);
    provider_.reset();
  }
  runtime_.stop();
}

#if defined(CAMBANG_ENABLE_DEV_NODES)
ProviderBroker* CamBANGServer::provider_broker_for_dev() const noexcept {
  return dynamic_cast<ProviderBroker*>(provider_.get());
}
#endif

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

void CamBANGServer::_on_godot_tick(double delta) {
  // If a virtual_time provider is active (stub heartbeat, synthetic virtual_time),
  // advance it using the Godot frame delta.
  if (runtime_.is_running()) {
    if (ICameraProvider* prov = runtime_.attached_provider()) {
      if (auto* broker = dynamic_cast<ProviderBroker*>(prov)) {
        const uint64_t dt_ns = static_cast<uint64_t>(delta * 1'000'000'000.0);
        (void)broker->try_tick_virtual_time(dt_ns);
      }
    }

    // Echo any core-thread banner line through Godot logging so it's visible in
    // the editor output panel (stdout isn't reliably surfaced on Windows).
    if (banners_enabled()) {
      char line[192];
      if (runtime_.take_core_banner_line(line, sizeof(line))) {
        godot::UtilityFunctions::print(line);
      }
    }
  }

  // Poll the core-published snapshot buffer.
  auto snap = snapshot_buffer_.snapshot_copy();
  if (!snap) {
    return;
  }

  // gen is zero-indexed. We must still emit the first published snapshot (gen=0).
  if (has_emitted_snapshot_ && snap->gen == last_emitted_gen_) {
    return;
  }

  latest_ = snap;
  last_emitted_gen_ = snap->gen;
  has_emitted_snapshot_ = true;
  emit_signal("state_published", (uint64_t)snap->gen, (uint64_t)snap->topology_gen);
}

bool CamBANGServer::_ensure_provider_attached_and_initialized() {
  if (!runtime_.is_running()) {
    return false;
  }

  // If we already have an attached provider, nothing to do.
  if (provider_ && runtime_.attached_provider() == provider_.get()) {
    return true;
  }

  // Fresh broker per start cycle.
  provider_ = std::make_unique<ProviderBroker>();
  runtime_.attach_provider(provider_.get());

  ProviderResult pr = provider_->initialize(runtime_.provider_callbacks());
  if (!pr.ok()) {
    runtime_.attach_provider(nullptr);
    provider_.reset();
    ERR_PRINT("CamBANGServer: provider initialize failed.");
    return false;
  }

  // Banner 1: Godot-facing provider selection (latched, effective).
  if (banners_enabled()) {
    const ProviderBannerInfo bi = describe_provider_for_banner(provider_.get());
    godot::UtilityFunctions::print("[CamBANG] provider selected (latched): ", bi.provider_mode, " / ", bi.provider_name);
  }

  return true;
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
