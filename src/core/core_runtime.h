// src/core/core_runtime.h
#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/core_dispatcher.h"
#include "core/core_acquisition_session_registry.h"
#include "core/core_capture_assembly_registry.h"
#include "core/core_capture_cohort_registry.h"
#include "core/core_device_registry.h"
#include "core/core_native_object_registry.h"
#include "core/core_result_store.h"
#include "core/core_rig_registry.h"
#include "core/core_runtime_state.h"
#include "core/core_spec_state.h"
#include "core/external_camera_description_state.h"
#include "core/provider_camera_fact_state.h"
#include "core/core_stream_registry.h"
#include "core/core_thread.h"
#include "core/core_frame_sink.h"
#include "core/i_state_snapshot_publisher.h"
#include "core/provider_callback_ingress.h"

#include "core/snapshot/snapshot_builder.h"


#include "imaging/api/icamera_provider.h"
#if !defined(CAMBANG_INTERNAL_SMOKE)
#include "imaging/broker/banner_info.h"
#endif

namespace cambang {


enum class BackingPlanEvaluationPrimaryFunction : uint8_t {
  StreamDisplayView = 0,
  CaptureReadyAndMaterialize = 1,
};

enum class BackingPlanEvaluationCompletionReason : uint8_t {
  None = 0,
  AllViableCandidatesEvaluated = 1,
  LiveDisplayDemandFamilyCrossing = 2,
  SingleViableCandidate = 3,
};

enum class CaptureEvidenceIncompleteReason : uint8_t {
  None = 0,
  NoRequiredBundle = 1,
  AwaitingCaptureReady = 2,
  AwaitingRequiredMemberMaterialization = 3,
  RequiredMemberUnsupported = 4,
};

struct CoreBackingPlanCandidateEvidenceReport {
  CoreRetainedProductionPlan candidate{};
  bool observation_seen = false;
  bool evidence_complete = false;
  bool evidence_accepted = false;
  ResultCapability provisional_display_view = ResultCapability::UNSUPPORTED;
  bool has_display_view_elapsed_ns = false;
  uint64_t display_view_elapsed_ns = 0;
  ResultCapability provisional_to_image = ResultCapability::UNSUPPORTED;
  bool has_materialization_elapsed_ns = false;
  uint64_t materialization_elapsed_ns = 0;
  bool has_capture_ready_elapsed_ns = false;
  uint64_t capture_ready_elapsed_ns = 0;
  bool has_total_elapsed_ns = false;
  uint64_t total_elapsed_ns = 0;
  uint32_t required_capture_member_count = 0;
  uint32_t observed_capture_member_count = 0;
  uint32_t materialized_capture_member_count = 0;
  bool has_first_missing_required_capture_member_index = false;
  uint32_t first_missing_required_capture_member_index = 0;
  CaptureEvidenceIncompleteReason capture_evidence_incomplete_reason =
      CaptureEvidenceIncompleteReason::None;
  bool has_normalized_cost_units = false;
  uint64_t normalized_cost_units = 0;
  bool has_observed_posture = false;
  CoreProductionPostureShape observed_posture =
      CoreProductionPostureShape::CpuPrimary;
  uint64_t observed_access_posture_id = 0;
  uint64_t observed_stream_id = 0;
  uint64_t observed_capture_id = 0;
  uint64_t observed_acquisition_session_id = 0;
  uint32_t observed_image_member_index = 0;
  ResultPayloadKind observed_payload_kind = ResultPayloadKind::CPU_PACKED;
  bool observed_has_retained_cpu_payload = false;
  bool observed_has_retained_gpu_backing = false;
  bool observed_gpu_materialization_available = false;
  bool observed_gpu_materialization_requires_readback = false;
};

struct CoreBackingPlanEvaluationReport {
  enum class ParentKind : uint8_t {
    Stream = 0,
    AcquisitionSession = 1,
    CapturePriming = 2,
  };

  enum class TargetKind : uint8_t {
    Stream = 0,
    Capture = 1,
  };

  ParentKind parent_kind = ParentKind::Stream;
  uint64_t parent_id = 0;
  uint64_t stream_id = 0;
  uint64_t acquisition_session_id = 0;
  uint64_t device_instance_id = 0;
  bool provisional_parent = false;
  BackingPlanEvaluationPrimaryFunction primary_function =
      BackingPlanEvaluationPrimaryFunction::StreamDisplayView;
  TargetKind target_kind = TargetKind::Stream;
  uint64_t target_id = 0;
  CoreRetainedProductionPlan requested{};
  CoreRetainedProductionPlan steady{};
  bool evaluator_active = false;
  uint8_t current_candidate_index = 0;
  std::vector<CoreRetainedProductionPlan> candidate_sequence{};
  std::vector<CoreBackingPlanCandidateEvidenceReport> candidate_evidence{};
  bool decision_from_evaluation = false;
  CoreRetainedProductionPlan decision_selected{};
  std::vector<CoreRetainedProductionPlan> decision_candidate_sequence{};
  BackingPlanEvaluationCompletionReason completion_reason =
      BackingPlanEvaluationCompletionReason::None;
};

struct CoreCaptureLifecycleTimingReport {
  uint64_t capture_id = 0;
  uint64_t device_instance_id = 0;
  uint64_t acquisition_session_id = 0;
  bool has_capture_started_ingested_steady_ns = false;
  uint64_t capture_started_ingested_steady_ns = 0;
  bool has_capture_completed_ingested_steady_ns = false;
  uint64_t capture_completed_ingested_steady_ns = 0;
  bool has_capture_failed_ingested_steady_ns = false;
  uint64_t capture_failed_ingested_steady_ns = 0;
};

enum class TrySetStreamPictureStatus : uint8_t {
  OK = 0,
  NotSupported = 1,
  Busy = 2,
  InvalidArgument = 3,
};

enum class TrySetCapturePictureStatus : uint8_t {
  OK = 0,
  NotSupported = 1,
  Busy = 2,
  InvalidArgument = 3,
};

enum class TrySetStillCaptureProfileStatus : uint8_t {
  OK = 0,
  NotSupported = 1,
  Busy = 2,
  InvalidArgument = 3,
};

enum class TrySetWarmHoldStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
};

enum class TryTriggerDeviceCaptureStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
  ProviderRejected = 3,
  Unavailable = 4,
};

enum class TryCreateStreamStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
};

enum class TryStartStreamStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
  ProviderRejected = 3,
};

enum class TryStopStreamStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
  ProviderRejected = 3,
};

enum class TryDestroyStreamStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
  Started = 3,
  ProviderRejected = 4,
};

