#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <string>

#include "imaging/api/provider_contract_datatypes.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
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
class CamBANGStreamResult;
class CamBANGStream;
class CamBANGCaptureResult;
class CamBANGCaptureResultSet;
class CamBANGDevice;
class CamBANGRig;

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
  // Reserved direct-lifecycle ID namespace:
  // - Low numeric IDs are commonly used by scenario-authored synthetic timeline
  //   materialization.
  // - Direct Godot lifecycle requests intentionally allocate from a high range
  //   to avoid ambiguity/collision with low authored IDs.
  // - These counters are process-monotonic and intentionally do not reset on
  //   stop(); endpoint_lifecycle_by_hardware_id_ is the stop/reset boundary state.
  static constexpr uint64_t DIRECT_DEVICE_INSTANCE_ID_BASE = 1000000000000ULL;
  static constexpr uint64_t DIRECT_ROOT_ID_BASE = 2000000000000ULL;
  static constexpr uint64_t DIRECT_STREAM_ID_BASE = 3000000000000ULL;

  CamBANGServer();
  ~CamBANGServer() override;

  static constexpr int PROVIDER_KIND_PLATFORM_BACKED = 0;
  static constexpr int PROVIDER_KIND_SYNTHETIC = 1;

  static constexpr int SYNTHETIC_ROLE_NOMINAL = 0;
  static constexpr int SYNTHETIC_ROLE_TIMELINE = 1;
  static constexpr int TIMING_DRIVER_REAL_TIME = 0;
  static constexpr int TIMING_DRIVER_VIRTUAL_TIME = 1;

  static constexpr int TIMELINE_RECONCILIATION_COMPLETION_GATED = 0;
  static constexpr int TIMELINE_RECONCILIATION_STRICT = 1;

  // User-facing control of core processing.
  godot::Error start(
      const godot::Variant& provider_kind = godot::Variant(),
      const godot::Variant& role = godot::Variant(),
      const godot::Variant& timing_driver = godot::Variant(),
      const godot::Variant& timeline_reconciliation = godot::Variant());
  void stop();
  void stop_and_quit(int64_t exit_code = 0);
  bool is_running() const;

  godot::Variant get_active_provider_config() const;
  godot::Dictionary get_provider_support() const;
  godot::Variant get_synthetic_metrics_snapshot() const;

  godot::Error select_builtin_scenario(const godot::String& scenario_name);
  godot::Error load_external_scenario(const godot::String& json_text);
  godot::Error start_scenario();
  godot::Error stop_scenario();
  godot::Error set_timeline_paused(bool paused);
  godot::Error advance_timeline(uint64_t dt_ns);

  static CamBANGServer* get_singleton() noexcept { return singleton_; }

  // Return the latest Godot-facing snapshot struct (as a Variant).
  // - Before the first publish, returns NIL.
  // - After publish, returns a Dictionary matching docs/state_snapshot.md.
  godot::Variant get_state_snapshot() const;
  godot::Array enumerate_devices() const;
  godot::Ref<CamBANGDevice> get_device_for_hardware_id(const godot::String& hardware_id) const;
  godot::Ref<CamBANGDevice> get_device(uint64_t device_instance_id) const;
  godot::Ref<CamBANGRig> get_rig(uint64_t rig_id) const;
  godot::Ref<CamBANGStreamResult> get_stream_result_by_stream_id(uint64_t stream_id) const;
  godot::Ref<CamBANGCaptureResult> get_capture_result_by_id(uint64_t capture_id, uint64_t device_instance_id) const;
  uint64_t get_latest_capture_id_for_device(uint64_t device_instance_id) const;
  godot::Ref<CamBANGCaptureResultSet> get_capture_result_set_by_id(uint64_t capture_id) const;
  void report_capture_result_member_observation(
      const SharedCaptureResultData& data,
      uint32_t image_member_index) const;
  void mark_stream_display_demand(uint64_t stream_id);
  void retain_stream_display_demand(uint64_t stream_id);
  void release_stream_display_demand(uint64_t stream_id);
  void release_stream_display_demand_async(uint64_t stream_id);
  godot::Error trigger_device_capture(
      uint64_t device_instance_id,
      uint64_t& out_capture_id);
  godot::Error set_device_still_capture_profile(uint64_t device_instance_id,
                                                const CaptureProfile& profile,
                                                const CaptureStillImageBundle& still_image_bundle);
  godot::Error set_endpoint_still_capture_profile_startup_intent(
      const godot::String& hardware_id,
      const CaptureProfile& profile,
      const CaptureStillImageBundle& still_image_bundle);
  godot::Error set_device_warm_hold_ms(uint64_t device_instance_id, uint32_t warm_hold_ms);
  godot::Error set_endpoint_warm_hold_ms_startup_intent(const godot::String& hardware_id, uint32_t warm_hold_ms);
  godot::Dictionary get_device_still_capture_profile(uint64_t device_instance_id) const;
  bool get_endpoint_capture_template_profile(const godot::String& hardware_id, CaptureProfile& out_profile) const;
  godot::Error engage_endpoint_handle(const godot::String& hardware_id, const godot::String& display_name);
  godot::Error disengage_endpoint_handle(const godot::String& hardware_id);
  godot::Ref<CamBANGStream> create_stream_for_endpoint_hardware_id(const godot::String& hardware_id);
  godot::Error destroy_direct_stream_handle(uint64_t stream_id,
                                            const godot::String& hardware_id,
                                            uint64_t device_instance_id);
  godot::Error start_direct_stream_handle(uint64_t stream_id,
                                          const godot::String& hardware_id,
                                          uint64_t device_instance_id);
  godot::Error stop_direct_stream_handle(uint64_t stream_id,
                                         const godot::String& hardware_id,
                                         uint64_t device_instance_id);
  uint64_t resolve_endpoint_instance_id(const godot::String& hardware_id) const;

