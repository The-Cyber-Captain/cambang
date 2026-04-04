#pragma once

#include <cstdint>
#include <memory>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "core/core_runtime.h"
#include "core/state_snapshot_buffer.h"
#include "core/snapshot/state_snapshot.h"

#include "godot/state_snapshot_export.h"

#include "imaging/broker/mode.h"
#include "imaging/synthetic/config.h"

// Provider lifecycle is owned by the server (Godot thread), but attached to the
// core runtime (core thread) via CoreRuntime::attach_provider.
#include "imaging/api/icamera_provider.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/main_loop.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>

namespace cambang {

// CamBANGServer is the release-facing lifecycle owner.
//
// Engine singleton lifetime:
// - Created at GDExtension initialization (scene init level)
// - Destroyed at GDExtension termination
//
// Work lifetime:
// - User calls start()/stop() to enable/disable core processing.
//
// Threading:
// - CoreRuntime publishes snapshots on the core thread.
// - Godot signals are emitted on the Godot main thread via a SceneTree tick hook.
class CamBANGServer final : public godot::Object {
  GDCLASS(CamBANGServer, godot::Object)

public:
  CamBANGServer();
  ~CamBANGServer() override;

  // User-facing control of core processing.
  void start();
  void stop();

  static constexpr int SYNTHETIC_ROLE_NOMINAL = static_cast<int>(SyntheticRole::Nominal);
  static constexpr int SYNTHETIC_ROLE_TIMELINE = static_cast<int>(SyntheticRole::Timeline);
  static constexpr int TIMING_DRIVER_REAL_TIME = static_cast<int>(TimingDriver::RealTime);
  static constexpr int TIMING_DRIVER_VIRTUAL_TIME = static_cast<int>(TimingDriver::VirtualTime);

  godot::Error set_platform_backed_provider();
  godot::Error set_synthetic_provider(int role, int timing_driver);

  godot::Error select_builtin_scenario(const godot::String& scenario_name);
  godot::Error load_external_scenario(const godot::String& json_text);
  godot::Error start_timeline();
  godot::Error stop_timeline();
  godot::Error set_timeline_paused(bool paused);
  godot::Error advance_timeline(uint64_t dt_ns);

  static CamBANGServer* get_singleton() noexcept { return singleton_; }

  // Return the latest Godot-facing snapshot struct (as a Variant).
  // - Before the first publish, returns NIL.
  // - After publish, returns a Dictionary matching docs/state_snapshot.md.
  godot::Variant get_state_snapshot() const;

#if defined(CAMBANG_ENABLE_DEV_NODES)
  // Dev-only escape hatch: allow dev scaffolding nodes to drive provider bring-up.
  CoreRuntime* runtime_for_dev() noexcept { return &runtime_; }

  // Dev-only access to the currently latched provider broker (if present).
  // Not bound to Godot.
  class ProviderBroker* provider_broker_for_dev() const noexcept;
#endif

protected:
  static void _bind_methods();

private:
  // Called on the Godot main thread via the SceneTree "process_frame" signal.
  void _on_godot_process_frame();

  // Core tick handler (Godot main thread) invoked by _on_godot_process_frame().
  void _on_godot_tick(double delta);

  // Consume latest core snapshot (if published_seq advanced) and optionally emit
  // state_published for this boundary observation.
  bool _consume_latest_core_snapshot(bool emit_signal);

  static CamBANGServer* singleton_;

  CoreRuntime runtime_;
  StateSnapshotBuffer snapshot_buffer_;

  // Godot-thread cached snapshot.
  std::shared_ptr<const CamBANGStateSnapshot> latest_;

  // Godot-thread cached exported snapshot (struct-like Variant graph).
  bool has_latest_export_ = false;
  godot::Dictionary latest_export_;

  // Godot-facing tick-bounded counters (truth model for state_published).
  // These are not the core's internal publication counters.
  bool has_godot_counters_ = false;
  uint64_t godot_gen_ = 0;
  uint64_t godot_version_ = 0;
  uint64_t godot_topology_version_ = 0;
  uint64_t last_emitted_topology_sig_ = 0;

  // O(1) "changed since last Godot tick" marker: core publish sequence.
  uint64_t last_seen_published_seq_ = 0;

  // Godot-boundary run/session guard.
  // - active_session_id_ is non-zero only while a start()-initiated run is active.
  // - accepted_min_gen_ prevents old-generation late publications from repopulating
  //   get_state_snapshot() across stop/start boundaries.
  uint64_t session_counter_ = 0;
  uint64_t active_session_id_ = 0;
  bool has_last_completed_gen_ = false;
  uint64_t last_completed_gen_ = 0;
  bool enforce_min_gen_gate_ = false;
  uint64_t accepted_min_gen_ = 0;

  void _ensure_tick_connected();
  bool _ensure_provider_attached_and_initialized();

  // SceneTree tick hook state.
  bool tick_connected_ = false;
  uint64_t last_tick_time_ns_ = 0;

  RuntimeMode provider_mode_requested_ = RuntimeMode::platform_backed;
  SyntheticRole synthetic_role_requested_ = SyntheticRole::Nominal;
  TimingDriver timing_driver_requested_ = TimingDriver::VirtualTime;
  bool provider_mode_busy_logged_ = false;

  // Godot-owned provider lifetime (e.g. ProviderBroker). This avoids relying on
  // temporary dev scaffolding to attach/initialize the provider.
  std::unique_ptr<ICameraProvider> provider_;
};

} // namespace cambang