enum class TryOpenDeviceStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
  ProviderRejected = 3,
};

enum class TryCloseDeviceStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
  ProviderRejected = 3,
};

  class CoreRuntime final : private CoreThread::IHooks {
  private:
    enum class ShutdownPhase : uint8_t;  // forward declaration

  public:
    struct Stats {
    uint64_t publish_requests_coalesced = 0;
    uint64_t publish_requests_dropped_full = 0;
    uint64_t publish_requests_dropped_closed = 0;
    uint64_t publish_requests_dropped_allocfail = 0;
    uint64_t display_demand_release_async_dropped_full = 0;
    uint64_t display_demand_release_async_dropped_closed = 0;
    uint64_t display_demand_release_async_dropped_allocfail = 0;
  };

  CoreRuntime();
  ~CoreRuntime();

  CoreRuntime(const CoreRuntime&) = delete;
  CoreRuntime& operator=(const CoreRuntime&) = delete;

  bool start();
  void stop();

  bool is_running() const { return core_thread_.is_running(); }

  // Tick-bounded publication bridge support.
  //
  // Core may publish multiple snapshots between Godot ticks. Godot-facing code
  // needs an O(1) marker to detect "something changed" since the previous tick,
  // without polling/coalescing in user code.
  //
  // - published_seq() increments once per successful core snapshot publish.
  // - published_topology_sig() is the core-computed topology signature of the
  //   latest published snapshot, suitable for boundary-side topology diffing.
  uint64_t published_seq() const noexcept {
    return published_seq_.load(std::memory_order_acquire);
  }

  uint64_t published_topology_sig() const noexcept {
    return published_topology_sig_.load(std::memory_order_acquire);
  }

  CoreRuntimeState state_copy() const noexcept {
    return state_.load(std::memory_order_acquire);
  }

  // Best-effort compatibility shim for disposable work only; drops are intentionally
  // not surfaced. Retained runtime truth must use a [[nodiscard]] admission-returning
  // path instead.
  void post(CoreThread::Task task);

  CoreThread::PostResult try_post(CoreThread::Task task);

  // Dev/internal stream lifecycle surfaces.
  // Defaulting is performed by core using provider->stream_template().
  // profile_version ownership is core-authoritative for this ingress:
  // pass profile_version=0 to request core-assigned lineage.
  // These are non-blocking and may return Busy if the provider_to_core_commands queue is full.
  TryCreateStreamStatus try_create_stream(
      uint64_t stream_id,
      uint64_t device_instance_id,
      StreamIntent intent,
      const CaptureProfile* request_profile,
      const PictureConfig* request_picture,
      uint64_t profile_version) noexcept;

  TryStartStreamStatus try_start_stream(uint64_t stream_id) noexcept;

  TryStopStreamStatus try_stop_stream(uint64_t stream_id) noexcept;

  TryDestroyStreamStatus try_destroy_stream(uint64_t stream_id) noexcept;

  TryOpenDeviceStatus try_open_device(
      const std::string& hardware_id,
      uint64_t device_instance_id,
      uint64_t root_id) noexcept;

  TryCloseDeviceStatus try_close_device(uint64_t device_instance_id) noexcept;

  // Stream-scoped picture update path.
  // Non-blocking: enqueues the provider call onto the core thread.
  TrySetStreamPictureStatus try_set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) noexcept;
  // Device-scoped capture-picture update path.
  TrySetCapturePictureStatus try_set_capture_picture_config(uint64_t device_instance_id, const PictureConfig& picture) noexcept;
  TrySetStillCaptureProfileStatus try_set_device_still_capture_profile(
      uint64_t device_instance_id,
      const CaptureProfile& profile,
      const CaptureStillImageBundle& still_image_bundle) noexcept;
  TrySetWarmHoldStatus try_set_device_warm_hold_ms(uint64_t device_instance_id, uint32_t warm_hold_ms) noexcept;

  // Server-facing synchronous wrappers. They marshal registry/provider access onto
  // the core thread and only return success after the work was accepted/submitted.
  TryTriggerDeviceCaptureStatus try_trigger_device_capture_with_capture_id_for_server(
      uint64_t device_instance_id,
      uint64_t capture_id);
  bool materialize_capture_request_for_server(uint64_t device_instance_id, CaptureRequest& out) const;

  // Compatibility alias for smoke/internal callers; still marshals to the core thread.
  bool materialize_capture_request(uint64_t device_instance_id, CaptureRequest& out) const;

  enum class RigPreflightFailure : uint8_t {
    None = 0,
    RigNotFound = 1,
    EmptyMembership = 2,
    HardwareIdUnresolved = 3,
    HardwareIdAmbiguous = 4,
    DuplicateResolvedDevice = 5,
    MaterializeFailed = 6,
  };

  struct RigPreflightParticipant {
    std::string hardware_id;
    uint64_t device_instance_id = 0;
    CaptureRequest request{};
  };

  struct RigPreflightResult {
    bool ok = false;
    RigPreflightFailure failure = RigPreflightFailure::None;
    uint64_t rig_id = 0;
    size_t failure_member_index = 0;
    std::string failure_hardware_id;
    uint64_t failure_device_instance_id = 0;
    std::vector<RigPreflightParticipant> participants;
  };

  enum class RigCohortAdmissionFailure : uint8_t {
    None = 0,
    InvalidCaptureId = 1,
    PreflightFailed = 2,
    EmptyParticipants = 3,
    ImagingSpecUnavailable = 4,
    ImagingSpecRejected = 5,
    DuplicateCaptureId = 6,
  };

  struct RigAdmittedParticipantRequest {
    std::string hardware_id;
    CaptureRequest request{};
  };

  struct RigAdmittedRequestBundle {
    bool ok = false;
    RigCohortAdmissionFailure failure = RigCohortAdmissionFailure::None;
    uint64_t capture_id = 0;
    uint64_t rig_id = 0;
    std::vector<RigAdmittedParticipantRequest> participants;
  };

  enum class RigSubmissionFailure : uint8_t {
    None = 0,
    InvalidBundle = 1,
    ProviderUnavailable = 2,
    TriggerFailed = 3,
  };

  struct RigSubmissionResult {
    bool ok = false;
    RigSubmissionFailure failure = RigSubmissionFailure::None;
    uint64_t capture_id = 0;
    uint64_t rig_id = 0;
    size_t submitted_count = 0;
    size_t failed_index = 0;
    uint64_t failed_device_instance_id = 0;
    uint32_t provider_error_code = 0;
  };

  enum class RigOrchestrationFailure : uint8_t {
    None = 0,
    InvalidCaptureId = 1,
    PreflightFailed = 2,
    AdmissionFailed = 3,
    SubmissionFailed = 4,
  };

  struct RigTriggerOrchestrationResult {
    bool ok = false;
    RigOrchestrationFailure failure = RigOrchestrationFailure::None;
    uint64_t rig_id = 0;
    uint64_t capture_id = 0;
    RigPreflightFailure preflight_failure = RigPreflightFailure::None;
    RigCohortAdmissionFailure admission_failure = RigCohortAdmissionFailure::None;
    RigSubmissionFailure submission_failure = RigSubmissionFailure::None;
    size_t submitted_count = 0;
    size_t failed_index = 0;
    uint64_t failed_device_instance_id = 0;
    uint32_t provider_error_code = 0;
  };

  enum class ReplaceCaptureGeolocationStatus : uint8_t { Ok, Busy, Invalid };
  ReplaceCaptureGeolocationStatus replace_capture_geolocation_for_server(
      std::optional<CaptureGeolocation> replacement);

