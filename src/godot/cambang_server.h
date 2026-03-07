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

  /**
   * set_provider_mode(mode: String) -> Error
   *
   * Godot-facing runtime provider mode selection.
   *
   * Valid values:
   *   - "platform_backed" (default; intended to be safe)
   *   - "synthetic" (only if compiled in)
   *
   * Rules:
   *   - Provider mode is latched per runtime session.
   *   - This may ONLY be called while the server is STOPPED
   *     (before start() or after stop()).
   *   - If called while LIVE, ERR_BUSY is returned (and logged once).
   *   - If the requested mode is not supported in this build,
   *     ERR_UNAVAILABLE is returned.
   *
   * Changing provider_mode requires a stop() → set_provider_mode() → start() cycle.
   */
  godot::Error set_provider_mode(const godot::String& mode);

  /// Returns the currently requested (latched-for-next-start) provider_mode string.
  godot::String get_provider_mode() const;

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

  void _ensure_tick_connected();
  bool _ensure_provider_attached_and_initialized();

  // SceneTree tick hook state.
  bool tick_connected_ = false;
  uint64_t last_tick_time_ns_ = 0;

  RuntimeMode provider_mode_requested_ = RuntimeMode::platform_backed;
  bool provider_mode_busy_logged_ = false;

  // Godot-owned provider lifetime (e.g. ProviderBroker). This avoids relying on
  // temporary dev scaffolding to attach/initialize the provider.
  std::unique_ptr<ICameraProvider> provider_;
};

} // namespace cambang