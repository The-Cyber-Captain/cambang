#include "godot/cambang_server.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>

#include "core/synthetic_timeline_request_binding.h"
#include "imaging/broker/provider_broker.h"
#include "imaging/broker/banner_info.h"

#include <cstdlib>

namespace cambang {

static bool banners_enabled() noexcept {
  const char* v = std::getenv("CAMBANG_BANNERS");
  return !(v && v[0] == '0' && v[1] == '\0');
}

CamBANGServer* CamBANGServer::singleton_ = nullptr;


namespace {

static const char* mode_to_cstr(RuntimeMode m) noexcept {
  switch (m) {
    case RuntimeMode::platform_backed: return "platform_backed";
    case RuntimeMode::synthetic: return "synthetic";
    default: return "unknown";
  }
}

static bool parse_mode_string(const godot::String& s, RuntimeMode& out_mode) noexcept {
  if (s == "platform_backed") {
    out_mode = RuntimeMode::platform_backed;
    return true;
  }
  if (s == "synthetic") {
    out_mode = RuntimeMode::synthetic;
    return true;
  }
  return false;
}

static godot::Error map_provider_result_to_godot_error(ProviderResult pr) noexcept {
  switch (pr.code) {
    case ProviderError::OK: return godot::OK;
    case ProviderError::ERR_BUSY: return godot::ERR_BUSY;
    case ProviderError::ERR_INVALID_ARGUMENT: return godot::ERR_INVALID_PARAMETER;
    case ProviderError::ERR_BAD_STATE: return godot::ERR_INVALID_PARAMETER;
    case ProviderError::ERR_NOT_SUPPORTED: return godot::ERR_UNAVAILABLE;
    default: return godot::FAILED;
  }
}

} // namespace

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
  const CoreRuntimeState state = runtime_.state_copy();
  if (state == CoreRuntimeState::STARTING || state == CoreRuntimeState::LIVE) {
    // CoreRuntime::start() is already idempotent for active sessions. Mirror that
    // at the Godot boundary so redundant start() calls do not clear the currently
    // latched snapshot/counters before the no-op runtime call returns.
    return;
  }

  // New session starts with no Godot-latched snapshot until first publish.
  latest_.reset();
  latest_export_.clear();
  has_latest_export_ = false;
  has_godot_counters_ = false;

  // Begin a new boundary session and reject any prior-generation late publishes
  // until the first snapshot from the next expected generation is observed.
  active_session_id_ = ++session_counter_;
  if (has_last_completed_gen_) {
    accepted_min_gen_ = last_completed_gen_ + 1;
    enforce_min_gen_gate_ = true;
  } else {
    accepted_min_gen_ = 0;
    enforce_min_gen_gate_ = false;
  }
  last_seen_published_seq_ = runtime_.published_seq();

  // Defensive re-check: requested mode must be supported in this build.
  {
    ProviderResult cap = ProviderBroker::check_mode_supported_in_build(provider_mode_requested_);
    if (!cap.ok()) {
      ERR_PRINT(godot::vformat(
          "CamBANGServer: cannot start; requested provider_mode='%s' is not supported in this build.",
          mode_to_cstr(provider_mode_requested_)));
      return;
    }
  }

  // Ensure the SceneTree tick hook exists so snapshots can be drained + signals emitted.
  _ensure_tick_connected();

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

  // Stop is a boundary operation; core may have published final teardown/retirement
  // truth during deterministic shutdown. Drain one final boundary observation before
  // returning to NIL so the prior generation can be observed once more if changed.
  if (has_godot_counters_) {
    (void)_consume_latest_core_snapshot(/*emit_signal=*/true);
  }

  if (has_godot_counters_) {
    has_last_completed_gen_ = true;
    last_completed_gen_ = godot_gen_;
  }