#if defined(CAMBANG_INTERNAL_SMOKE)
  ReplaceCaptureGeolocationStatus smoke_replace_capture_geolocation(
      double latitude_degrees, double longitude_degrees,
      std::optional<double> altitude_meters);
  ReplaceCaptureGeolocationStatus smoke_clear_capture_geolocation();
  bool smoke_set_capture_datetime_utc_nanoseconds(int64_t unix_epoch_nanoseconds);
  uint64_t smoke_capture_admission_clock_sample_count() const noexcept;
  void smoke_reset_capture_admission_clock_sample_count() noexcept;
  std::optional<CaptureAdmissionContext> smoke_capture_admission_context(
      uint64_t capture_id, uint64_t device_instance_id) const;
  RigPreflightResult preflight_rig_participants_materialize(uint64_t rig_id) const;
  bool smoke_set_rig_member_hardware_ids(uint64_t rig_id, std::vector<std::string> member_hardware_ids);
  RigAdmittedRequestBundle smoke_admit_rig_cohort_from_preflight(
      uint64_t rig_id,
      uint64_t capture_id,
      const RigPreflightResult& preflight);
  RigTriggerOrchestrationResult smoke_orchestrate_rig_capture_from_preflight(
      uint64_t rig_id,
      uint64_t capture_id,
      const RigPreflightResult& preflight);
  RigSubmissionResult smoke_submit_admitted_rig_bundle(const RigAdmittedRequestBundle& bundle);
  RigTriggerOrchestrationResult smoke_orchestrate_rig_capture_with_capture_id(
      uint64_t rig_id,
      uint64_t capture_id);

  struct ImagingSpecRetainedStateForSmoke {
    uint64_t imaging_spec_version = 0;
    CoreSpecState::ImagingSpecRetentionKind retention_kind =
        CoreSpecState::ImagingSpecRetentionKind::None;
    std::vector<uint8_t> payload{};
  };

  std::optional<ImagingSpecRetainedStateForSmoke> imaging_spec_retained_state_for_smoke() const;
  std::optional<ExternalCameraDescriptionEntry>
  active_external_camera_description_for_smoke(const std::string& camera_id) const;
  size_t active_external_camera_description_count_for_smoke() const;
  uint64_t active_external_camera_description_version_for_smoke() const;
  std::optional<ProviderCameraFacts> provider_camera_facts_for_smoke(
      uint64_t device_instance_id) const;
  std::optional<ProviderCaptureImageFacts> provider_capture_image_facts_for_smoke(
      uint64_t capture_id,
      uint64_t device_instance_id,
      uint32_t image_member_index) const;

#endif

  bool retain_rig_member_hardware_ids(uint64_t rig_id, const std::vector<std::string>& member_hardware_ids);

  enum class IngestCameraConcurrencyStatus : uint8_t {
    Ok = 0,
    Busy = 1,
    ParseError = 2,
    Invalid = 3,
  };

  struct IngestCameraConcurrencyResult {
    IngestCameraConcurrencyStatus status = IngestCameraConcurrencyStatus::Ok;
    std::string error_message;
    uint64_t imaging_spec_version = 0;
  };

  IngestCameraConcurrencyResult ingest_camera_concurrency_json_for_server(
      const std::string& json_text);

  enum class ReplaceExternalCameraDescriptionStatus : uint8_t {
    Ok = 0,
    Busy = 1,
    ParseError = 2,
    Invalid = 3,
  };

  struct ReplaceExternalCameraDescriptionResult {
    ReplaceExternalCameraDescriptionStatus status =
        ReplaceExternalCameraDescriptionStatus::Ok;
    std::string error_message;
    uint64_t camera_description_version = 0;
    uint64_t imaging_spec_version = 0;
  };

  // Internal seam; it deliberately has no Godot binding until Tranche 3.
  ReplaceExternalCameraDescriptionResult replace_external_camera_description_json_for_internal(
      const std::string& json_text);

  // Server-internal adapter: caller supplies capture_id (no allocation here).
  RigTriggerOrchestrationResult orchestrate_rig_capture_with_capture_id_for_server(
      uint64_t rig_id,
      uint64_t capture_id);

#if defined(CAMBANG_INTERNAL_SMOKE)
  CoreThread::PostResult try_post_core_thread_unchecked(CoreThread::Task task) {
    return core_thread_.try_post(std::move(task));
  }
#endif

  void request_publish();

  Stats stats_copy() const noexcept;

  ProviderCallbackIngress::Stats ingress_stats_copy() const noexcept { return ingress_.stats_copy(); }

  struct ShutdownDiag {
    uint8_t phase_code = 0;
    uint64_t phase_changes = 0;
  };

  ShutdownDiag shutdown_diag_copy() const noexcept;

#if defined(CAMBANG_INTERNAL_SMOKE)
  // Smoke-only: avoid hard-coded numeric coupling in tests.
  static constexpr uint8_t shutdown_phase_exit_code() noexcept {
    return static_cast<uint8_t>(ShutdownPhase::EXIT);
  }

  void smoke_set_frame_sink(ICoreFrameSink* sink) noexcept {
    dispatcher_.set_frame_sink(sink);
  }

  void smoke_hold_provider_fact_timer_ticks(bool hold) noexcept {
    smoke_hold_provider_fact_timer_ticks_.store(hold, std::memory_order_release);
    if (!hold) {
      core_thread_.request_timer_tick();
    }
  }

  uint32_t smoke_ingress_depth_for_stream(uint64_t stream_id) const noexcept {
    return ingress_.ingress_depth_for_stream(stream_id);
  }
