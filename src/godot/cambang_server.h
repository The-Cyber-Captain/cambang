#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <string>

#include "imaging/api/provider_contract_datatypes.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include "core/capture_public_id.h"
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
  // Rig Capture Ids allocate from their own high range, following the same
  // convention. Device Capture Ids deliberately keep starting at 1: they
  // inherit the pre-existing capture_id space, and rebasing them would change
  // every capture id in the system for no benefit.
  //
  // The separation has to be numeric, not merely conceptual. Two independent
  // counters both starting at 1 produce a Rig Capture Id and a Device Capture
  // Id with the same VALUE on the very first rig capture -- different spaces,
  // identical number -- which is indistinguishable from a genuine collision to
  // any consistency check, and makes a log or a debugger actively misleading.
  // Scene 73 caught exactly that.
  static constexpr uint64_t RIG_CAPTURE_ID_BASE = 4000000000000ULL;

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

  // Public CamBANG FourCC-style pixel format constants for Godot Dictionary profile fields.
  // One constant per format CamBANG can name, so callers never write a raw
  // FourCC literal. Naming a format here says CamBANG has a descriptor for it,
  // not that any given provider emits it -- that remains runtime capability
  // truth, resolved by negotiation.
  static constexpr int PIXEL_FORMAT_RGBA = static_cast<int>(FOURCC_RGBA);
  static constexpr int PIXEL_FORMAT_BGRA = static_cast<int>(FOURCC_BGRA);
  static constexpr int PIXEL_FORMAT_NV12 = static_cast<int>(FOURCC_NV12);
  static constexpr int PIXEL_FORMAT_NV21 = static_cast<int>(FOURCC_NV21);
  static constexpr int PIXEL_FORMAT_I420 = static_cast<int>(FOURCC_I420);
  static constexpr int PIXEL_FORMAT_YV12 = static_cast<int>(FOURCC_YV12);
  static constexpr int PIXEL_FORMAT_YUY2 = static_cast<int>(FOURCC_YUY2);
  static constexpr int PIXEL_FORMAT_UYVY = static_cast<int>(FOURCC_UYVY);

  // Terminal disposition of one Device Capture, as carried by the
  // capture_finished signals (capture_identity_and_lifecycle.md 4.3). Mirrors
  // CoreCaptureAssemblyRegistry::TerminalState so a caller never writes a bare
  // number: a completion whose reason cannot be named is only half reported.
  //
  // All six are bound because they are the model's vocabulary. Only four can
  // reach capture_finished today -- LATE_EXCLUDED and NEVER_ARRIVED are
  // produced solely as cohort member outcomes, and become caller-visible when
  // 4.3's per-member reporting lands. See 9.4.
  static constexpr int DISPOSITION_DELIVERED =
      static_cast<int>(CoreCaptureAssemblyRegistry::TerminalState::DELIVERED);
  static constexpr int DISPOSITION_FAILED =
      static_cast<int>(CoreCaptureAssemblyRegistry::TerminalState::FAILED);
  static constexpr int DISPOSITION_LATE_EXCLUDED =
      static_cast<int>(CoreCaptureAssemblyRegistry::TerminalState::LATE_EXCLUDED);
  static constexpr int DISPOSITION_PREEMPTED_BY_RIG =
      static_cast<int>(CoreCaptureAssemblyRegistry::TerminalState::PREEMPTED_BY_RIG);
  static constexpr int DISPOSITION_DEVICE_LOST =
      static_cast<int>(CoreCaptureAssemblyRegistry::TerminalState::DEVICE_LOST);
  static constexpr int DISPOSITION_NEVER_ARRIVED =
      static_cast<int>(CoreCaptureAssemblyRegistry::TerminalState::NEVER_ARRIVED);

  // Why a Rig Capture's cohort closed (4.4). WINDOW_EXPIRED means the
  // simultaneity window ran out with members still outstanding -- the result
  // set may be short, and that is the correct outcome rather than a failure.
  static constexpr int COHORT_CLOSED_ALL_MEMBERS_TERMINAL =
      static_cast<int>(CoreCaptureCohortRegistry::CohortClosedReason::ALL_MEMBERS_TERMINAL);
  static constexpr int COHORT_CLOSED_WINDOW_EXPIRED =
      static_cast<int>(CoreCaptureCohortRegistry::CohortClosedReason::WINDOW_EXPIRED);

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
  // Per-access cost evidence, provider-neutral. The same evidence is also
  // embedded in get_synthetic_metrics_snapshot(), but that accessor returns
  // nothing unless a SyntheticProvider snapshot is available, which made the
  // measurement unreachable on exactly the platform-backed runs where access
  // cost matters most.
  godot::Dictionary get_result_access_timing_evidence() const;
  // Provider-neutral view of Core's backing-plan evaluation reports, sourced
  // directly from CoreRuntime rather than through the synthetic-metrics crutch,
  // so it is available under every provider. Returns a NIL Variant when the
  // runtime is not running; otherwise an Array of per-target report
  // Dictionaries. Diagnostic/verification surface only -- these reports are
  // deliberately not part of the published snapshot (see
  // docs/status_panel_surface_policy.md); this reads Core registry-backed truth
  // on demand.
  godot::Variant get_backing_plan_evaluation_diagnostics() const;

  godot::Error select_builtin_scenario(const godot::String& scenario_name);
  godot::Error load_external_scenario(const godot::String& json_text);
  godot::Error ingest_camera_description(const godot::String& json_text);
  godot::Error set_capture_geolocation(const godot::Dictionary& geolocation);
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
  // Form a rig from two or more engaged devices and return its bound handle,
  // or null if the ingested concurrency truth does not authorize the
  // combination. The server mints the rig_id (like capture_id).
  //
  // Takes device handles, matching CamBANGRig::add_member/remove_member. The
  // caller already holds CamBANGDevice objects; routing them out to hardware-id
  // strings and back invites a typo that no type check can catch, and made rig
  // creation the one place in the rig surface that spoke a different language
  // from the rest of it.
  godot::Ref<CamBANGRig> create_rig(const godot::TypedArray<CamBANGDevice>& members);
  // Hardware id of a device handle, whichever way it was obtained: an endpoint
  // handle carries it directly, one from get_device() resolves through the
  // runtime. Empty when the handle names no device.
  godot::String resolve_device_hardware_id(const godot::Ref<CamBANGDevice>& device) const;
  godot::Ref<CamBANGStreamResult> get_stream_result_by_stream_id(uint64_t stream_id) const;
  // Fetch one capture's result by the Device Capture Id a trigger returned.
  //
  // Took a device_instance_id alongside until the id spaces were split (2.1).
  // That argument disambiguated nothing afterwards -- a Device Capture Id
  // belongs to exactly one device -- and a parameter that outlives its reason
  // teaches callers a constraint that is not real.
  godot::Ref<CamBANGCaptureResult> get_capture_result_by_id(const godot::String& capture_id) const;
  uint64_t get_latest_capture_id_for_device(uint64_t device_instance_id) const;

  // Captures this boundary has minted and not yet seen finish
  // (capture_identity_and_lifecycle.md 4.5). Returns:
  //
  //   {
  //     by_device:    { <device_instance_id>: [ <device_capture_id>, ... ] },
  //     rig_captures: [ <rig_capture_id>, ... ],
  //     total:        int   // device captures + rig captures
  //   }
  //
  // Rig-member captures appear under by_device like any other Device Capture,
  // because that is what they are; the rig's own id appears in rig_captures.
  //
  // Exposed rather than left to each caller because a device's busy state is
  // otherwise observable ONLY by being refused, and every non-trivial consumer
  // written against this codebase has hand-rolled the same counter to avoid
  // that.
  //
  // CAVEAT, and it is not about timing: an empty entry for a device does NOT
  // guarantee the next trigger is admitted. Admission refuses for reasons
  // unrelated to busy-ness -- materialization backlog, orchestration failure --
  // and those surface as ERR_BUSY too. A caller still handles the trigger's
  // own return.
  godot::Dictionary get_unfinished_captures() const;

  // What became of each member of a rig capture
  // (capture_identity_and_lifecycle.md 4.3). One entry per member, in
  // hardware-id order, whether or not that member produced an image:
  //
  //   { hardware_id: String, device_instance_id: int,
  //     device_capture_id: int, disposition: int, error_code: int }
  //
  // This exists because a result set alone cannot answer the question a
  // caller actually has. A member that failed contributes no result, so a
  // two-camera rig that lost one returns a single result and no explanation --
  // identical to a rig that only ever had one member. `disposition` compares
  // against DISPOSITION_*; `error_code` is the provider error where one was
  // reported, 0 otherwise.
  //
  // Empty until the rig capture finishes, which is what rig_capture_finished
  // announces: outcomes are decided at cohort closure.
  godot::Array get_capture_member_outcomes_by_id(const godot::String& rig_capture_id) const;

  // Rig participation of a Device Capture, for CamBANGCaptureResult's identity
  // (2.3). Not bound: a caller reads it through the result it already holds.
  std::optional<CoreRuntime::RigParticipationForServer>
  rig_participation_for_device_capture(uint64_t device_capture_id) const;

  // Internal <-> public capture id (capture_identity_and_lifecycle.md 2.2).
  // Not bound to Godot: these translate for the wrappers, which is why they sit
  // here rather than on the scripting surface.
  //
  // Lookup returns an empty String for an internal id this session never
  // minted, and 0 for a public id that is unknown OR belongs to the other
  // space. Refusing a wrong-space id is 2.2's requirement, not a nicety: a Rig
  // Capture Id accepted where a Device Capture Id belongs would resolve to
  // nothing and read as "no such capture" instead of "wrong kind of id".
  godot::String device_capture_public_id(uint64_t internal_id) const;
  godot::String rig_capture_public_id(uint64_t internal_id) const;
  uint64_t device_capture_internal_id(const godot::String& public_id) const;
  uint64_t rig_capture_internal_id(const godot::String& public_id) const;
  godot::TypedArray<CamBANGCaptureResult> get_capture_result_set_by_id(const godot::String& capture_id) const;
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
  // Device-scoped capture picture (pattern appearance) update; parses the full
  // PictureConfig from the dict, gated on supports_capture_picture_updates().
  godot::Error set_device_capture_picture(uint64_t device_instance_id,
                                          const godot::Dictionary& picture_def);
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
  godot::Ref<CamBANGStream> create_stream_for_endpoint_hardware_id(
      const godot::String& hardware_id,
      const godot::Variant& definition);
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

  // Rig membership mutation, called by CamBANGRig once it has resolved a
  // device handle to a hardware id. Hardware id is the participation identity
  // (capture_identity_and_lifecycle.md 2.3); the public surface takes a
  // CamBANGDevice so callers never handle the string themselves.
  godot::Error add_rig_member_by_hardware_id(uint64_t rig_id,
                                             const godot::String& hardware_id);
  godot::Error remove_rig_member_by_hardware_id(uint64_t rig_id,
                                                const godot::String& hardware_id);
  // A CamBANGDevice built from an instance id carries no hardware id (see
  // CamBANGDevice::set_server_and_instance, which clears it), so a handle from
  // get_device() must be resolved through the runtime. Returns empty when the
  // device is unknown.
  godot::String resolve_hardware_id_for_instance(uint64_t device_instance_id) const;