  // Enforce documented NIL pre-baseline behaviour across restart boundaries.
  latest_.reset();
  latest_export_.clear();
  has_latest_export_ = false;
  has_godot_counters_ = false;
  snapshot_buffer_.clear();
  last_seen_published_seq_ = runtime_.published_seq();
  active_session_id_ = 0;
  enforce_min_gen_gate_ = false;

  // Allow a fresh "busy" log once per live session.
  provider_mode_busy_logged_ = false;
}

#if defined(CAMBANG_ENABLE_DEV_NODES)
ProviderBroker* CamBANGServer::provider_broker_for_dev() const noexcept {
  return dynamic_cast<ProviderBroker*>(provider_.get());
}
#endif

void CamBANGServer::_ensure_tick_connected() {
  if (tick_connected_) {
    return;
  }

  // Connect to SceneTree's per-frame tick. This is the cleanest way for an
  // Engine singleton (not in the scene tree) to receive a main-thread tick.
  godot::MainLoop* ml = godot::Engine::get_singleton()->get_main_loop();
  godot::SceneTree* tree = godot::Object::cast_to<godot::SceneTree>(ml);
  if (!tree) {
    return;
  }

  // process_frame is emitted once per rendered frame (Godot main thread).
  // It has no args; we compute delta locally.
  godot::Callable cb(this, "_on_godot_process_frame");
  tree->connect("process_frame", cb);

  tick_connected_ = true;
  last_tick_time_ns_ = 0;
}

void CamBANGServer::_on_godot_process_frame() {
  using clock = std::chrono::steady_clock;
  const uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());

  double delta_s = 0.0;
  if (last_tick_time_ns_ != 0 && now_ns >= last_tick_time_ns_) {
    delta_s = static_cast<double>(now_ns - last_tick_time_ns_) / 1'000'000'000.0;
  }
  last_tick_time_ns_ = now_ns;

  _on_godot_tick(delta_s);
}

bool CamBANGServer::_consume_latest_core_snapshot(bool should_emit_signal) {
  const uint64_t published_seq = runtime_.published_seq();
  if (published_seq == last_seen_published_seq_) {
    return false;
  }

  // Something changed since the last boundary observation. Latch the latest core snapshot.
  auto snap = snapshot_buffer_.snapshot_copy();
  if (!snap) {
    // Do not consume published_seq yet; retry on a later observation so we do not
    // lose this publish if the marker becomes observable before buffer visibility.
    return false;
  }
  last_seen_published_seq_ = published_seq;

  if (active_session_id_ == 0) {
    return false;
  }

  if (enforce_min_gen_gate_) {
    if (snap->gen < accepted_min_gen_) {
      // Late prior-generation publication after stop/start boundary; ignore.
      return false;
    }
    enforce_min_gen_gate_ = false;
  }

  // Establish / reset tick-bounded counters on new gen.
  // gen is defined from the Godot-facing perspective but still aligns with
  // core generations: it advances on each successful start() STOPPED->LIVE.
  if (!has_godot_counters_ || snap->gen != godot_gen_) {
    has_godot_counters_ = true;
    godot_gen_ = snap->gen;
    godot_version_ = 0;
    godot_topology_version_ = 0;
    last_emitted_topology_sig_ = runtime_.published_topology_sig();
  } else {
    // Tick-bounded version increments on every emission within a gen.
    ++godot_version_;

    // topology_version increments only when the observed topology differs
    // from the topology at the previous emission.
    const uint64_t topo_sig = runtime_.published_topology_sig();
    if (topo_sig != last_emitted_topology_sig_) {
      last_emitted_topology_sig_ = topo_sig;
      ++godot_topology_version_;
    }
  }

  latest_ = snap;

  // Export as a struct-like Variant graph for Godot inspection.
  latest_export_ = export_snapshot_to_godot(*snap, godot_gen_, godot_version_, godot_topology_version_);
  has_latest_export_ = true;

  if (should_emit_signal) {
    emit_signal("state_published",
                static_cast<uint64_t>(godot_gen_),
                static_cast<uint64_t>(godot_version_),
                static_cast<uint64_t>(godot_topology_version_));
  }

  return true;
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

  // Godot-facing snapshot truth is tick-bounded.
  // Core may publish multiple intermediate snapshots between Godot ticks.
  // We emit at most once per tick, and only if *anything* has changed since
  // the previous tick.
  (void)_consume_latest_core_snapshot(/*emit_signal=*/true);
}