#endif

  void set_snapshot_publisher(IStateSnapshotPublisher* publisher) noexcept {
    snapshot_publisher_.store(publisher, std::memory_order_release);
  }

  [[nodiscard]] CoreDispatchStats dispatcher_stats() const noexcept { return dispatcher_.stats(); }

  const CoreStreamRegistry::StreamRecord* stream_record(uint64_t stream_id) const noexcept {
    return streams_.find(stream_id);
  }
  const CoreDeviceRegistry::DeviceRecord* device_record(uint64_t device_instance_id) const noexcept {
    return devices_.find(device_instance_id);
  }
#if defined(CAMBANG_INTERNAL_SMOKE)
  std::optional<CoreCaptureAssemblyRegistry::DeviceCaptureAssembly>
  capture_assembly_for_smoke(uint64_t capture_id,
                             uint64_t device_instance_id) const {
    return capture_assembly_registry_.find_for_smoke(
        capture_id, device_instance_id);
  }
  SharedCaptureResultData capture_result_unguarded_for_smoke(
      uint64_t capture_id,
      uint64_t device_instance_id) const {
    return result_store_.get_capture_result(capture_id, device_instance_id);
  }
#endif

  std::vector<CoreBackingPlanEvaluationReport> backing_plan_evaluation_reports() const;
  std::vector<CoreCaptureLifecycleTimingReport>
  recent_capture_lifecycle_timing_reports() const;

  // Narrow internal backing-plan evaluation handoff. Godot-side retained-result
  // calibration reports structural/support truth plus measured public-operation
  // timing back to Core so parent-scoped requested vs steady planning can
  // settle without inventing a separate benchmarking subsystem.
  void report_stream_retained_display_view_observation(
      uint64_t stream_id,
      uint64_t posture_id,
      ResultCapability provisional_display_view,
      bool has_display_view_elapsed_ns,
      uint64_t display_view_elapsed_ns);
  void report_stream_retained_to_image_observation(
      uint64_t stream_id,
      uint64_t posture_id,
      ResultCapability provisional_to_image,
      bool has_normalized_cost_units,
      uint64_t normalized_cost_units);
  void report_capture_retained_to_image_observation(
      uint64_t device_instance_id,
      uint64_t capture_id,
      uint64_t acquisition_session_id_hint,
      uint64_t posture_id,
      ResultCapability provisional_to_image,
      bool has_materialization_elapsed_ns,
      uint64_t materialization_elapsed_ns,
      bool has_normalized_cost_units,
      uint64_t normalized_cost_units,
      uint32_t image_member_index = 0);
  uint64_t stream_backing_plan_evaluation_settle_delay_ns() const noexcept;
  uint64_t capture_backing_plan_evaluation_settle_delay_ns() const noexcept;

  IProviderCallbacks* provider_callbacks() { return &ingress_; }

  SharedStreamResultData get_latest_stream_result(uint64_t stream_id) const {
    return result_store_.get_latest_stream_result(stream_id);
  }

  SharedCaptureResultData get_capture_result(uint64_t capture_id, uint64_t device_instance_id) const;
  std::vector<SharedCaptureResultData> get_capture_result_set(uint64_t capture_id) const;
  void mark_stream_display_demand(uint64_t stream_id) {
    result_store_.mark_stream_display_demand(stream_id, ns_since_epoch_());
  }
  void retain_stream_display_demand(uint64_t stream_id) {
    result_store_.retain_stream_display_demand(stream_id);
  }
  void release_stream_display_demand(uint64_t stream_id) {
    result_store_.release_stream_display_demand(stream_id);
  }
  bool is_stream_display_demand_active(uint64_t stream_id) const {
    return result_store_.is_stream_display_demand_active(stream_id, ns_since_epoch_());
  }
  void release_stream_display_demand_async(uint64_t stream_id);


  void attach_provider(ICameraProvider* provider) noexcept {
    provider_.store(provider, std::memory_order_release);

    // Ensure the core loop observes the attachment promptly so the
    // core-loop banner can print even if no other work is scheduled.
    // Safe to call from any thread.
    if (provider != nullptr) {
      core_thread_.request_timer_tick();
    }
  }

  // Non-owning access to the currently attached provider. Intended for
  // Godot-side orchestration (e.g. virtual_time pumping) without leaking
  // provider type knowledge into Core.
  ICameraProvider* attached_provider() const noexcept {
    return provider_.load(std::memory_order_acquire);
  }

  // Core-owned identity/spec truth retention hooks.
  // These are internal runtime surfaces used by orchestrators that issue provider calls.
  // Invalid input is rejected without enqueueing and reported as Closed because no
  // retained-truth admission occurred.
  [[nodiscard]] CoreThread::PostResult retain_device_identity(uint64_t device_instance_id, const std::string& hardware_id);
  [[nodiscard]] CoreThread::PostResult retain_camera_spec_version(const std::string& hardware_id, uint64_t camera_spec_version);
  [[nodiscard]] CoreThread::PostResult retain_device_capture_profile(uint64_t device_instance_id,
                                                                     uint32_t width,
                                                                     uint32_t height,
                                                                     uint32_t format,
                                                                     uint64_t capture_profile_version);
  [[nodiscard]] CoreThread::PostResult retain_rig_capture_profile(uint64_t rig_id,
                                                                  uint32_t width,
                                                                  uint32_t height,
                                                                  uint32_t format,
                                                                  uint64_t capture_profile_version);
  // The retained imaging-spec payload is opaque to Core. Callers using the
  // patch path must therefore pass the effective post-apply retained payload.
  [[nodiscard]] CoreThread::PostResult retain_imaging_spec_version(uint64_t imaging_spec_version);
  [[nodiscard]] CoreThread::PostResult retain_imaging_spec_replace(uint64_t imaging_spec_version,
                                                                   SpecPatchView effective_spec);
  [[nodiscard]] CoreThread::PostResult retain_imaging_spec_patch(uint64_t imaging_spec_version,
                                                                 SpecPatchView effective_spec);

  // Internal dev visibility: allow the Godot main thread to echo a core-thread
  // banner line via UtilityFunctions::print for environments where stdout isn't
  // reliably visible (e.g. Godot editor output on Windows).
  bool take_core_banner_line(char* out, size_t cap) noexcept {
    if (!out || cap == 0) {
      return false;
    }
    if (!core_banner_line_pending_.exchange(false, std::memory_order_acq_rel)) {
      return false;
    }
    // Core thread writes the buffer before setting the pending flag.
    std::strncpy(out, core_banner_line_, cap - 1);
    out[cap - 1] = '\0';
    return true;
  }