protected:
  static void _bind_methods();

private:
  friend class CamBANGRig;
  // Called on the Godot main thread via the SceneTree "process_frame" signal.
  void _on_godot_process_frame();

  // Core tick handler (Godot main thread) invoked by _on_godot_process_frame().
  void _on_godot_tick(double delta);
  void _arm_live_retained_result_access_calibration_from_snapshot_(
      uint64_t now_ns,
      const std::vector<CoreBackingPlanEvaluationReport>& backing_plan_reports);
  void _observe_active_stream_evaluation_calibration_identities_(
      uint64_t now_ns,
      const std::vector<CoreBackingPlanEvaluationReport>& backing_plan_reports);
  void _observe_active_capture_evaluation_calibration_identities_(
      uint64_t now_ns,
      const std::vector<CoreBackingPlanEvaluationReport>& backing_plan_reports);
  void _process_armed_live_retained_result_access_calibration_(uint64_t now_ns);
  void _clear_live_retained_result_access_calibration_state_();
  void _drain_pending_stop_and_quit_();
  void _reconcile_endpoint_lifecycle_from_snapshot(const CamBANGStateSnapshot& snap);

  // Consume latest core snapshot (if published_seq advanced) and emit
  // state_published for this boundary observation.
  bool _consume_latest_core_snapshot();
  bool is_public_boundary_ready_() const;
  bool is_provider_discovery_available_() const;
  bool is_synthetic_timeline_session_active_() const;
  void _clear_pending_scenario_start_();
  void _reset_scenario_session_state_();
  bool _resolve_provider_endpoint_(const godot::String& hardware_id, godot::String* out_display_name) const;
  std::string _pending_endpoint_startup_key_(uint64_t session_id, const godot::String& hardware_id) const;
  godot::Error _record_pending_endpoint_startup_engage_(const godot::String& hardware_id, const godot::String& display_name);
  void _clear_pending_endpoint_startup_intents_();
  void _drain_pending_endpoint_startup_intents_after_baseline_();
  godot::Error _start_scenario_now_();
  void _drain_pending_scenario_start_after_baseline_();

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
  void _disconnect_tick_if_connected_();
  godot::Error _start_with_provider_config(
      RuntimeMode mode,
      SyntheticRole synthetic_role,
      TimingDriver timing_driver,
      bool completion_gated_destructive_sequencing_enabled);
  bool _ensure_provider_attached_and_initialized(
      RuntimeMode mode,
      SyntheticRole synthetic_role,
      TimingDriver timing_driver);

  // Server-internal helper for rig-trigger orchestration (not Godot-bound).
  uint64_t trigger_rig_capture_internal_(uint64_t rig_id);

  // SceneTree tick hook state.
  bool tick_connected_ = false;
  uint64_t last_tick_time_ns_ = 0;

  struct ArmedLiveStreamRetainedResultCalibration {
    uint64_t stream_id = 0;
    uint64_t posture_id = 0;
    uint64_t evaluation_identity = 0;
    ResultCapability display_view = ResultCapability::UNSUPPORTED;
    ResultCapability to_image = ResultCapability::UNSUPPORTED;
    uint64_t due_after_ns = 0;
  };
  struct ArmedLiveCaptureRetainedResultCalibration {
    uint64_t device_instance_id = 0;
    uint64_t capture_id = 0;
    uint64_t acquisition_session_id = 0;
    uint64_t member_identity_signature = 0;
    uint64_t evaluation_identity = 0;
    uint64_t due_after_ns = 0;
  };
  std::unordered_map<uint64_t, ArmedLiveStreamRetainedResultCalibration>
      pending_live_stream_retained_result_calibrations_;
  std::unordered_map<uint64_t, ArmedLiveStreamRetainedResultCalibration>
      completed_live_stream_retained_result_calibrations_;
  std::unordered_map<uint64_t, ArmedLiveCaptureRetainedResultCalibration>
      pending_live_capture_retained_result_calibrations_;
  std::unordered_map<uint64_t, ArmedLiveCaptureRetainedResultCalibration>
      completed_live_capture_retained_result_calibrations_;

  // Editor/debugger diagnostic flush workaround for stop_and_quit().
  static constexpr uint32_t kEditorDiagnosticQuitFlushFrames = 30;
  bool pending_stop_and_quit_ = false;
  uint32_t pending_stop_and_quit_frames_remaining_ = 0;
  int pending_stop_and_quit_exit_code_ = 0;

  void _refresh_timeline_teardown_trace_mode();
  void _handle_timeline_teardown_trace_line(const std::string& line);

  bool timeline_trace_echo_enabled_ = false;
  RuntimeMode active_runtime_mode_ = RuntimeMode::platform_backed;
  SyntheticRole active_synthetic_role_ = SyntheticRole::Nominal;
  bool completion_gated_destructive_sequencing_enabled_ = true;
  bool strict_scenario_unmet_logged_ = false;

  bool scenario_config_staged_for_session_ = false;
  bool pending_scenario_start_after_baseline_ = false;
  uint64_t pending_scenario_start_session_id_ = 0;
  bool pending_timeline_pause_after_scenario_start_ = false;
  bool pending_timeline_pause_value_ = false;

  static constexpr uint32_t PENDING_ENDPOINT_WARM_POLICY_MAX_DRAIN_TICKS = 120;

  struct PendingEndpointStartupIntent {
    uint64_t session_id = 0;
    godot::String hardware_id;
    godot::String display_name;
    bool engage_requested = false;
    bool engage_applied = false;
    bool has_still_profile = false;
    bool still_profile_applied = false;
    CaptureProfile still_profile{};
    CaptureStillImageBundle still_image_bundle = make_default_metered_still_image_bundle();
    bool has_warm_policy = false;
    uint32_t warm_hold_ms = 0;
    uint32_t warm_policy_wait_ticks = 0;
  };
  std::unordered_map<std::string, PendingEndpointStartupIntent> pending_endpoint_startup_intents_;

  // Godot-owned provider lifetime (e.g. ProviderBroker). This avoids relying on
  // temporary dev scaffolding to attach/initialize the provider.
  std::unique_ptr<ICameraProvider> provider_;
  std::atomic<uint64_t> next_capture_id_{1};
  std::atomic<uint64_t> next_direct_device_instance_id_{DIRECT_DEVICE_INSTANCE_ID_BASE};
  std::atomic<uint64_t> next_direct_root_id_{DIRECT_ROOT_ID_BASE};
  std::atomic<uint64_t> next_direct_stream_id_{DIRECT_STREAM_ID_BASE};

  struct EndpointLifecycleState {
    godot::String hardware_id;
    godot::String display_name;
    uint64_t device_instance_id = 0;
    uint64_t root_id = 0;
    bool open_requested = false;
    bool close_requested = false;
  };
  std::unordered_map<std::string, EndpointLifecycleState> endpoint_lifecycle_by_hardware_id_;
  std::unordered_map<uint64_t, godot::String> direct_stream_hardware_id_by_stream_id_;
  std::unordered_map<uint64_t, uint64_t> latest_capture_id_by_device_instance_id_;
};

} // namespace cambang