godot::Variant CamBANGServer::get_state_snapshot() const {
  if (!has_latest_export_) {
    return godot::Variant();
  }
  return latest_export_;
}
godot::Error CamBANGServer::set_provider_mode(const godot::String& mode) {
  // Provider mode is latched per runtime session; changes require STOPPED.
  if (runtime_.is_running()) {
    if (!provider_mode_busy_logged_) {
      ERR_PRINT("CamBANGServer: set_provider_mode() rejected; server is LIVE. Stop the server before changing provider_mode.");
      provider_mode_busy_logged_ = true;
    }
    return godot::ERR_BUSY;
  }

  RuntimeMode parsed{};
  if (!parse_mode_string(mode, parsed)) {
    ERR_PRINT(godot::vformat(
        "CamBANGServer: set_provider_mode('%s') rejected; unknown provider_mode. Expected 'platform_backed' or 'synthetic'.",
        mode));
    return godot::ERR_INVALID_PARAMETER;
  }

  ProviderResult cap = ProviderBroker::check_mode_supported_in_build(parsed);
  if (!cap.ok()) {
    ERR_PRINT(godot::vformat(
        "CamBANGServer: set_provider_mode('%s') rejected; mode is not supported in this build.",
        mode_to_cstr(parsed)));
    return map_provider_result_to_godot_error(cap);
  }

  provider_mode_requested_ = parsed;
  return godot::OK;
}

godot::String CamBANGServer::get_provider_mode() const {
  return godot::String(mode_to_cstr(provider_mode_requested_));
}

bool CamBANGServer::_ensure_provider_attached_and_initialized() {
  if (!runtime_.is_running()) {
    return false;
  }

  // If we already have an attached provider, nothing to do.
  if (provider_ && runtime_.attached_provider() == provider_.get()) {
    return true;
  }

  // Fresh broker per start cycle (latched provider_mode).
  {
    auto broker = std::make_unique<ProviderBroker>();
    broker->set_synthetic_timeline_request_dispatch_hook(
        make_synthetic_timeline_request_dispatch_hook(runtime_));
    ProviderResult sr = broker->set_runtime_mode_requested(provider_mode_requested_);
    if (!sr.ok()) {
      ERR_PRINT(godot::vformat(
          "CamBANGServer: provider_mode='%s' is not supported in this build.",
          mode_to_cstr(provider_mode_requested_)));
      return false;
    }
    provider_ = std::move(broker);
  }
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
void CamBANGServer::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("start"), &CamBANGServer::start);
  godot::ClassDB::bind_method(godot::D_METHOD("stop"), &CamBANGServer::stop);
  godot::ClassDB::bind_method(godot::D_METHOD("set_provider_mode", "mode"), &CamBANGServer::set_provider_mode);
  godot::ClassDB::bind_method(godot::D_METHOD("get_provider_mode"), &CamBANGServer::get_provider_mode);
  godot::ClassDB::bind_method(godot::D_METHOD("get_state_snapshot"), &CamBANGServer::get_state_snapshot);

  // Internal tick hook (connected to SceneTree.process_frame).
  godot::ClassDB::bind_method(godot::D_METHOD("_on_godot_process_frame"), &CamBANGServer::_on_godot_process_frame);

  ADD_SIGNAL(godot::MethodInfo(
      "state_published",
      godot::PropertyInfo(godot::Variant::INT, "gen"),
      godot::PropertyInfo(godot::Variant::INT, "version"),
      godot::PropertyInfo(godot::Variant::INT, "topology_version")));
}

} // namespace cambang