private:
  void on_core_start() override;
  void on_core_timer_tick() override;
  void on_core_stop() override;

  void enqueue_provider_fact(ProviderToCoreCommand&& cmd);
  void enqueue_request(CoreThread::Task task);
  void request_publish_from_core_unchecked();
  void begin_capture_stream_preemption_(uint64_t capture_id, uint64_t device_instance_id);
  void begin_capture_stream_preemption_for_bundle_(const RigAdmittedRequestBundle& bundle);
  void release_result_safe_capture_stream_preemptions_();
  bool is_stream_preempted_for_capture_(uint64_t stream_id) const;
  bool suppress_repeating_stream_frame_for_capture_(ProviderToCoreCommand&& cmd);
  size_t suppress_queued_repeating_stream_frames_for_capture_();
  // Core-thread-only helpers. These read/write core-owned registries directly.
  bool materialize_capture_request_(uint64_t device_instance_id, CaptureRequest& out) const;
  TryTriggerDeviceCaptureStatus trigger_device_capture_with_capture_id_(
      uint64_t device_instance_id,
      uint64_t capture_id);
  RigPreflightResult preflight_rig_participants_materialize_(uint64_t rig_id) const;
  RigAdmittedRequestBundle admit_rig_cohort_from_preflight_(
      uint64_t rig_id,
      uint64_t capture_id,
      const RigPreflightResult& preflight);
  RigTriggerOrchestrationResult orchestrate_rig_capture_from_preflight_(
      uint64_t rig_id,
      uint64_t capture_id,
      const RigPreflightResult& preflight);
  RigCohortAdmissionFailure grouped_rig_imaging_spec_admission_failure_(
      const RigPreflightResult& preflight) const noexcept;
  RigSubmissionResult submit_admitted_rig_bundle_(const RigAdmittedRequestBundle& bundle);
  RigTriggerOrchestrationResult orchestrate_rig_capture_with_capture_id_(
      uint64_t rig_id,
      uint64_t capture_id);
  CaptureAdmissionContext make_capture_admission_context_() const;
  CoreResolvedCaptureImageFacts resolve_capture_image_facts_(
      uint64_t capture_id, uint64_t device_instance_id,
      uint32_t image_member_index) const;
  void finalize_completed_capture_facts_(
      uint64_t capture_id, uint64_t device_instance_id);
  bool build_effective_capture_request_without_retained_plan_(
      uint64_t device_instance_id,
      CaptureRequest& out) const;
  bool resolve_stream_backing_capabilities_(
      uint64_t device_instance_id,
      uint64_t stream_id,
      StreamIntent intent,
      const CaptureProfile& profile,
      const PictureConfig& picture,
      ProducerBackingCapabilities& runtime_backing_capabilities,
      ProducerBackingCapabilities& parent_context_backing_capabilities);
  bool resolve_capture_backing_capabilities_(
      uint64_t device_instance_id,
      const CaptureRequest& request,
      ProducerBackingCapabilities& runtime_backing_capabilities,
      ProducerBackingCapabilities& parent_context_backing_capabilities);
  bool refresh_stream_retained_plan_state_(
      uint64_t stream_id,
      bool apply_to_provider,
      bool requested_bump_access_posture_epoch);
  bool refresh_capture_retained_plan_state_(
      uint64_t device_instance_id,
      bool requested_bump_access_posture_epoch);
  bool sync_capture_parent_priming_(
      uint64_t device_instance_id,
      const CaptureRequest& effective,
      const ProducerBackingCapabilities& runtime_backing_capabilities,
      const ProducerBackingCapabilities& parent_context_backing_capabilities);
  void release_capture_parent_priming_(
      uint64_t device_instance_id,
      const char* reason = nullptr);
  bool device_has_any_stream_(uint64_t device_instance_id) const noexcept;
  bool device_has_active_capture_evaluator_(
      uint64_t device_instance_id) const noexcept;
  struct CaptureRetainedPlanParentKey {
    enum class Kind : uint8_t {
      AcquisitionSession = 0,
      CapturePriming = 1,
    };

    Kind kind = Kind::CapturePriming;
    uint64_t id = 0;

    bool operator<(const CaptureRetainedPlanParentKey& other) const noexcept {
      if (kind != other.kind) {
        return kind < other.kind;
      }
      return id < other.id;
    }
  };
  struct ResolvedCaptureRetainedPlanParent {
    CaptureRetainedPlanParentKey key{};
    uint64_t acquisition_session_id = 0;
    uint64_t device_instance_id = 0;
    bool provisional = true;
  };
  ResolvedCaptureRetainedPlanParent
  resolve_capture_retained_plan_parent_(
      uint64_t device_instance_id,
      uint64_t preferred_acquisition_session_id = 0) const;
  bool integrate_pending_provider_facts_before_capture_request_();
  bool rehome_capture_retained_plan_parent_state_(
      uint64_t device_instance_id,
      uint64_t preferred_acquisition_session_id = 0);
  void erase_capture_retained_plan_state_for_device_(
      uint64_t device_instance_id,
      const CaptureRetainedPlanParentKey* keep = nullptr);
  void handle_stream_retained_to_image_observation_(
      uint64_t stream_id,
      uint64_t posture_id,
      ResultCapability provisional_to_image,
      bool has_normalized_cost_units,
      uint64_t normalized_cost_units);
  void handle_stream_retained_display_view_observation_(
      uint64_t stream_id,
      uint64_t posture_id,
      ResultCapability provisional_display_view,
      bool has_display_view_elapsed_ns,
      uint64_t display_view_elapsed_ns);
  void handle_capture_retained_to_image_observation_(
      uint64_t device_instance_id,
      uint64_t capture_id,
      uint64_t acquisition_session_id_hint,
      uint64_t posture_id,
      ResultCapability provisional_to_image,
      bool has_materialization_elapsed_ns,
      uint64_t materialization_elapsed_ns,
      bool has_normalized_cost_units,
      uint64_t normalized_cost_units,
      uint32_t image_member_index,
      uint8_t deferred_retries_remaining = 0);
  void enqueue_pending_capture_observation_(
      uint64_t device_instance_id,
      uint64_t capture_id,
      uint64_t acquisition_session_id_hint,
      uint64_t posture_id,
      ResultCapability provisional_to_image,
      bool has_materialization_elapsed_ns,
      uint64_t materialization_elapsed_ns,
      bool has_normalized_cost_units,
      uint64_t normalized_cost_units,
      uint32_t image_member_index,
      uint8_t deferred_retries_remaining,
      uint64_t not_before_ns);
  void process_pending_capture_observations_(
      uint64_t now_ns,
      bool& has_next_delay,
      uint64_t& next_delay_ns);
  void mark_capture_retained_plan_state_orphaned_for_device_(
      uint64_t device_instance_id,
      uint64_t retire_after_ns);
  size_t retire_expired_capture_retained_plan_orphans_(uint64_t now_ns);
  void next_capture_retained_plan_orphan_retirement_delay_(
      uint64_t now_ns,
      bool& has_next_delay,
      uint64_t& next_delay_ns) const;
  void account_display_demand_release_async_post_failure_(CoreThread::PostResult result) noexcept;
  std::vector<SharedCaptureResultData> curate_capture_result_set_accept_all_assembly_successful_(
      std::vector<SharedCaptureResultData> candidates) const;