protected:
  static void _bind_methods();

private:
  friend class CamBANGDevice;
  friend class CamBANGStream;
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
  void _refresh_tracked_wrapper_live_states_from_snapshot_();
  // Drains Core's settlement queues and emits, once per Godot tick.
  void _emit_capture_completion_signals_();
  void _set_all_tracked_wrapper_live_states_false_();
  bool _is_device_live_by_identity_(const godot::String& hardware_id,
                                    uint64_t device_instance_id) const;
  bool _is_stream_result_live_by_identity_(uint64_t stream_id) const;
  godot::Ref<CamBANGDevice> _canonical_device_for_hardware_id_(
      const std::string& hardware_id, const godot::String& display_name) const;
  godot::Ref<CamBANGRig> _canonical_rig_for_id_(uint64_t rig_id) const;
  void register_tracked_device_wrapper_(uint64_t wrapper_object_id);
  void register_tracked_rig_wrapper_(uint64_t wrapper_object_id);
  void register_tracked_stream_wrapper_(uint64_t wrapper_object_id);

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
  // capture_id == 0 means the trigger was rejected and `error` carries the
  // mapped public result: ERR_UNCONFIGURED for the ImagingSpec admission-gate
  // categories (missing/rejected camera-concurrency truth -- a permanent
  // configuration gap the caller must fix via ingest_camera_description()),
  // ERR_BUSY for every other category (tranche 7 deliberately maps only the
  // configuration gate; see docs/dev/current_tranche.md at that date).
  struct RigTriggerInternalResult {
    // Rig Capture Id, and each member's hardware id paired with that member's
    // own Device Capture Id (capture_identity_and_lifecycle.md 4.1).
    uint64_t rig_capture_id = 0;
    std::vector<CoreRuntime::RigTriggeredMember> members;
    godot::Error error = godot::ERR_BUSY;
  };
  RigTriggerInternalResult trigger_rig_capture_internal_(uint64_t rig_id);
  // One-shot per runtime session: the first rejected rig trigger logs its
  // concrete orchestration failure category so collapsed public codes stay
  // self-describing in the log. Reset on start().
  bool rig_trigger_rejection_logged_ = false;

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
    // Shape only: member count, per-member index, posture id and to_image
    // truth. Deliberately excludes capture_id and retained_frame_id, which
    // member_identity_signature above mixes in, so this survives across
    // captures of the same access domain.
    uint64_t member_shape_signature = 0;
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
  struct SyntheticStreamResultObservation {
    uint64_t retained_frame_id = 0;
    uint64_t revision = 0;
  };
  // Scenario-only metrics state. It deliberately exposes an opaque revision,
  // never a retained-frame identity, through the synthetic diagnostics seam.
  mutable std::map<uint64_t, SyntheticStreamResultObservation>
      synthetic_stream_result_observations_;
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
  // Two id spaces, not one counter (capture_identity_and_lifecycle.md 2.1).
  // A Device Capture Id is drawn for every device capture, standalone or rig
  // member; a Rig Capture Id names the rig capture itself. Both are
  // session-scoped and both are minted here -- Core must not become a second
  // allocator for either. Collapsing these back into one counter reintroduces
  // the member/cohort collision and is caught by provider_compliance_verify.
  std::atomic<uint64_t> next_device_capture_id_{1};
  std::atomic<uint64_t> next_rig_capture_id_{RIG_CAPTURE_ID_BASE};
  std::atomic<uint64_t> next_rig_id_{1};
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
  std::unordered_set<uint64_t> tracked_device_wrapper_object_ids_;
  std::unordered_set<uint64_t> tracked_stream_wrapper_object_ids_;
  std::unordered_set<uint64_t> tracked_rig_wrapper_object_ids_;

  // Public capture ids (2.2). Minted at the boundary at trigger time; Core
  // keeps using the uint64 throughout, so nothing here is on a hot path.
  //
  // Godot-thread only, like the rest of the boundary state. Both directions are
  // kept because both are needed: reporting translates internal -> public,
  // and an id-keyed lookup translates public -> internal.
  //
  // These grow for the life of a session, one pair of entries per capture, and
  // are cleared on stop(). A long soak accumulates them; pruning in step with
  // the result store's retention would bound it, at the cost of coupling the
  // boundary to Core's retirement schedule.
  // Latest Rig Capture Id per rig, the counterpart of
  // latest_capture_id_by_device_instance_id_ for devices.
  //
  // Exists so retained-result calibration can find "the most recent capture on
  // this rig" without reading it out of the published snapshot. The snapshot
  // publishes tick-bounded STATE; using it as a lookup index is the same
  // category error that put staleness guards in the harnesses.
  std::unordered_map<uint64_t, uint64_t> latest_rig_capture_id_by_rig_id_;

  CapturePublicIdMinter capture_public_id_minter_;
  std::unordered_map<uint64_t, std::string> device_capture_public_by_internal_;
  std::unordered_map<std::string, uint64_t> device_capture_internal_by_public_;
  std::unordered_map<uint64_t, std::string> rig_capture_public_by_internal_;
  std::unordered_map<std::string, uint64_t> rig_capture_internal_by_public_;
  godot::String mint_device_capture_public_id_(uint64_t internal_id);
  godot::String mint_rig_capture_public_id_(uint64_t internal_id);

  // Outstanding work (4.5). Godot-thread only: written by the trigger
  // wrappers and erased by the per-tick completion drain, both of which run
  // there, so no lock is needed and none should be added.
  //
  // An id enters only once its trigger was ACCEPTED. A refused trigger burns
  // an id without returning it, and recording that would leave a capture
  // outstanding forever that the caller was never told about.
  std::unordered_map<uint64_t, uint64_t> unfinished_device_capture_device_by_id_;
  std::unordered_set<uint64_t> unfinished_rig_capture_ids_;

  // Canonical wrapper instances, one per id (section 4.2). A handle is only
  // worth subscribing to if the next lookup returns the same object: with a
  // fresh instance per call, a caller connects to a wrapper the server has
  // already forgotten, and the signal it is waiting for fires on a different
  // one. Identity comparison (a == b) also stops working, which is how a
  // caller would naturally keep a set of devices.
  //
  // One map per accessor, because the two device handle kinds are not
  // interchangeable: an endpoint handle resolves its instance id through the
  // endpoint lifecycle, an instance handle carries it directly. Each accessor
  // is canonical for its own key, which is what section 4.2 requires and what
  // per-object signals need. Unifying them was tried and reverted -- see
  // get_device().
  //
  // Strong refs: the server keeps these alive, which is the whole point --
  // a canonical wrapper that dies when the caller drops it is not canonical.
  // Cleared on stop(), because the ids they name do not survive the session.
  mutable std::unordered_map<std::string, godot::Ref<CamBANGDevice>>
      canonical_device_by_hardware_id_;
  mutable std::unordered_map<uint64_t, godot::Ref<CamBANGDevice>>
      canonical_device_by_instance_id_;
  mutable std::unordered_map<uint64_t, godot::Ref<CamBANGRig>>
      canonical_rig_by_id_;
};

} // namespace cambang