private:
  struct MeasuredPlanEvidence;
  struct RetainedPlanEvaluatorState;
  struct RetainedPlanDecisionProvenance;

  CoreThread core_thread_;
  CoreRigRegistry rigs_;
  CoreDeviceRegistry devices_;
  CoreAcquisitionSessionRegistry acquisition_sessions_;
  CoreSpecState spec_state_;
  CoreStreamRegistry streams_;
  CoreNativeObjectRegistry native_objects_;
  // result_store_, capture_assembly_registry_, and capture_cohort_registry_
  // are the deliberate exceptions to the core-thread-only registry model used
  // by the members above: get_capture_result[_set]() read them directly from
  // the calling (e.g. Godot) thread, so each provides its own internal lock.
  // See the threading-model note on CoreResultStore for the full rationale.
  CoreResultStore result_store_;
  CoreCaptureAssemblyRegistry capture_assembly_registry_;
  CoreCaptureCohortRegistry capture_cohort_registry_;

  // Snapshot header counters (schema v1).
  // gen: core generation counter, monotonic across app/server lifetime.
  // version: per-publish within gen.
  // topology_version: structural within gen.
  std::uint64_t gen_counter_ = 0;
  std::uint64_t current_gen_ = 0;
  std::uint64_t version_ = 0;
  std::uint64_t topology_version_ = 0;
  uint64_t last_topology_sig_ = 0;
  bool has_topology_sig_ = false;

  std::atomic<CoreRuntimeState> state_{CoreRuntimeState::CREATED};

  SnapshotBuilder snapshot_builder_;
  std::atomic<IStateSnapshotPublisher*> snapshot_publisher_{nullptr};

  // Core-defined epoch for snapshot timestamp_ns (session-relative monotonic).
  // Stored as an atomic nanosecond count (steady_clock::time_since_epoch())
  // rather than a plain time_point: on_core_start() writes this on the core
  // thread on every start()/restart, while core_monotonic_now_ns_ns_since_epoch_()
  // is called from provider threads (per IProviderCallbacks::core_monotonic_now_ns's
  // documented "safe from any provider thread" contract) and from CamBANGServer via
  // mark_stream_display_demand()/is_stream_display_demand_active() on the Godot
  // thread. A plain time_point had no synchronization between that write and
  // these cross-thread reads.
  std::atomic<int64_t> epoch_steady_ns_{0};

  // Nanoseconds elapsed since epoch_steady_ns_ at the given instant, safe to
  // call from any thread. Overload for callers that already captured "now"
  // once (e.g. a single core-timer-tick pump using one consistent timestamp
  // across several calculations) and must not silently drift by re-sampling
  // the clock at each call site.
  uint64_t ns_since_epoch_(std::chrono::steady_clock::time_point now) const noexcept {
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        now.time_since_epoch()).count();
    const int64_t delta = now_ns - epoch_steady_ns_.load(std::memory_order_acquire);
    return delta > 0 ? static_cast<uint64_t>(delta) : 0;
  }

  // Nanoseconds elapsed since epoch_steady_ns_, safe to call from any thread.
  uint64_t ns_since_epoch_() const noexcept {
    return ns_since_epoch_(std::chrono::steady_clock::now());
  }

  // Reentrancy tripwire for the icamera_provider.h contract on
  // update_stream_retained_production_plan(): "providers must not emit a
  // frame synchronously from this call". Set to the stream_id for the
  // duration of that provider call (always made from the core thread);
  // ProviderCallbackIngress::on_frame() checks this alongside
  // core_thread_.is_core_thread() to detect a provider violating it by
  // delivering a frame synchronously, same-thread, from within the call
  // (never a false positive for a correctly-implemented provider posting a
  // frame asynchronously through CBProviderStrand's own thread).
  std::atomic<uint64_t> applying_stream_retained_plan_for_stream_id_{0};

  CoreDispatcher dispatcher_;
  ProviderCallbackIngress ingress_;

#if defined(CAMBANG_INTERNAL_SMOKE)
  std::atomic<bool> smoke_hold_provider_fact_timer_ticks_{false};
#endif

  std::deque<ProviderToCoreCommand> provider_facts_;
  size_t provider_capture_facts_queued_ = 0;

  struct MeasuredPlanEvidence {
    struct CaptureMemberMaterializationEvidence {
      bool observed = false;
      ResultCapability provisional_to_image = ResultCapability::UNSUPPORTED;
      bool has_materialization_elapsed_ns = false;
      uint64_t materialization_elapsed_ns = 0;
      bool has_normalized_cost_units = false;
      uint64_t normalized_cost_units = 0;
    };

    bool observed_display_view = false;
    bool display_view_evidence_complete = false;
    bool display_view_evidence_accepted = false;
    ResultCapability provisional_display_view = ResultCapability::UNSUPPORTED;
    bool has_display_view_elapsed_ns = false;
    uint64_t display_view_elapsed_ns = 0;
    bool observed_to_image = false;
    bool capture_evidence_complete = false;
    bool capture_evidence_accepted = false;
    ResultCapability provisional_to_image = ResultCapability::UNSUPPORTED;
    bool has_materialization_elapsed_ns = false;
    uint64_t materialization_elapsed_ns = 0;
    bool has_capture_ready_elapsed_ns = false;
    uint64_t capture_ready_elapsed_ns = 0;
    bool has_total_elapsed_ns = false;
    uint64_t total_elapsed_ns = 0;
    uint32_t required_capture_member_count = 0;
    uint32_t observed_capture_member_count = 0;
    uint32_t materialized_capture_member_count = 0;
    bool has_first_missing_required_capture_member_index = false;
    uint32_t first_missing_required_capture_member_index = 0;
    CaptureEvidenceIncompleteReason capture_evidence_incomplete_reason =
        CaptureEvidenceIncompleteReason::None;
    bool has_normalized_cost_units = false;
    uint64_t normalized_cost_units = 0;
    bool has_observed_posture = false;
    CoreProductionPostureShape observed_posture =
        CoreProductionPostureShape::CpuPrimary;
    uint64_t observed_access_posture_id = 0;
    uint64_t observed_stream_id = 0;
    uint64_t observed_capture_id = 0;
    uint64_t observed_acquisition_session_id = 0;
    uint32_t observed_image_member_index = 0;
    ResultPayloadKind observed_payload_kind = ResultPayloadKind::CPU_PACKED;
    bool observed_has_retained_cpu_payload = false;
    bool observed_has_retained_gpu_backing = false;
    bool observed_gpu_materialization_available = false;
    bool observed_gpu_materialization_requires_readback = false;
    std::vector<CaptureMemberMaterializationEvidence>
        capture_member_materialization{};
  };

  struct CapturePrimingSeedSignature {
    std::string hardware_id;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format_fourcc = 0;
    PictureConfig picture{};
    CaptureStillImageBundle still_image_bundle =
        make_default_metered_still_image_bundle();
    ProducerBackingCapabilities runtime_backing_capabilities{};
    ProducerBackingCapabilities parent_context_backing_capabilities{};
  };

  struct CapturePrimingSeed {
    CapturePrimingSeedSignature signature{};
    CoreRetainedProductionPlan selected{};
  };

  struct CaptureParentPrimingState {
    CapturePrimingSeedSignature signature{};
    bool provider_hold_active = false;
  };

  struct RetainedPlanEvaluatorState {
    uint64_t device_instance_id = 0;
    uint64_t acquisition_session_id = 0;
    uint64_t orphan_retire_after_ns = 0;
    uint64_t current_candidate_ready_after_ns = 0;
    BackingPlanEvaluationPrimaryFunction primary_function =
        BackingPlanEvaluationPrimaryFunction::StreamDisplayView;
    BackingPlanEvaluationCompletionReason completion_reason =
        BackingPlanEvaluationCompletionReason::None;
    CapturePrimingSeedSignature capture_priming_seed_signature{};
    bool active = false;
    uint8_t candidate_count = 0;
    uint8_t current_candidate_index = 0;
    CoreRetainedProductionPlan candidate_sequence[3]{};
    MeasuredPlanEvidence evidence[3]{};
  };

  struct RetainedPlanDecisionProvenance {
    uint64_t device_instance_id = 0;
    uint64_t acquisition_session_id = 0;
    uint64_t orphan_retire_after_ns = 0;
    bool valid = false;
    bool from_evaluation = false;
    BackingPlanEvaluationPrimaryFunction primary_function =
        BackingPlanEvaluationPrimaryFunction::StreamDisplayView;
    BackingPlanEvaluationCompletionReason completion_reason =
        BackingPlanEvaluationCompletionReason::None;
    CapturePrimingSeedSignature capture_priming_seed_signature{};
    CoreRetainedProductionPlan selected{};
    uint8_t candidate_count = 0;
    CoreRetainedProductionPlan candidate_sequence[3]{};
    MeasuredPlanEvidence evidence[3]{};
  };

  static bool plan_is_strictly_better_for_stream_(
      const MeasuredPlanEvidence& candidate,
      const MeasuredPlanEvidence* current_best) noexcept;
  static bool plan_is_strictly_better_for_capture_(
      const MeasuredPlanEvidence& candidate,
      const MeasuredPlanEvidence* current_best) noexcept;
  static bool infer_stream_result_posture_shape_(
      const SharedStreamResultData& result,
      CoreProductionPostureShape& out) noexcept;
  static bool infer_capture_member_posture_shape_(
      const CoreCaptureResultData::ImageMemberData& member,
      CoreProductionPostureShape& out) noexcept;
  static void fill_stream_observation_identity_(
      MeasuredPlanEvidence& evidence,
      const SharedStreamResultData& result,
      CoreProductionPostureShape observed_posture) noexcept;
  static void fill_capture_observation_identity_(
      MeasuredPlanEvidence& evidence,
      const SharedCaptureResultData& result,
      const CoreCaptureResultData::ImageMemberData& member,
      CoreProductionPostureShape observed_posture) noexcept;
  static bool same_observation_identity_(
      const MeasuredPlanEvidence& a,
      const MeasuredPlanEvidence& b) noexcept;
  static bool same_capture_observation_family_(
      const MeasuredPlanEvidence& a,
      const MeasuredPlanEvidence& b) noexcept;
  static bool capture_member_matches_required_bundle_(
      const CaptureStillImageBundle& bundle,
      const CoreCaptureResultData::ImageMemberData& member) noexcept;
  static void recompute_capture_materialization_aggregate_(
      MeasuredPlanEvidence& evidence,
      const CaptureStillImageBundle& bundle) noexcept;
  static CoreBackingPlanCandidateEvidenceReport build_candidate_evidence_report_(
      CoreRetainedProductionPlan candidate,
      const MeasuredPlanEvidence& evidence) noexcept;
  std::vector<CoreBackingPlanEvaluationReport>
  backing_plan_evaluation_reports_on_core_thread_() const;
  std::vector<CoreCaptureLifecycleTimingReport>
  recent_capture_lifecycle_timing_reports_on_core_thread_() const;
  void note_capture_lifecycle_ingress_(
      const CoreCaptureLifecycleIngressEvent& event);
  static RetainedPlanDecisionProvenance build_decision_provenance_(
      const RetainedPlanEvaluatorState& state,
      CoreRetainedProductionPlan selected) noexcept;
  static RetainedPlanDecisionProvenance build_non_evaluated_decision_provenance_(
      BackingPlanEvaluationPrimaryFunction primary_function,
      uint64_t device_instance_id,
      uint64_t acquisition_session_id,
      CoreRetainedProductionPlan requested,
      CoreRetainedProductionPlan steady,
      uint8_t candidate_count = 0,
      const CoreRetainedProductionPlan* candidate_sequence = nullptr) noexcept;
  CapturePrimingSeedSignature build_capture_priming_seed_signature_(
      uint64_t device_instance_id,
      const CaptureRequest& effective,
      ProducerBackingCapabilities runtime_backing_capabilities,
      ProducerBackingCapabilities parent_context_backing_capabilities) const;
  bool try_find_capture_priming_seed_(
      const CapturePrimingSeedSignature& signature,
      CoreRetainedProductionPlan& out_selected) const;
  void remember_capture_priming_seed_(
      const CapturePrimingSeedSignature& signature,
      CoreRetainedProductionPlan selected);

  std::map<uint64_t, RetainedPlanEvaluatorState> stream_retained_plan_evaluators_;
  std::map<CaptureRetainedPlanParentKey, RetainedPlanEvaluatorState>
      capture_retained_plan_evaluators_;
  std::map<uint64_t, RetainedPlanDecisionProvenance> stream_retained_plan_decisions_;
  std::map<CaptureRetainedPlanParentKey, RetainedPlanDecisionProvenance>
      capture_retained_plan_decisions_;
  std::map<std::string, CapturePrimingSeed> capture_priming_seeds_;
  std::map<uint64_t, CaptureParentPrimingState> capture_parent_priming_states_;

  struct CaptureStreamPreemptionRecord {
    uint64_t capture_id = 0;
    uint64_t device_instance_id = 0;
  };
  struct PendingCaptureObservation {
    uint64_t device_instance_id = 0;
    uint64_t capture_id = 0;
    uint64_t acquisition_session_id_hint = 0;
    uint64_t posture_id = 0;
    ResultCapability provisional_to_image = ResultCapability::UNSUPPORTED;
    bool has_materialization_elapsed_ns = false;
    uint64_t materialization_elapsed_ns = 0;
    bool has_normalized_cost_units = false;
    uint64_t normalized_cost_units = 0;
    uint32_t image_member_index = 0;
    uint8_t deferred_retries_remaining = 0;
    uint64_t not_before_ns = 0;
  };
  std::map<uint64_t, std::map<uint64_t, CaptureStreamPreemptionRecord>> capture_stream_preemptions_by_device_;
  std::deque<PendingCaptureObservation> pending_capture_observations_;
  std::map<std::pair<uint64_t, uint64_t>, CoreCaptureLifecycleTimingReport>
      recent_capture_lifecycle_timing_reports_;
  std::vector<std::pair<uint64_t, uint64_t>>
      recent_capture_lifecycle_timing_order_;
  std::deque<CoreThread::Task> requests_;

  enum class ShutdownPhase : uint8_t {
    NONE = 0,
    STOP_STREAMS,
    AWAIT_STREAMS_STOPPED,
    DESTROY_STREAMS,
    AWAIT_STREAMS_DESTROYED,
    CLOSE_DEVICES,
    AWAIT_DEVICES_CLOSED,
    PROVIDER_SHUTDOWN,
    FINAL_RETENTION_SWEEP,
    FINAL_PUBLISH,
    CLEAR_DESTROYED_RETAINED_NATIVE_OBJECTS,
    EXIT
  };

  // Cross-thread stop signal. Consumed on the core thread and folded into
  // shutdown_requested_ so shutdown choreography remains core-thread-owned.
  std::atomic<bool> shutdown_requested_from_stop_{false};
  bool shutdown_requested_ = false;
  ShutdownPhase shutdown_phase_ = ShutdownPhase::NONE;
  std::atomic<uint8_t> shutdown_phase_code_{0};
  std::atomic<uint64_t> shutdown_phase_changes_{0};
  bool shutdown_final_publish_requested_ = false;
  uint32_t shutdown_wait_ticks_ = 0;

  std::atomic<ICameraProvider*> provider_{nullptr};
  bool provider_banner_printed_ = false; // core-thread only; reset each start()

  // One-line banner echo mailbox (core thread -> Godot thread).
  std::atomic<bool> core_banner_line_pending_{false};
  char core_banner_line_[192] = {0};

  std::atomic<bool> publish_pending_{false};

  // Publish markers (core thread writes; any thread reads).
  // These do not redefine the snapshot schema; they exist to support the
  // Godot-facing tick-bounded truth model cheaply.
  std::atomic<uint64_t> published_seq_{0};
  std::atomic<uint64_t> published_topology_sig_{0};
  std::atomic<uint64_t> create_stream_profile_version_seq_{1};

  std::atomic<uint64_t> publish_requests_coalesced_{0};
  std::atomic<uint64_t> publish_requests_dropped_full_{0};
  std::atomic<uint64_t> publish_requests_dropped_closed_{0};
  std::atomic<uint64_t> publish_requests_dropped_allocfail_{0};
  std::atomic<uint64_t> display_demand_release_async_dropped_full_{0};
  std::atomic<uint64_t> display_demand_release_async_dropped_closed_{0};
  std::atomic<uint64_t> display_demand_release_async_dropped_allocfail_{0};
  mutable std::mutex configured_imaging_spec_mutex_;
  uint64_t configured_camera_description_version_ = 0;
  uint64_t configured_imaging_spec_version_ = 0;
  std::vector<uint8_t> configured_imaging_spec_payload_{};
  std::optional<ExternalCameraDescriptionState> configured_external_camera_description_{};
  ExternalCameraDescriptionState active_external_camera_description_{};
  ProviderCameraFactState provider_camera_fact_state_{};
  uint64_t active_camera_description_version_ = 0;
  uint64_t next_configured_camera_description_version_ = 1;
  uint64_t next_configured_imaging_spec_version_ = 1;
  mutable std::mutex configured_capture_geolocation_mutex_;
  std::optional<CaptureGeolocation> configured_capture_geolocation_{};
  std::optional<CaptureGeolocation> active_capture_geolocation_{};
  std::atomic<int64_t> capture_admission_clock_override_ns_{INT64_MIN};
  mutable std::atomic<uint64_t> capture_admission_clock_sample_count_{0};

  static constexpr uint64_t kDestroyedNativeObjectRetentionWindowNs = 5ull * 1000ull * 1000ull * 1000ull;

  // Retention window for capture_cohort_registry_ entries (ledger #52).
  // Deliberately generous and comfortably longer than the default capture-
  // admission watchdog timeout (30s) so a cohort is never retired while a
  // participant could still legitimately be resolving.
  static constexpr uint64_t kCaptureCohortRetentionWindowNs =
      300ull * 1000ull * 1000ull * 1000ull; // 5 minutes

  // Retention window for capture_assembly_registry_/result_store_ terminal
  // capture entries (ledger #52). Time-based, not supersession-based: see
  // CoreCaptureAssemblyRegistry::retire_terminal_older_than()'s doc comment
  // for why "a newer capture exists for this device" is not a safe signal
  // that an older one is unused (Core's own result-access-cost calibration
  // feedback loop can legitimately reference an old capture well after a
  // newer one on the same device). Deliberately the same generous window as
  // kCaptureCohortRetentionWindowNs.
  static constexpr uint64_t kCaptureResultRetentionWindowNs =
      300ull * 1000ull * 1000ull * 1000ull; // 5 minutes
};

} // namespace cambang
