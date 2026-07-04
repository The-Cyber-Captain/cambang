#include "godot/cambang_server.h"
#include "godot/cambang_capture_result.h"
#include "godot/cambang_capture_result_set.h"
#include "godot/cambang_device.h"
#include "godot/cambang_stream.h"
#include "godot/cambang_stream_result.h"
#include "godot/cambang_stream_result_internal.h"
#include "godot/result_access_cost_evidence.h"
#include "godot/retained_result_access_calibration.h"
#include "godot/synthetic_gpu_backing_bridge.h"
#include "godot/godot_gpu_display_service.h"
#include "godot/cambang_rig.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <vector>

#include "core/synthetic_timeline_request_binding.h"
#include "imaging/api/timeline_teardown_trace.h"
#include "imaging/api/provider_error_string.h"
#include "imaging/broker/provider_broker.h"
#include "imaging/broker/banner_info.h"

#ifndef CAMBANG_GDE_TARGET_PLATFORM
#define CAMBANG_GDE_TARGET_PLATFORM "unknown"
#endif

#ifndef CAMBANG_GDE_PLATFORM_PROVIDER_FAMILY
#define CAMBANG_GDE_PLATFORM_PROVIDER_FAMILY "unknown"
#endif

#ifndef CAMBANG_GDE_PLATFORM_PROVIDER_STATUS
#define CAMBANG_GDE_PLATFORM_PROVIDER_STATUS "unknown"
#endif


namespace cambang {

CamBANGServer* CamBANGServer::singleton_ = nullptr;


namespace {

struct LiveRetainedCalibrationMetrics final {
  uint64_t arm_calls = 0;
  uint64_t observe_stream_calls = 0;
  uint64_t observe_capture_calls = 0;
  uint64_t process_calls = 0;
  uint64_t arm_total_ns = 0;
  uint64_t observe_stream_total_ns = 0;
  uint64_t observe_capture_total_ns = 0;
  uint64_t process_total_ns = 0;
  uint64_t arm_pending_stream_count = 0;
  uint64_t observe_stream_pending_stream_count = 0;
  uint64_t observe_capture_pending_capture_count = 0;
  uint64_t process_pending_stream_count = 0;
  uint64_t process_pending_capture_count = 0;
};

static LiveRetainedCalibrationMetrics g_live_retained_calibration_metrics;

static double ns_to_ms_u64(uint64_t ns) noexcept {
  return static_cast<double>(ns) / 1'000'000.0;
}

static godot::Dictionary live_retained_calibration_metrics_snapshot() {
  godot::Dictionary d;
  d["live_retained_calibration_arm_calls"] =
      static_cast<uint64_t>(g_live_retained_calibration_metrics.arm_calls);
  d["live_retained_calibration_observe_stream_calls"] =
      static_cast<uint64_t>(g_live_retained_calibration_metrics.observe_stream_calls);
  d["live_retained_calibration_observe_capture_calls"] =
      static_cast<uint64_t>(g_live_retained_calibration_metrics.observe_capture_calls);
  d["live_retained_calibration_process_calls"] =
      static_cast<uint64_t>(g_live_retained_calibration_metrics.process_calls);
  d["live_retained_calibration_arm_total_ms"] =
      ns_to_ms_u64(g_live_retained_calibration_metrics.arm_total_ns);
  d["live_retained_calibration_observe_stream_total_ms"] =
      ns_to_ms_u64(g_live_retained_calibration_metrics.observe_stream_total_ns);
  d["live_retained_calibration_observe_capture_total_ms"] =
      ns_to_ms_u64(g_live_retained_calibration_metrics.observe_capture_total_ns);
  d["live_retained_calibration_process_total_ms"] =
      ns_to_ms_u64(g_live_retained_calibration_metrics.process_total_ns);
  d["live_retained_calibration_arm_pending_stream_count"] =
      static_cast<uint64_t>(g_live_retained_calibration_metrics.arm_pending_stream_count);
  d["live_retained_calibration_observe_stream_pending_stream_count"] =
      static_cast<uint64_t>(g_live_retained_calibration_metrics.observe_stream_pending_stream_count);
  d["live_retained_calibration_observe_capture_pending_capture_count"] =
      static_cast<uint64_t>(g_live_retained_calibration_metrics.observe_capture_pending_capture_count);
  d["live_retained_calibration_process_pending_stream_count"] =
      static_cast<uint64_t>(g_live_retained_calibration_metrics.process_pending_stream_count);
  d["live_retained_calibration_process_pending_capture_count"] =
      static_cast<uint64_t>(g_live_retained_calibration_metrics.process_pending_capture_count);
  return d;
}

static const char* mode_to_cstr(RuntimeMode m) noexcept {
  switch (m) {
    case RuntimeMode::platform_backed: return "platform_backed";
    case RuntimeMode::synthetic: return "synthetic";
    default: return "unknown";
  }
}


static godot::String core_production_posture_name(CoreProductionPostureShape posture) {
  switch (posture) {
    case CoreProductionPostureShape::CpuPrimary:
      return "CPU-primary";
    case CoreProductionPostureShape::GpuPrimaryNoCpuSidecar:
      return "GPU-primary, no CPU sidecar";
    case CoreProductionPostureShape::GpuPrimaryWithCpuSidecar:
      return "GPU-primary, with CPU sidecar";
  }
  return "unknown";
}

static godot::String core_retained_production_plan_name(CoreRetainedProductionPlan plan) {
  if (!plan.valid) {
    return "";
  }
  return core_production_posture_name(plan.posture);
}

static godot::Dictionary core_retained_plan_to_dictionary(CoreRetainedProductionPlan plan) {
  godot::Dictionary d;
  d["valid"] = plan.valid;
  d["posture"] = core_retained_production_plan_name(plan);
  return d;
}

static godot::String backing_plan_evaluation_parent_kind_name(
    CoreBackingPlanEvaluationReport::ParentKind parent_kind) {
  switch (parent_kind) {
    case CoreBackingPlanEvaluationReport::ParentKind::Stream:
      return "stream";
    case CoreBackingPlanEvaluationReport::ParentKind::AcquisitionSession:
      return "acquisition_session";
    case CoreBackingPlanEvaluationReport::ParentKind::CapturePriming:
      return "capture_priming";
  }
  return "unknown";
}

static godot::String backing_plan_evaluation_primary_function_name(
    BackingPlanEvaluationPrimaryFunction primary_function) {
  switch (primary_function) {
    case BackingPlanEvaluationPrimaryFunction::StreamDisplayView:
      return "display_view";
    case BackingPlanEvaluationPrimaryFunction::CaptureReadyAndMaterialize:
      return "capture_ready_and_materialize";
  }
  return "unknown";
}

static godot::String backing_plan_evaluation_completion_reason_name(
    BackingPlanEvaluationCompletionReason reason) {
  switch (reason) {
    case BackingPlanEvaluationCompletionReason::None:
      return "none";
    case BackingPlanEvaluationCompletionReason::AllViableCandidatesEvaluated:
      return "all_viable_candidates_evaluated";
    case BackingPlanEvaluationCompletionReason::LiveDisplayDemandFamilyCrossing:
      return "live_display_demand_family_crossing";
    case BackingPlanEvaluationCompletionReason::SingleViableCandidate:
      return "single_viable_candidate";
  }
  return "unknown";
}

static godot::String result_capability_name(ResultCapability capability) {
  switch (capability) {
    case ResultCapability::UNSUPPORTED:
      return "unsupported";
    case ResultCapability::READY:
      return "ready";
    case ResultCapability::CHEAP:
      return "cheap";
    case ResultCapability::EXPENSIVE:
      return "expensive";
  }
  return "unknown";
}

static godot::String result_payload_kind_name(ResultPayloadKind payload_kind) {
  switch (payload_kind) {
    case ResultPayloadKind::CPU_PACKED:
      return "cpu_packed";
    case ResultPayloadKind::GPU_SURFACE:
      return "gpu_surface";
    case ResultPayloadKind::ENCODED_IMAGE:
      return "encoded_image";
    case ResultPayloadKind::RAW_IMAGE:
      return "raw_image";
  }
  return "unknown";
}

static godot::String capture_evidence_incomplete_reason_name(
    CaptureEvidenceIncompleteReason reason) {
  switch (reason) {
    case CaptureEvidenceIncompleteReason::None:
      return "none";
    case CaptureEvidenceIncompleteReason::NoRequiredBundle:
      return "no_required_bundle";
    case CaptureEvidenceIncompleteReason::AwaitingCaptureReady:
      return "awaiting_capture_ready";
    case CaptureEvidenceIncompleteReason::AwaitingRequiredMemberMaterialization:
      return "awaiting_required_member_materialization";
    case CaptureEvidenceIncompleteReason::RequiredMemberUnsupported:
      return "required_member_unsupported";
  }
  return "unknown";
}

static godot::Dictionary backing_plan_candidate_evidence_to_dictionary(
    const CoreBackingPlanCandidateEvidenceReport& evidence) {
  godot::Dictionary d;
  d["candidate"] = core_retained_plan_to_dictionary(evidence.candidate);
  d["observation_seen"] = evidence.observation_seen;
  d["evidence_complete"] = evidence.evidence_complete;
  d["evidence_accepted"] = evidence.evidence_accepted;
  d["provisional_display_view"] =
      result_capability_name(evidence.provisional_display_view);
  d["has_display_view_elapsed_ns"] = evidence.has_display_view_elapsed_ns;
  d["display_view_elapsed_ns"] =
      static_cast<uint64_t>(evidence.display_view_elapsed_ns);
  d["provisional_to_image"] =
      result_capability_name(evidence.provisional_to_image);
  d["has_materialization_elapsed_ns"] =
      evidence.has_materialization_elapsed_ns;
  d["materialization_elapsed_ns"] =
      static_cast<uint64_t>(evidence.materialization_elapsed_ns);
  d["has_capture_ready_elapsed_ns"] =
      evidence.has_capture_ready_elapsed_ns;
  d["capture_ready_elapsed_ns"] =
      static_cast<uint64_t>(evidence.capture_ready_elapsed_ns);
  d["has_total_elapsed_ns"] = evidence.has_total_elapsed_ns;
  d["total_elapsed_ns"] = static_cast<uint64_t>(evidence.total_elapsed_ns);
  d["required_capture_member_count"] =
      static_cast<uint64_t>(evidence.required_capture_member_count);
  d["observed_capture_member_count"] =
      static_cast<uint64_t>(evidence.observed_capture_member_count);
  d["materialized_capture_member_count"] =
      static_cast<uint64_t>(evidence.materialized_capture_member_count);
  d["has_first_missing_required_capture_member_index"] =
      evidence.has_first_missing_required_capture_member_index;
  d["first_missing_required_capture_member_index"] =
      static_cast<uint64_t>(evidence.first_missing_required_capture_member_index);
  d["capture_evidence_incomplete_reason"] =
      capture_evidence_incomplete_reason_name(
          evidence.capture_evidence_incomplete_reason);
  d["has_normalized_cost_units"] = evidence.has_normalized_cost_units;
  d["normalized_cost_units"] =
      static_cast<uint64_t>(evidence.normalized_cost_units);
  d["has_observed_posture"] = evidence.has_observed_posture;
  d["observed_posture"] = core_production_posture_name(evidence.observed_posture);
  d["observed_access_posture_id"] =
      static_cast<uint64_t>(evidence.observed_access_posture_id);
  d["observed_stream_id"] = static_cast<uint64_t>(evidence.observed_stream_id);
  d["observed_capture_id"] = static_cast<uint64_t>(evidence.observed_capture_id);
  d["observed_acquisition_session_id"] =
      static_cast<uint64_t>(evidence.observed_acquisition_session_id);
  d["observed_image_member_index"] =
      static_cast<int>(evidence.observed_image_member_index);
  d["observed_payload_kind"] =
      result_payload_kind_name(evidence.observed_payload_kind);
  d["observed_has_retained_cpu_payload"] =
      evidence.observed_has_retained_cpu_payload;
  d["observed_has_retained_gpu_backing"] =
      evidence.observed_has_retained_gpu_backing;
  d["observed_gpu_materialization_available"] =
      evidence.observed_gpu_materialization_available;
  d["observed_gpu_materialization_requires_readback"] =
      evidence.observed_gpu_materialization_requires_readback;
  return d;
}

static godot::Dictionary backing_plan_evaluation_report_to_dictionary(
    const CoreBackingPlanEvaluationReport& report) {
  godot::Dictionary d;
  d["parent_kind"] = backing_plan_evaluation_parent_kind_name(report.parent_kind);
  d["primary_function"] =
      backing_plan_evaluation_primary_function_name(report.primary_function);
  d["parent_id"] = static_cast<uint64_t>(report.parent_id);
  d["stream_id"] = static_cast<uint64_t>(report.stream_id);
  d["acquisition_session_id"] = static_cast<uint64_t>(report.acquisition_session_id);
  d["device_instance_id"] = static_cast<uint64_t>(report.device_instance_id);
  d["provisional_parent"] = report.provisional_parent;
  d["target_kind"] = report.target_kind == CoreBackingPlanEvaluationReport::TargetKind::Stream
      ? godot::String("stream")
      : godot::String("capture");
  d["target_id"] = static_cast<uint64_t>(report.target_id);
  d["requested"] = core_retained_plan_to_dictionary(report.requested);
  d["steady"] = core_retained_plan_to_dictionary(report.steady);
  d["evaluator_active"] = report.evaluator_active;
  d["current_candidate_index"] = static_cast<int>(report.current_candidate_index);
  d["completion_reason"] =
      backing_plan_evaluation_completion_reason_name(report.completion_reason);
  godot::Array candidates;
  for (const CoreRetainedProductionPlan& candidate : report.candidate_sequence) {
    candidates.append(core_retained_plan_to_dictionary(candidate));
  }
  d["candidate_sequence"] = candidates;
  godot::Array candidate_evidence;
  for (const CoreBackingPlanCandidateEvidenceReport& evidence :
       report.candidate_evidence) {
    candidate_evidence.append(
        backing_plan_candidate_evidence_to_dictionary(evidence));
  }
  d["candidate_evidence"] = candidate_evidence;
  d["decision_from_evaluation"] = report.decision_from_evaluation;
  d["decision_selected"] = core_retained_plan_to_dictionary(report.decision_selected);
  godot::Array decision_candidates;
  for (const CoreRetainedProductionPlan& candidate : report.decision_candidate_sequence) {
    decision_candidates.append(core_retained_plan_to_dictionary(candidate));
  }
  d["decision_candidate_sequence"] = decision_candidates;
  return d;
}

static bool parse_synthetic_role_int(int value, SyntheticRole& out_role) noexcept {
  switch (value) {
    case static_cast<int>(SyntheticRole::Nominal):
      out_role = SyntheticRole::Nominal;
      return true;
    case static_cast<int>(SyntheticRole::Timeline):
      out_role = SyntheticRole::Timeline;
      return true;
    default:
      return false;
  }
}

static bool parse_timing_driver_int(int value, TimingDriver& out_timing_driver) noexcept {
  switch (value) {
    case static_cast<int>(TimingDriver::RealTime):
      out_timing_driver = TimingDriver::RealTime;
      return true;
    case static_cast<int>(TimingDriver::VirtualTime):
      out_timing_driver = TimingDriver::VirtualTime;
      return true;
    default:
      return false;
  }
}

static bool parse_timeline_reconciliation_int(int value, TimelineReconciliation& out_reconciliation) noexcept {
  switch (value) {
    case CamBANGServer::TIMELINE_RECONCILIATION_COMPLETION_GATED:
      out_reconciliation = TimelineReconciliation::CompletionGated;
      return true;
    case CamBANGServer::TIMELINE_RECONCILIATION_STRICT:
      out_reconciliation = TimelineReconciliation::Strict;
      return true;
    default:
      return false;
  }
}

static bool read_synthetic_producer_output_form_project_setting(
    SyntheticProducerOutputFormMode& out_mode) {
  out_mode = SyntheticProducerOutputFormMode::Auto;
  godot::ProjectSettings* settings = godot::ProjectSettings::get_singleton();
  if (!settings) {
    return true;
  }
  godot::Variant value = settings->get_setting(
      kSyntheticProducerOutputFormProjectSetting,
      synthetic_producer_output_form_mode_setting_value(out_mode));
  const godot::String mode_string = value;
  const std::string mode(mode_string.utf8().get_data());
  return parse_synthetic_producer_output_form_mode(mode, out_mode);
}

static bool try_read_single_namespaced_cmdline_override(
    const std::string& prefix,
    std::string& out_value,
    bool& out_found) {
  out_value.clear();
  out_found = false;

  godot::OS* os = godot::OS::get_singleton();
  if (!os) {
    return true;
  }

  const auto scan = [&](const godot::PackedStringArray& args) -> bool {
    for (int64_t i = 0; i < args.size(); ++i) {
      const std::string arg(args[i].utf8().get_data());
      if (arg.rfind(prefix, 0) != 0) {
        continue;
      }
      if (out_found) {
        return false;
      }
      out_value = arg.substr(prefix.size());
      out_found = true;
    }
    return true;
  };

  if (!scan(os->get_cmdline_user_args())) {
    return false;
  }
  if (!out_found && !scan(os->get_cmdline_args())) {
    return false;
  }
  return true;
}

static bool apply_synthetic_producer_output_form_cmdline_to_project_setting() {
  godot::ProjectSettings* settings = godot::ProjectSettings::get_singleton();
  if (!settings) {
    return true;
  }

  const std::string prefix(kSyntheticProducerOutputFormArg);
  std::string mode;
  bool found = false;
  if (!try_read_single_namespaced_cmdline_override(prefix, mode, found)) {
    return false;
  }
  if (found) {
    SyntheticProducerOutputFormMode parsed = SyntheticProducerOutputFormMode::Auto;
    if (!parse_synthetic_producer_output_form_mode(mode, parsed)) {
      return false;
    }
    settings->set_setting(
        kSyntheticProducerOutputFormProjectSetting,
        godot::String(mode.c_str()));
  }
  return true;
}

static bool read_synthetic_stream_capability_downgrade_project_setting(
    std::vector<SyntheticStreamCapabilityDowngradeCondition>& out_conditions) {
  out_conditions.clear();
  godot::ProjectSettings* settings = godot::ProjectSettings::get_singleton();
  if (!settings) {
    return true;
  }
  const godot::Variant value = settings->get_setting(
      kSyntheticStreamCapabilityDowngradeProjectSetting,
      godot::String(""));
  const godot::String condition_string = value;
  const std::string conditions(condition_string.utf8().get_data());
  return parse_synthetic_stream_capability_downgrade_conditions(
      conditions, out_conditions);
}

static bool apply_synthetic_stream_capability_downgrade_cmdline_to_project_setting() {
  godot::ProjectSettings* settings = godot::ProjectSettings::get_singleton();
  if (!settings) {
    return true;
  }

  const std::string prefix(kSyntheticStreamCapabilityDowngradeArg);
  std::string value;
  bool found = false;
  if (!try_read_single_namespaced_cmdline_override(prefix, value, found)) {
    return false;
  }
  if (found) {
    std::vector<SyntheticStreamCapabilityDowngradeCondition> parsed{};
    if (!parse_synthetic_stream_capability_downgrade_conditions(
            value, parsed)) {
      return false;
    }
    settings->set_setting(
        kSyntheticStreamCapabilityDowngradeProjectSetting,
        godot::String(value.c_str()));
  }
  return true;
}

static bool read_synthetic_capture_capability_downgrade_project_setting(
    std::vector<SyntheticCaptureCapabilityDowngradeCondition>& out_conditions) {
  out_conditions.clear();
  godot::ProjectSettings* settings = godot::ProjectSettings::get_singleton();
  if (!settings) {
    return true;
  }
  const godot::Variant value = settings->get_setting(
      kSyntheticCaptureCapabilityDowngradeProjectSetting,
      godot::String(""));
  const godot::String condition_string = value;
  const std::string conditions(condition_string.utf8().get_data());
  return parse_synthetic_capture_capability_downgrade_conditions(
      conditions, out_conditions);
}

static bool apply_synthetic_capture_capability_downgrade_cmdline_to_project_setting() {
  godot::ProjectSettings* settings = godot::ProjectSettings::get_singleton();
  if (!settings) {
    return true;
  }

  const std::string prefix(kSyntheticCaptureCapabilityDowngradeArg);
  std::string value;
  bool found = false;
  if (!try_read_single_namespaced_cmdline_override(prefix, value, found)) {
    return false;
  }
  if (found) {
    std::vector<SyntheticCaptureCapabilityDowngradeCondition> parsed{};
    if (!parse_synthetic_capture_capability_downgrade_conditions(
            value, parsed)) {
      return false;
    }
    settings->set_setting(
        kSyntheticCaptureCapabilityDowngradeProjectSetting,
        godot::String(value.c_str()));
  }
  return true;
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

static godot::Error map_provider_access_status_to_godot_error(ProviderAccessStatus access) noexcept {
  switch (access.code) {
    case ProviderAccessCode::Ready: return godot::OK;
    case ProviderAccessCode::PermissionRequired:
    case ProviderAccessCode::PermissionDenied:
      return godot::ERR_UNAUTHORIZED;
    case ProviderAccessCode::AccessUnavailable:
      return godot::ERR_UNAVAILABLE;
    case ProviderAccessCode::CheckFailed:
      return godot::FAILED;
    default:
      return godot::FAILED;
  }
}

static const char* godot_error_to_cstr(godot::Error err) noexcept {
  switch (err) {
    case godot::OK: return "OK";
    case godot::ERR_BUSY: return "ERR_BUSY";
    case godot::ERR_UNAVAILABLE: return "ERR_UNAVAILABLE";
    case godot::ERR_INVALID_PARAMETER: return "ERR_INVALID_PARAMETER";
    case godot::ERR_ALREADY_IN_USE: return "ERR_ALREADY_IN_USE";
    case godot::ERR_CANT_OPEN: return "ERR_CANT_OPEN";
    case godot::FAILED: return "FAILED";
    default: return "ERR_UNKNOWN";
  }
}

static godot::Error map_try_set_still_capture_profile_status(TrySetStillCaptureProfileStatus s) noexcept {
  switch (s) {
    case TrySetStillCaptureProfileStatus::OK: return godot::OK;
    case TrySetStillCaptureProfileStatus::NotSupported: return godot::ERR_UNAVAILABLE;
    case TrySetStillCaptureProfileStatus::Busy: return godot::ERR_BUSY;
    case TrySetStillCaptureProfileStatus::InvalidArgument: return godot::ERR_INVALID_PARAMETER;
    default: return godot::FAILED;
  }
}

static godot::Error map_try_set_warm_hold_status(TrySetWarmHoldStatus s) noexcept {
  switch (s) {
    case TrySetWarmHoldStatus::OK: return godot::OK;
    case TrySetWarmHoldStatus::Busy: return godot::ERR_BUSY;
    case TrySetWarmHoldStatus::InvalidArgument: return godot::ERR_INVALID_PARAMETER;
    default: return godot::FAILED;
  }
}

static godot::Error map_try_trigger_device_capture_status(
    TryTriggerDeviceCaptureStatus s) noexcept {
  switch (s) {
    case TryTriggerDeviceCaptureStatus::OK: return godot::OK;
    case TryTriggerDeviceCaptureStatus::Busy: return godot::ERR_BUSY;
    case TryTriggerDeviceCaptureStatus::InvalidArgument:
      return godot::ERR_INVALID_PARAMETER;
    case TryTriggerDeviceCaptureStatus::ProviderRejected:
    case TryTriggerDeviceCaptureStatus::Unavailable:
      return godot::ERR_UNAVAILABLE;
    default:
      return godot::FAILED;
  }
}

static godot::Error map_try_close_device_status(TryCloseDeviceStatus s) noexcept {
  switch (s) {
    case TryCloseDeviceStatus::OK: return godot::OK;
    case TryCloseDeviceStatus::Busy: return godot::ERR_BUSY;
    case TryCloseDeviceStatus::InvalidArgument: return godot::ERR_INVALID_PARAMETER;
    case TryCloseDeviceStatus::ProviderRejected: return godot::FAILED;
    default: return godot::FAILED;
  }
}

static godot::Error map_try_destroy_stream_status(TryDestroyStreamStatus s) noexcept {
  switch (s) {
    case TryDestroyStreamStatus::OK: return godot::OK;
    case TryDestroyStreamStatus::Busy: return godot::ERR_BUSY;
    case TryDestroyStreamStatus::InvalidArgument: return godot::ERR_INVALID_PARAMETER;
    case TryDestroyStreamStatus::Started: return godot::ERR_UNAVAILABLE;
    case TryDestroyStreamStatus::ProviderRejected: return godot::ERR_UNAVAILABLE;
    default: return godot::FAILED;
  }
}

static uint64_t mix_identity_u64(uint64_t seed, uint64_t value) noexcept {
  seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
  return seed;
}

static uint64_t build_capture_member_identity_signature(
    const SharedCaptureResultData& data) noexcept {
  if (!data) {
    return 0;
  }
  uint64_t signature = 0;
  signature = mix_identity_u64(signature, data->capture_id);
  signature = mix_identity_u64(signature, data->acquisition_session_id);
  signature = mix_identity_u64(signature, data->image_member_count());
  for (uint32_t i = 0; i < data->image_member_count(); ++i) {
    const auto* member = data->image_member_at(i);
    if (!member) {
      continue;
    }
    signature = mix_identity_u64(signature, member->image_member_index);
    signature = mix_identity_u64(signature, member->capture_timestamp_ns);
    signature = mix_identity_u64(signature, member->access_posture.posture_id);
    signature = mix_identity_u64(
        signature,
        static_cast<uint64_t>(member->retained_access_truth.to_image));
  }
  return signature;
}

static bool infer_stream_result_posture_shape_for_calibration(
    const SharedStreamResultData& result,
    CoreProductionPostureShape& out) noexcept {
  if (!result) {
    return false;
  }
  if (result->payload_kind == ResultPayloadKind::GPU_SURFACE &&
      result->retained_gpu_backing) {
    out = result->access_posture.has_retained_cpu_payload
              ? CoreProductionPostureShape::GpuPrimaryWithCpuSidecar
              : CoreProductionPostureShape::GpuPrimaryNoCpuSidecar;
    return true;
  }
  if (result->payload_kind == ResultPayloadKind::CPU_PACKED) {
    out = CoreProductionPostureShape::CpuPrimary;
    return true;
  }
  return false;
}

static bool infer_capture_result_posture_shape_for_calibration(
    const SharedCaptureResultData& result,
    CoreProductionPostureShape& out) noexcept {
  if (!result) {
    return false;
  }
  const auto* member = result->image_member_at(0u);
  if (!member) {
    return false;
  }
  if (member->payload_kind == ResultPayloadKind::GPU_SURFACE &&
      member->retained_gpu_backing) {
    out = member->access_posture.has_retained_cpu_payload
              ? CoreProductionPostureShape::GpuPrimaryWithCpuSidecar
              : CoreProductionPostureShape::GpuPrimaryNoCpuSidecar;
    return true;
  }
  if (member->payload_kind == ResultPayloadKind::CPU_PACKED) {
    out = CoreProductionPostureShape::CpuPrimary;
    return true;
  }
  return false;
}

static uint64_t build_stream_evaluator_identity_signature(
    const CoreBackingPlanEvaluationReport& report) noexcept {
  uint64_t signature = 0;
  signature = mix_identity_u64(signature, report.stream_id);
  signature = mix_identity_u64(signature, report.current_candidate_index);
  signature = mix_identity_u64(
      signature,
      static_cast<uint64_t>(report.requested.valid
                                ? static_cast<uint8_t>(report.requested.posture)
                                : 0));
  return signature;
}

static uint64_t build_capture_evaluator_identity_signature(
    const CoreBackingPlanEvaluationReport& report) noexcept {
  uint64_t signature = 0;
  signature = mix_identity_u64(signature, report.device_instance_id);
  signature = mix_identity_u64(signature, report.acquisition_session_id);
  signature = mix_identity_u64(signature, report.current_candidate_index);
  signature = mix_identity_u64(
      signature,
      static_cast<uint64_t>(report.requested.valid
                                ? static_cast<uint8_t>(report.requested.posture)
                                : 0));
  return signature;
}

static const char* try_destroy_stream_status_name(TryDestroyStreamStatus s) noexcept {
  switch (s) {
    case TryDestroyStreamStatus::OK: return "OK";
    case TryDestroyStreamStatus::Busy: return "Busy";
    case TryDestroyStreamStatus::InvalidArgument: return "InvalidArgument";
    case TryDestroyStreamStatus::Started: return "Started";
    case TryDestroyStreamStatus::ProviderRejected: return "ProviderRejected";
  }
  return "Unknown";
}

static godot::Error map_try_start_stream_status(TryStartStreamStatus s) noexcept {
  switch (s) {
    case TryStartStreamStatus::OK: return godot::OK;
    case TryStartStreamStatus::Busy: return godot::ERR_BUSY;
    case TryStartStreamStatus::InvalidArgument: return godot::ERR_INVALID_PARAMETER;
    case TryStartStreamStatus::ProviderRejected: return godot::ERR_CANT_OPEN;
    default: return godot::FAILED;
  }
}

static godot::Error map_try_stop_stream_status(TryStopStreamStatus s) noexcept {
  switch (s) {
    case TryStopStreamStatus::OK: return godot::OK;
    case TryStopStreamStatus::Busy: return godot::ERR_BUSY;
    case TryStopStreamStatus::InvalidArgument: return godot::ERR_INVALID_PARAMETER;
    case TryStopStreamStatus::ProviderRejected: return godot::ERR_UNAVAILABLE;
    default: return godot::FAILED;
  }
}

static bool line_contains_token(const std::string& line, const char* token) {
  return token && line.find(token) != std::string::npos;
}

static int runtime_mode_to_provider_kind_int(RuntimeMode mode) noexcept {
  switch (mode) {
    case RuntimeMode::platform_backed: return CamBANGServer::PROVIDER_KIND_PLATFORM_BACKED;
    case RuntimeMode::synthetic: return CamBANGServer::PROVIDER_KIND_SYNTHETIC;
    default: return CamBANGServer::PROVIDER_KIND_PLATFORM_BACKED;
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
  timeline_trace_echo_enabled_ = timeline_teardown_trace_enabled();
  _refresh_timeline_teardown_trace_mode();
}

CamBANGServer::~CamBANGServer() {
  _disconnect_tick_if_connected_();

  // Ensure graceful stop if the extension is torn down.
  runtime_.stop();
  result_access_cost_evidence::clear();
  _clear_live_retained_result_access_calibration_state_();
  if (singleton_ == this) {
    singleton_ = nullptr;
  }
}

godot::Error CamBANGServer::start(const godot::Variant& provider_kind_arg,
                                  const godot::Variant& role_arg,
                                  const godot::Variant& timing_driver_arg,
                                  const godot::Variant& timeline_reconciliation_arg) {
  const bool has_provider_kind = provider_kind_arg.get_type() != godot::Variant::NIL;
  const bool has_role = role_arg.get_type() != godot::Variant::NIL;
  const bool has_timing_driver = timing_driver_arg.get_type() != godot::Variant::NIL;
  const bool has_timeline_reconciliation = timeline_reconciliation_arg.get_type() != godot::Variant::NIL;

  int provider_kind = PROVIDER_KIND_PLATFORM_BACKED;
  if (has_provider_kind) {
    if (provider_kind_arg.get_type() != godot::Variant::INT) {
      ERR_PRINT("CamBANGServer: start rejected; provider_kind must be an integer when supplied.");
      return godot::ERR_INVALID_PARAMETER;
    }
    provider_kind = static_cast<int>(int64_t(provider_kind_arg));
  }

  if (provider_kind == PROVIDER_KIND_PLATFORM_BACKED) {
    if (has_role || has_timing_driver || has_timeline_reconciliation) {
      ERR_PRINT("CamBANGServer: start rejected; platform-backed start does not accept synthetic role/timing/reconciliation arguments.");
      return godot::ERR_INVALID_PARAMETER;
    }
    return _start_with_provider_config(
        RuntimeMode::platform_backed,
        SyntheticRole::Nominal,
        TimingDriver::VirtualTime,
        true);
  }
  if (provider_kind != PROVIDER_KIND_SYNTHETIC) {
    ERR_PRINT(godot::vformat(
        "CamBANGServer: start rejected; unknown provider_kind value '%d'.",
        provider_kind));
    return godot::ERR_INVALID_PARAMETER;
  }

  int role = SYNTHETIC_ROLE_NOMINAL;
  if (has_role) {
    if (role_arg.get_type() != godot::Variant::INT) {
      ERR_PRINT("CamBANGServer: start rejected; synthetic role must be an integer when supplied.");
      return godot::ERR_INVALID_PARAMETER;
    }
    role = static_cast<int>(int64_t(role_arg));
  }

  SyntheticRole parsed_role{};
  if (!parse_synthetic_role_int(role, parsed_role)) {
    ERR_PRINT(godot::vformat(
        "CamBANGServer: start rejected; unknown synthetic role value '%d'.",
        role));
    return godot::ERR_INVALID_PARAMETER;
  }

  int timing_driver = TIMING_DRIVER_VIRTUAL_TIME;
  if (has_timing_driver) {
    if (timing_driver_arg.get_type() != godot::Variant::INT) {
      ERR_PRINT("CamBANGServer: start rejected; timing_driver must be an integer when supplied.");
      return godot::ERR_INVALID_PARAMETER;
    }
    timing_driver = static_cast<int>(int64_t(timing_driver_arg));
  }

  TimingDriver parsed_timing_driver{};
  if (!parse_timing_driver_int(timing_driver, parsed_timing_driver)) {
    ERR_PRINT(godot::vformat(
        "CamBANGServer: start rejected; unknown timing driver value '%d'.",
        timing_driver));
    return godot::ERR_INVALID_PARAMETER;
  }

  const bool reconciliation_applicable =
      parsed_role == SyntheticRole::Timeline && parsed_timing_driver == TimingDriver::VirtualTime;
  bool completion_gated_destructive_sequencing_enabled = true;
  TimelineReconciliation requested_timeline_reconciliation = TimelineReconciliation::CompletionGated;
  if (has_timeline_reconciliation) {
    if (!reconciliation_applicable) {
      ERR_PRINT("CamBANGServer: start rejected; timeline_reconciliation applies only to synthetic timeline virtual_time mode.");
      return godot::ERR_INVALID_PARAMETER;
    }
    if (timeline_reconciliation_arg.get_type() != godot::Variant::INT) {
      ERR_PRINT("CamBANGServer: start rejected; timeline_reconciliation must be an integer when supplied.");
      return godot::ERR_INVALID_PARAMETER;
    }
    const int timeline_reconciliation = static_cast<int>(int64_t(timeline_reconciliation_arg));
    if (!parse_timeline_reconciliation_int(timeline_reconciliation, requested_timeline_reconciliation)) {
      ERR_PRINT(godot::vformat(
          "CamBANGServer: start rejected; unknown timeline_reconciliation value '%d'.",
          timeline_reconciliation));
      return godot::ERR_INVALID_PARAMETER;
    }
  }

  completion_gated_destructive_sequencing_enabled =
      (requested_timeline_reconciliation == TimelineReconciliation::CompletionGated);

  return _start_with_provider_config(
      RuntimeMode::synthetic,
      parsed_role,
      parsed_timing_driver,
      completion_gated_destructive_sequencing_enabled);
}

godot::Dictionary CamBANGServer::get_provider_support() const {
  godot::Dictionary out;
  out["platform_backed"] = ProviderBroker::check_mode_supported_in_build(RuntimeMode::platform_backed).ok();
  out["synthetic"] = ProviderBroker::check_mode_supported_in_build(RuntimeMode::synthetic).ok();
  out["target_platform"] = godot::String(CAMBANG_GDE_TARGET_PLATFORM);
  out["platform_family"] = godot::String(CAMBANG_GDE_PLATFORM_PROVIDER_FAMILY);
  out["platform_provider_status"] = godot::String(CAMBANG_GDE_PLATFORM_PROVIDER_STATUS);
  return out;
}

godot::Error CamBANGServer::_start_with_provider_config(
    RuntimeMode mode,
    SyntheticRole synthetic_role,
    TimingDriver timing_driver,
    bool completion_gated_destructive_sequencing_enabled) {
  const CoreRuntimeState state = runtime_.state_copy();
  if (state == CoreRuntimeState::STARTING || state == CoreRuntimeState::LIVE) {
    return godot::ERR_ALREADY_IN_USE;
  }

  // New session starts with no Godot-latched snapshot or pending endpoint startup
  // intent until first publish.
  _clear_pending_endpoint_startup_intents_();
  latest_.reset();
  latest_export_.clear();
  has_latest_export_ = false;
  has_godot_counters_ = false;
  CamBANGStreamResult::clear_live_stream_cpu_display_views();
  result_access_cost_evidence::clear();
  _clear_live_retained_result_access_calibration_state_();

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

  active_runtime_mode_ = mode;
  active_synthetic_role_ = synthetic_role;
  completion_gated_destructive_sequencing_enabled_ = completion_gated_destructive_sequencing_enabled;
  strict_scenario_unmet_logged_ = false;
  _reset_scenario_session_state_();
  _clear_pending_endpoint_startup_intents_();
  _refresh_timeline_teardown_trace_mode();

  const auto clear_start_attempt_state = [this]() {
    _set_all_tracked_wrapper_live_states_false_();
    latest_.reset();
    latest_export_.clear();
    has_latest_export_ = false;
    has_godot_counters_ = false;
    snapshot_buffer_.clear();
    last_seen_published_seq_ = runtime_.published_seq();
    active_session_id_ = 0;
    enforce_min_gen_gate_ = false;
    endpoint_lifecycle_by_hardware_id_.clear();
    direct_stream_hardware_id_by_stream_id_.clear();
    latest_capture_id_by_device_instance_id_.clear();
    CamBANGStreamResult::clear_live_stream_cpu_display_views();
    result_access_cost_evidence::clear();
    _clear_live_retained_result_access_calibration_state_();

    strict_scenario_unmet_logged_ = false;
    _reset_scenario_session_state_();
    _clear_pending_endpoint_startup_intents_();
    _refresh_timeline_teardown_trace_mode();
  };

  // Defensive re-check: requested mode must be supported in this build.
  {
    ProviderResult cap = ProviderBroker::check_mode_supported_in_build(mode);
    if (!cap.ok()) {
      ERR_PRINT(godot::vformat(
          "CamBANGServer: cannot start; requested provider_mode='%s' is not supported in this build.",
          mode_to_cstr(mode)));
      clear_start_attempt_state();
      return map_provider_result_to_godot_error(cap);
    }
  }

  // Provider access/readiness is distinct from build support.  This preflight
  // must run before CoreRuntime::start() so a permission/access failure cannot
  // create a runtime generation, publish a baseline, or attach a provider.
  {
    const ProviderAccessStatus access = ProviderBroker::check_mode_access_readiness(mode);
    if (!access.ok()) {
      ERR_PRINT(godot::vformat(
          "CamBANGServer: cannot start; provider access/readiness preflight failed for provider_mode='%s' code='%s' reason='%s'.",
          mode_to_cstr(mode),
          cambang::to_string(access.code),
          access.stable_reason ? access.stable_reason : ""));
      clear_start_attempt_state();
      return map_provider_access_status_to_godot_error(access);
    }
  }

  // Ensure the SceneTree tick hook exists so snapshots can be drained + signals emitted.
  _ensure_tick_connected();

  // Explicit user action: do not auto-start on launch.
  if (!runtime_.start()) {
    clear_start_attempt_state();
    return godot::FAILED;
  }
  _refresh_timeline_teardown_trace_mode();

  // Ensure a provider is attached + initialized (latched selection).
  // This is the canonical linkage point between Godot and the core runtime.
  if (!_ensure_provider_attached_and_initialized(mode, synthetic_role, timing_driver)) {
    runtime_.stop();
    runtime_.attach_provider(nullptr);
    provider_.reset();
    clear_start_attempt_state();
    return godot::FAILED;
  }
  return godot::OK;
}

void CamBANGServer::stop() {
  synthetic_gpu_backing_warn_and_abandon_live_display_wrappers_before_stop();

  // CoreRuntime owns attached-provider shutdown while the core thread is live.
  runtime_.stop();
  runtime_.attach_provider(nullptr);
  provider_.reset();
  CamBANGStreamResult::clear_live_stream_cpu_display_views();
  result_access_cost_evidence::clear();
  _clear_live_retained_result_access_calibration_state_();

  strict_scenario_unmet_logged_ = false;
  _reset_scenario_session_state_();
  _clear_pending_endpoint_startup_intents_();
  _refresh_timeline_teardown_trace_mode();

  // Stop is a boundary operation; core may have published final teardown/retirement
  // truth during deterministic shutdown. Drain one final boundary observation before
  // returning to NIL so the prior generation can be observed once more if changed.
  if (has_godot_counters_) {
    (void)_consume_latest_core_snapshot();
  }

  if (has_godot_counters_) {
    has_last_completed_gen_ = true;
    last_completed_gen_ = godot_gen_;
  }

  _set_all_tracked_wrapper_live_states_false_();

  // Enforce documented NIL pre-baseline behaviour across restart boundaries.
  latest_.reset();
  latest_export_.clear();
  has_latest_export_ = false;
  has_godot_counters_ = false;
  snapshot_buffer_.clear();
  last_seen_published_seq_ = runtime_.published_seq();
  active_session_id_ = 0;
  enforce_min_gen_gate_ = false;
  endpoint_lifecycle_by_hardware_id_.clear();
  direct_stream_hardware_id_by_stream_id_.clear();
  latest_capture_id_by_device_instance_id_.clear();
}

void CamBANGServer::stop_and_quit(int64_t exit_code) {
  // Keep stop() as the synchronous teardown boundary. The quit delay belongs to
  // this immediate-exit helper only, so normal stop() callers keep unchanged
  // semantics and do not wait for editor/debugger diagnostics to flush.
  stop();

  if (pending_stop_and_quit_) {
    return;
  }

  godot::Engine* engine = godot::Engine::get_singleton();
  godot::SceneTree* tree = nullptr;
  if (engine) {
    tree = godot::Object::cast_to<godot::SceneTree>(engine->get_main_loop());
  }
  if (!tree) {
    godot::UtilityFunctions::push_warning(
        "CamBANGServer.stop_and_quit() could not schedule quit because no SceneTree main loop is available.");
    return;
  }

  _ensure_tick_connected();
  if (!tick_connected_) {
    godot::UtilityFunctions::push_warning(
        "CamBANGServer.stop_and_quit() could not schedule quit because the SceneTree process_frame hook is unavailable.");
    return;
  }

  pending_stop_and_quit_ = true;
  pending_stop_and_quit_frames_remaining_ = kEditorDiagnosticQuitFlushFrames;
  pending_stop_and_quit_exit_code_ = static_cast<int>(exit_code);
}

bool CamBANGServer::is_running() const {
  const CoreRuntimeState state = runtime_.state_copy();
  return state == CoreRuntimeState::STARTING || state == CoreRuntimeState::LIVE;
}

bool CamBANGServer::is_public_boundary_ready_() const {
  if (runtime_.state_copy() != CoreRuntimeState::LIVE) {
    return false;
  }
  if (active_session_id_ == 0) {
    return false;
  }
  if (!has_godot_counters_ || !has_latest_export_ || !latest_) {
    return false;
  }
  return latest_->gen == godot_gen_;
}

bool CamBANGServer::is_provider_discovery_available_() const {
  if (active_session_id_ == 0) {
    return false;
  }
  if (!provider_) {
    return false;
  }

  const CoreRuntimeState state = runtime_.state_copy();
  return state == CoreRuntimeState::STARTING || state == CoreRuntimeState::LIVE;
}

bool CamBANGServer::is_synthetic_timeline_session_active_() const {
  return active_session_id_ != 0 &&
         runtime_.is_running() &&
         provider_ &&
         active_runtime_mode_ == RuntimeMode::synthetic &&
         active_synthetic_role_ == SyntheticRole::Timeline &&
         dynamic_cast<ProviderBroker*>(provider_.get()) != nullptr;
}

void CamBANGServer::_clear_pending_scenario_start_() {
  pending_scenario_start_after_baseline_ = false;
  pending_scenario_start_session_id_ = 0;
  pending_timeline_pause_after_scenario_start_ = false;
  pending_timeline_pause_value_ = false;
}

void CamBANGServer::_reset_scenario_session_state_() {
  scenario_config_staged_for_session_ = false;
  _clear_pending_scenario_start_();
}

bool CamBANGServer::_resolve_provider_endpoint_(const godot::String& hardware_id, godot::String* out_display_name) const {
  if (hardware_id.is_empty() || !is_provider_discovery_available_()) {
    return false;
  }
  std::vector<CameraEndpoint> endpoints;
  const ProviderResult pr = provider_->enumerate_endpoints(endpoints);
  if (!pr.ok()) {
    return false;
  }
  for (const CameraEndpoint& endpoint : endpoints) {
    if (hardware_id == endpoint.hardware_id.c_str()) {
      if (out_display_name) {
        *out_display_name = godot::String(endpoint.name.c_str());
      }
      return true;
    }
  }
  return false;
}

std::string CamBANGServer::_pending_endpoint_startup_key_(uint64_t session_id, const godot::String& hardware_id) const {
  return std::to_string(session_id) + "\n" + std::string(hardware_id.utf8().get_data());
}

godot::Error CamBANGServer::_record_pending_endpoint_startup_engage_(
    const godot::String& hardware_id,
    const godot::String& display_name) {
  if (hardware_id.is_empty() || active_session_id_ == 0) {
    return godot::ERR_UNAVAILABLE;
  }
  godot::String resolved_display_name;
  if (!_resolve_provider_endpoint_(hardware_id, &resolved_display_name)) {
    return godot::ERR_INVALID_PARAMETER;
  }
  if (resolved_display_name.is_empty()) {
    resolved_display_name = display_name;
  }

  PendingEndpointStartupIntent& intent =
      pending_endpoint_startup_intents_[_pending_endpoint_startup_key_(active_session_id_, hardware_id)];
  intent.session_id = active_session_id_;
  intent.hardware_id = hardware_id;
  intent.display_name = resolved_display_name;
  intent.engage_requested = true;

  EndpointLifecycleState& state = endpoint_lifecycle_by_hardware_id_[std::string(hardware_id.utf8().get_data())];
  state.hardware_id = hardware_id;
  state.display_name = resolved_display_name;
  return godot::OK;
}

void CamBANGServer::_clear_pending_endpoint_startup_intents_() {
  pending_endpoint_startup_intents_.clear();
}

godot::Array CamBANGServer::enumerate_devices() const {
  godot::Array out;
  if (!is_provider_discovery_available_()) {
    return out;
  }
  std::vector<CameraEndpoint> endpoints;
  const ProviderResult pr = provider_->enumerate_endpoints(endpoints);
  if (!pr.ok()) {
    return out;
  }
  for (const CameraEndpoint& endpoint : endpoints) {
    godot::Dictionary entry;
    entry["hardware_id"] = godot::String(endpoint.hardware_id.c_str());
    entry["name"] = godot::String(endpoint.name.c_str());
    out.push_back(entry);
  }
  return out;
}

godot::Ref<CamBANGDevice> CamBANGServer::get_device_for_hardware_id(const godot::String& hardware_id) const {
  if (hardware_id.is_empty() || !is_provider_discovery_available_()) {
    return godot::Ref<CamBANGDevice>();
  }
  std::vector<CameraEndpoint> endpoints;
  const ProviderResult pr = provider_->enumerate_endpoints(endpoints);
  if (!pr.ok()) {
    return godot::Ref<CamBANGDevice>();
  }
  for (const CameraEndpoint& endpoint : endpoints) {
    if (hardware_id == endpoint.hardware_id.c_str()) {
      godot::Ref<CamBANGDevice> out;
      out.instantiate();
      out->set_server_and_endpoint(
          const_cast<CamBANGServer*>(this),
          hardware_id,
          godot::String(endpoint.name.c_str()));
      auto& state = const_cast<CamBANGServer*>(this)->endpoint_lifecycle_by_hardware_id_[endpoint.hardware_id];
      state.hardware_id = hardware_id;
      state.display_name = godot::String(endpoint.name.c_str());
      return out;
    }
  }
  return godot::Ref<CamBANGDevice>();
}

godot::Error CamBANGServer::engage_endpoint_handle(const godot::String& hardware_id, const godot::String& display_name) {
  if (hardware_id.is_empty()) {
    return godot::ERR_UNAVAILABLE;
  }
  if (!is_public_boundary_ready_()) {
    if (is_provider_discovery_available_()) {
      return _record_pending_endpoint_startup_engage_(hardware_id, display_name);
    }
    return godot::ERR_UNAVAILABLE;
  }
  if (!provider_) {
    return godot::ERR_UNAVAILABLE;
  }

  godot::String resolved_display_name = display_name;
  if (!_resolve_provider_endpoint_(hardware_id, &resolved_display_name)) {
    return godot::ERR_INVALID_PARAMETER;
  }

  EndpointLifecycleState& state = endpoint_lifecycle_by_hardware_id_[std::string(hardware_id.utf8().get_data())];
  state.hardware_id = hardware_id;
  state.display_name = resolved_display_name;
  if (state.device_instance_id != 0 || state.open_requested) {
    return godot::OK;
  }

  state.device_instance_id = next_direct_device_instance_id_.fetch_add(1, std::memory_order_relaxed);
  state.root_id = next_direct_root_id_.fetch_add(1, std::memory_order_relaxed);
  const TryOpenDeviceStatus open_status =
      runtime_.try_open_device(std::string(hardware_id.utf8().get_data()), state.device_instance_id, state.root_id);
  switch (open_status) {
    case TryOpenDeviceStatus::OK:
      state.open_requested = true;
      state.close_requested = false;
      return godot::OK;
    case TryOpenDeviceStatus::Busy:
      state.device_instance_id = 0;
      state.root_id = 0;
      return godot::ERR_BUSY;
    case TryOpenDeviceStatus::ProviderRejected:
      state.device_instance_id = 0;
      state.root_id = 0;
      return godot::FAILED;
    case TryOpenDeviceStatus::InvalidArgument:
    default:
      state.device_instance_id = 0;
      state.root_id = 0;
      return godot::ERR_INVALID_PARAMETER;
  }
}

godot::Error CamBANGServer::disengage_endpoint_handle(const godot::String& hardware_id) {
  if (hardware_id.is_empty() || !is_public_boundary_ready_() || !provider_) {
    return godot::ERR_UNAVAILABLE;
  }

  const std::string hardware_id_key(hardware_id.utf8().get_data());
  auto state_it = endpoint_lifecycle_by_hardware_id_.find(hardware_id_key);
  if (state_it == endpoint_lifecycle_by_hardware_id_.end()) {
    std::vector<CameraEndpoint> endpoints;
    const ProviderResult pr = provider_->enumerate_endpoints(endpoints);
    if (!pr.ok()) {
      return godot::ERR_UNAVAILABLE;
    }
    bool found = false;
    for (const CameraEndpoint& endpoint : endpoints) {
      if (hardware_id == endpoint.hardware_id.c_str()) {
        EndpointLifecycleState state{};
        state.hardware_id = hardware_id;
        state.display_name = godot::String(endpoint.name.c_str());
        endpoint_lifecycle_by_hardware_id_[hardware_id_key] = state;
        found = true;
        break;
      }
    }
    if (!found) {
      return godot::ERR_INVALID_PARAMETER;
    }
    state_it = endpoint_lifecycle_by_hardware_id_.find(hardware_id_key);
  }

  EndpointLifecycleState& state = state_it->second;
  if (state.device_instance_id == 0 && !state.open_requested && !state.close_requested) {
    return godot::OK;
  }
  if (state.close_requested) {
    return godot::OK;
  }
  if (state.device_instance_id == 0) {
    return godot::ERR_INVALID_PARAMETER;
  }

  const godot::Error rc = map_try_close_device_status(runtime_.try_close_device(state.device_instance_id));
  if (rc == godot::OK) {
    state.close_requested = true;
  }
  return rc;
}

godot::Ref<CamBANGStream> CamBANGServer::create_stream_for_endpoint_hardware_id(const godot::String& hardware_id) {
  if (hardware_id.is_empty() || !is_public_boundary_ready_() || !provider_) {
    return godot::Ref<CamBANGStream>();
  }
  const auto state_it = endpoint_lifecycle_by_hardware_id_.find(std::string(hardware_id.utf8().get_data()));
  if (state_it == endpoint_lifecycle_by_hardware_id_.end()) {
    return godot::Ref<CamBANGStream>();
  }
  const EndpointLifecycleState& state = state_it->second;
  if (state.device_instance_id == 0) {
    return godot::Ref<CamBANGStream>();
  }

  uint64_t stream_id = next_direct_stream_id_.fetch_add(1, std::memory_order_relaxed);
  if (stream_id == 0) {
    stream_id = next_direct_stream_id_.fetch_add(1, std::memory_order_relaxed);
  }
  const TryCreateStreamStatus cs = runtime_.try_create_stream(
      stream_id,
      state.device_instance_id,
      StreamIntent::PREVIEW,
      nullptr,
      nullptr,
      0);
  if (cs != TryCreateStreamStatus::OK) {
    return godot::Ref<CamBANGStream>();
  }
  direct_stream_hardware_id_by_stream_id_[stream_id] = hardware_id;
  godot::Ref<CamBANGStream> out;
  out.instantiate();
  out->set_identity(const_cast<CamBANGServer*>(this), hardware_id, state.device_instance_id, stream_id);
  return out;
}

godot::Error CamBANGServer::destroy_direct_stream_handle(
    uint64_t stream_id,
    const godot::String& hardware_id,
    uint64_t device_instance_id) {
  if (stream_id == 0 || !is_public_boundary_ready_() || !provider_) {
    return godot::ERR_UNAVAILABLE;
  }
  const auto it = direct_stream_hardware_id_by_stream_id_.find(stream_id);
  if (it == direct_stream_hardware_id_by_stream_id_.end()) {
    return godot::OK;
  }
  if (!hardware_id.is_empty() && it->second != hardware_id) {
    return godot::ERR_INVALID_PARAMETER;
  }
  if (device_instance_id == 0) {
    return godot::ERR_INVALID_PARAMETER;
  }
  const TryDestroyStreamStatus status = runtime_.try_destroy_stream(stream_id);
  const godot::Error rc = map_try_destroy_stream_status(status);
  if (rc == godot::OK) {
    CamBANGStreamResult::remove_live_stream_cpu_display_view(stream_id);
    direct_stream_hardware_id_by_stream_id_.erase(it);
  } else if (status == TryDestroyStreamStatus::Started) {
    ERR_PRINT("CamBANGServer: destroy_stream rejected because stream is started; call stop() before destroy().");
  } else if (status == TryDestroyStreamStatus::ProviderRejected) {
    ERR_PRINT(godot::vformat(
        "CamBANGServer: destroy_stream rejected by provider for stream_id=%d; status=%s.",
        static_cast<int64_t>(stream_id),
        try_destroy_stream_status_name(status)));
  }
  return rc;
}

godot::Error CamBANGServer::start_direct_stream_handle(
    uint64_t stream_id,
    const godot::String& hardware_id,
    uint64_t device_instance_id) {
  if (stream_id == 0 || !is_public_boundary_ready_() || !provider_) {
    return godot::ERR_UNAVAILABLE;
  }
  const auto it = direct_stream_hardware_id_by_stream_id_.find(stream_id);
  if (it == direct_stream_hardware_id_by_stream_id_.end()) {
    return godot::ERR_INVALID_PARAMETER;
  }
  if (!hardware_id.is_empty() && it->second != hardware_id) {
    return godot::ERR_INVALID_PARAMETER;
  }
  if (device_instance_id == 0) {
    return godot::ERR_INVALID_PARAMETER;
  }
  return map_try_start_stream_status(runtime_.try_start_stream(stream_id));
}

godot::Error CamBANGServer::stop_direct_stream_handle(
    uint64_t stream_id,
    const godot::String& hardware_id,
    uint64_t device_instance_id) {
  if (stream_id == 0 || !is_public_boundary_ready_() || !provider_) {
    return godot::ERR_UNAVAILABLE;
  }
  const auto it = direct_stream_hardware_id_by_stream_id_.find(stream_id);
  if (it == direct_stream_hardware_id_by_stream_id_.end()) {
    return godot::ERR_INVALID_PARAMETER;
  }
  if (!hardware_id.is_empty() && it->second != hardware_id) {
    return godot::ERR_INVALID_PARAMETER;
  }
  if (device_instance_id == 0) {
    return godot::ERR_INVALID_PARAMETER;
  }
  return map_try_stop_stream_status(runtime_.try_stop_stream(stream_id));
}

uint64_t CamBANGServer::resolve_endpoint_instance_id(const godot::String& hardware_id) const {
  if (hardware_id.is_empty() || !is_public_boundary_ready_()) {
    return 0;
  }
  const auto it = endpoint_lifecycle_by_hardware_id_.find(std::string(hardware_id.utf8().get_data()));
  if (it == endpoint_lifecycle_by_hardware_id_.end()) {
    return 0;
  }
  return it->second.device_instance_id;
}

void CamBANGServer::register_tracked_device_wrapper_(uint64_t wrapper_object_id) {
  if (wrapper_object_id == 0) {
    return;
  }
  tracked_device_wrapper_object_ids_.insert(wrapper_object_id);
  godot::Object* object = godot::ObjectDB::get_instance(wrapper_object_id);
  CamBANGDevice* device = godot::Object::cast_to<CamBANGDevice>(object);
  if (!device) {
    tracked_device_wrapper_object_ids_.erase(wrapper_object_id);
    return;
  }
  device->_set_live_from_server_(
      _is_device_live_by_identity_(device->get_hardware_id(), device->get_instance_id()));
}

void CamBANGServer::register_tracked_stream_wrapper_(uint64_t wrapper_object_id) {
  if (wrapper_object_id == 0) {
    return;
  }
  tracked_stream_wrapper_object_ids_.insert(wrapper_object_id);
  godot::Object* object = godot::ObjectDB::get_instance(wrapper_object_id);
  CamBANGStream* stream = godot::Object::cast_to<CamBANGStream>(object);
  if (!stream) {
    tracked_stream_wrapper_object_ids_.erase(wrapper_object_id);
    return;
  }
  stream->_set_result_live_from_server_(
      _is_stream_result_live_by_identity_(stream->get_stream_id()));
}

bool CamBANGServer::_is_device_live_by_identity_(const godot::String& hardware_id,
                                                 uint64_t device_instance_id) const {
  if (!is_public_boundary_ready_() || !latest_) {
    return false;
  }
  for (const DeviceState& device : latest_->devices) {
    if (device.phase != CBLifecyclePhase::LIVE) {
      continue;
    }
    if (!hardware_id.is_empty() && hardware_id == device.hardware_id.c_str()) {
      return true;
    }
    if (device_instance_id != 0 && device.instance_id == device_instance_id) {
      return true;
    }
  }
  return false;
}

bool CamBANGServer::_is_stream_result_live_by_identity_(uint64_t stream_id) const {
  if (stream_id == 0 || !is_public_boundary_ready_() || !latest_) {
    return false;
  }
  for (const StreamState& stream : latest_->streams) {
    if (stream.stream_id != stream_id) {
      continue;
    }
    if (stream.phase != CBLifecyclePhase::LIVE ||
        stream.mode != CBStreamMode::FLOWING) {
      return false;
    }
    return runtime_.get_latest_stream_result(stream_id) != nullptr;
  }
  return false;
}

void CamBANGServer::_refresh_tracked_wrapper_live_states_from_snapshot_() {
  for (auto it = tracked_device_wrapper_object_ids_.begin();
       it != tracked_device_wrapper_object_ids_.end();) {
    godot::Object* object = godot::ObjectDB::get_instance(*it);
    CamBANGDevice* device = godot::Object::cast_to<CamBANGDevice>(object);
    if (!device) {
      it = tracked_device_wrapper_object_ids_.erase(it);
      continue;
    }
    device->_set_live_from_server_(
        _is_device_live_by_identity_(device->get_hardware_id(), device->get_instance_id()));
    ++it;
  }

  for (auto it = tracked_stream_wrapper_object_ids_.begin();
       it != tracked_stream_wrapper_object_ids_.end();) {
    godot::Object* object = godot::ObjectDB::get_instance(*it);
    CamBANGStream* stream = godot::Object::cast_to<CamBANGStream>(object);
    if (!stream) {
      it = tracked_stream_wrapper_object_ids_.erase(it);
      continue;
    }
    stream->_set_result_live_from_server_(
        _is_stream_result_live_by_identity_(stream->get_stream_id()));
    ++it;
  }
}

void CamBANGServer::_set_all_tracked_wrapper_live_states_false_() {
  for (auto it = tracked_device_wrapper_object_ids_.begin();
       it != tracked_device_wrapper_object_ids_.end();) {
    godot::Object* object = godot::ObjectDB::get_instance(*it);
    CamBANGDevice* device = godot::Object::cast_to<CamBANGDevice>(object);
    if (!device) {
      it = tracked_device_wrapper_object_ids_.erase(it);
      continue;
    }
    device->_set_live_from_server_(false);
    ++it;
  }

  for (auto it = tracked_stream_wrapper_object_ids_.begin();
       it != tracked_stream_wrapper_object_ids_.end();) {
    godot::Object* object = godot::ObjectDB::get_instance(*it);
    CamBANGStream* stream = godot::Object::cast_to<CamBANGStream>(object);
    if (!stream) {
      it = tracked_stream_wrapper_object_ids_.erase(it);
      continue;
    }
    stream->_set_result_live_from_server_(false);
    ++it;
  }
}

godot::Ref<CamBANGDevice> CamBANGServer::get_device(uint64_t device_instance_id) const {
  if (device_instance_id == 0 || !is_public_boundary_ready_()) {
    return godot::Ref<CamBANGDevice>();
  }
  godot::Ref<CamBANGDevice> out;
  out.instantiate();
  out->set_server_and_instance(const_cast<CamBANGServer*>(this), device_instance_id);
  return out;
}

uint64_t CamBANGServer::get_latest_capture_id_for_device(uint64_t device_instance_id) const {
  if (device_instance_id == 0 || !is_public_boundary_ready_()) {
    return 0;
  }
  const auto it = latest_capture_id_by_device_instance_id_.find(device_instance_id);
  if (it == latest_capture_id_by_device_instance_id_.end()) {
    return 0;
  }
  return it->second;
}

godot::Ref<CamBANGRig> CamBANGServer::get_rig(uint64_t rig_id) const {
  if (rig_id == 0 || !is_public_boundary_ready_()) {
    return godot::Ref<CamBANGRig>();
  }
  godot::Ref<CamBANGRig> out;
  out.instantiate();
  out->set_server_and_id(const_cast<CamBANGServer*>(this), rig_id);
  return out;
}

godot::Ref<CamBANGStreamResult> CamBANGServer::get_stream_result_by_stream_id(uint64_t stream_id) const {
  if (!is_public_boundary_ready_()) {
    return godot::Ref<CamBANGStreamResult>();
  }
  SharedStreamResultData data = runtime_.get_latest_stream_result(stream_id);
  if (!data) {
    return godot::Ref<CamBANGStreamResult>();
  }
  godot::Ref<CamBANGStreamResult> out;
  out.instantiate();
  out->set_data(std::move(data));
  return out;
}

godot::Ref<CamBANGCaptureResult> CamBANGServer::get_capture_result_by_id(uint64_t capture_id, uint64_t device_instance_id) const {
  if (!is_public_boundary_ready_()) {
    return godot::Ref<CamBANGCaptureResult>();
  }
  SharedCaptureResultData data = runtime_.get_capture_result(capture_id, device_instance_id);
  if (!data) {
    return godot::Ref<CamBANGCaptureResult>();
  }
  godot::Ref<CamBANGCaptureResult> out;
  out.instantiate();
  out->set_data(std::move(data));
  out->set_server(const_cast<CamBANGServer*>(this));
  return out;
}

godot::Ref<CamBANGCaptureResultSet> CamBANGServer::get_capture_result_set_by_id(uint64_t capture_id) const {
  if (!is_public_boundary_ready_()) {
    return godot::Ref<CamBANGCaptureResultSet>();
  }
  std::vector<SharedCaptureResultData> results = runtime_.get_capture_result_set(capture_id);
  godot::Ref<CamBANGCaptureResultSet> out;
  out.instantiate();
  out->set_capture_id(capture_id);
  out->set_server(const_cast<CamBANGServer*>(this));
  out->set_results(std::move(results));
  return out;
}

void CamBANGServer::report_capture_result_member_observation(
    const SharedCaptureResultData& data,
    uint32_t image_member_index) const {
  if (!is_public_boundary_ready_()) {
    return;
  }
  retained_result_access_calibration::report_capture_result_member_observation(
      data,
      image_member_index,
      const_cast<CoreRuntime*>(&runtime_));
}

void CamBANGServer::mark_stream_display_demand(uint64_t stream_id) {
  if (stream_id == 0 || !is_public_boundary_ready_()) {
    return;
  }
  runtime_.mark_stream_display_demand(stream_id);
}

void CamBANGServer::retain_stream_display_demand(uint64_t stream_id) {
  if (stream_id == 0 || !is_public_boundary_ready_()) {
    return;
  }
  runtime_.retain_stream_display_demand(stream_id);
}

void CamBANGServer::release_stream_display_demand(uint64_t stream_id) {
  if (stream_id == 0 || !is_public_boundary_ready_()) {
    return;
  }
  runtime_.release_stream_display_demand(stream_id);
}

void CamBANGServer::release_stream_display_demand_async(uint64_t stream_id) {
  if (stream_id == 0 || !is_public_boundary_ready_()) {
    return;
  }
  runtime_.release_stream_display_demand_async(stream_id);
}

godot::Error CamBANGServer::trigger_device_capture(
    uint64_t device_instance_id,
    uint64_t& out_capture_id) {
  out_capture_id = 0;
  if (device_instance_id == 0 || !is_public_boundary_ready_()) {
    return godot::ERR_BUSY;
  }

  uint64_t capture_id = next_capture_id_.fetch_add(1, std::memory_order_relaxed);
  if (capture_id == 0) {
    capture_id = next_capture_id_.fetch_add(1, std::memory_order_relaxed);
  }

  const TryTriggerDeviceCaptureStatus status =
      runtime_.try_trigger_device_capture_with_capture_id_for_server(device_instance_id, capture_id);
  if (status != TryTriggerDeviceCaptureStatus::OK) {
    return map_try_trigger_device_capture_status(status);
  }
  latest_capture_id_by_device_instance_id_[device_instance_id] = capture_id;
  out_capture_id = capture_id;
  return godot::OK;
}

godot::Error CamBANGServer::set_device_still_capture_profile(
    uint64_t device_instance_id,
    const CaptureProfile& profile,
    const CaptureStillImageBundle& still_image_bundle) {
  if (device_instance_id == 0 || !is_public_boundary_ready_() || !provider_) {
    return godot::ERR_BUSY;
  }
  return map_try_set_still_capture_profile_status(
      runtime_.try_set_device_still_capture_profile(device_instance_id, profile, still_image_bundle));
}

godot::Error CamBANGServer::set_endpoint_still_capture_profile_startup_intent(
    const godot::String& hardware_id,
    const CaptureProfile& profile,
    const CaptureStillImageBundle& still_image_bundle) {
  if (hardware_id.is_empty() || profile.width == 0 || profile.height == 0 || profile.format_fourcc == 0) {
    return godot::ERR_INVALID_PARAMETER;
  }
  if (!is_valid_capture_still_image_bundle(
          still_image_bundle,
          provider_ ? provider_->supports_multi_image_still_sequence() : false)) {
    return godot::ERR_INVALID_PARAMETER;
  }
  if (is_public_boundary_ready_()) {
    return godot::ERR_UNAVAILABLE;
  }
  godot::String resolved_display_name;
  if (!_resolve_provider_endpoint_(hardware_id, &resolved_display_name)) {
    return godot::ERR_INVALID_PARAMETER;
  }

  PendingEndpointStartupIntent& intent =
      pending_endpoint_startup_intents_[_pending_endpoint_startup_key_(active_session_id_, hardware_id)];
  intent.session_id = active_session_id_;
  intent.hardware_id = hardware_id;
  intent.display_name = resolved_display_name;
  intent.has_still_profile = true;
  intent.still_profile_applied = false;
  intent.still_profile = profile;
  intent.still_image_bundle = still_image_bundle;
  return godot::OK;
}


godot::Error CamBANGServer::set_endpoint_warm_hold_ms_startup_intent(const godot::String& hardware_id, uint32_t warm_hold_ms) {
  if (hardware_id.is_empty()) {
    return godot::ERR_UNAVAILABLE;
  }
  if (is_public_boundary_ready_()) {
    return godot::ERR_UNAVAILABLE;
  }
  godot::String resolved_display_name;
  if (!_resolve_provider_endpoint_(hardware_id, &resolved_display_name)) {
    return godot::ERR_INVALID_PARAMETER;
  }

  PendingEndpointStartupIntent& intent =
      pending_endpoint_startup_intents_[_pending_endpoint_startup_key_(active_session_id_, hardware_id)];
  intent.session_id = active_session_id_;
  intent.hardware_id = hardware_id;
  intent.display_name = resolved_display_name;
  intent.has_warm_policy = true;
  intent.warm_hold_ms = warm_hold_ms;
  intent.warm_policy_wait_ticks = 0;
  return godot::OK;
}

godot::Error CamBANGServer::set_device_warm_hold_ms(uint64_t device_instance_id, uint32_t warm_hold_ms) {
  if (device_instance_id == 0 || !is_public_boundary_ready_() || !provider_) {
    return godot::ERR_BUSY;
  }
  return map_try_set_warm_hold_status(runtime_.try_set_device_warm_hold_ms(device_instance_id, warm_hold_ms));
}

bool CamBANGServer::get_endpoint_capture_template_profile(const godot::String& hardware_id, CaptureProfile& out_profile) const {
  out_profile = CaptureProfile{};
  if (hardware_id.is_empty() || !_resolve_provider_endpoint_(hardware_id, nullptr) || !provider_) {
    return false;
  }
  const CaptureTemplate tmpl = provider_->capture_template();
  out_profile = tmpl.profile;
  if (out_profile.format_fourcc == 0) {
    out_profile.format_fourcc = FOURCC_RGBA;
  }
  return out_profile.width > 0 && out_profile.height > 0 && out_profile.format_fourcc != 0;
}

godot::Dictionary CamBANGServer::get_device_still_capture_profile(uint64_t device_instance_id) const {
  godot::Dictionary out;
  if (device_instance_id == 0 || !is_public_boundary_ready_()) {
    return out;
  }
  CaptureRequest req{};
  if (!runtime_.materialize_capture_request_for_server(device_instance_id, req)) {
    return out;
  }
  out["width"] = static_cast<int64_t>(req.width);
  out["height"] = static_cast<int64_t>(req.height);
  out["format_fourcc"] = static_cast<int64_t>(req.format_fourcc);
  godot::Dictionary seq;
  godot::Array members;
  for (const auto& m : req.still_image_bundle.members) {
    godot::Dictionary md;
    md["image_member_index"] = static_cast<int64_t>(m.image_member_index);
    md["role"] = static_cast<int64_t>(m.role);
    md["role_name"] = (m.role == CaptureStillImageMemberRole::DEFAULT_METERED)
        ? godot::String("DEFAULT_METERED")
        : godot::String("ADDITIONAL_BRACKET");
    md["intended_exposure_compensation_milli_ev"] = static_cast<int64_t>(m.intended_exposure_compensation_milli_ev);
    members.push_back(md);
  }
  seq["members"] = members;
  out["still_image_bundle"] = seq;
  return out;
}

void CamBANGServer::_drain_pending_endpoint_startup_intents_after_baseline_() {
  if (pending_endpoint_startup_intents_.empty()) {
    return;
  }
  if (!is_public_boundary_ready_()) {
    return;
  }

  std::vector<std::string> keys;
  keys.reserve(pending_endpoint_startup_intents_.size());
  for (const auto& kv : pending_endpoint_startup_intents_) {
    keys.push_back(kv.first);
  }
  std::sort(keys.begin(), keys.end());

  for (const std::string& key : keys) {
    auto it = pending_endpoint_startup_intents_.find(key);
    if (it == pending_endpoint_startup_intents_.end()) {
      continue;
    }
    PendingEndpointStartupIntent& intent = it->second;
    if (intent.session_id != active_session_id_) {
      pending_endpoint_startup_intents_.erase(it);
      continue;
    }
    if (intent.hardware_id.is_empty()) {
      pending_endpoint_startup_intents_.erase(it);
      continue;
    }

    if (intent.engage_requested && !intent.engage_applied) {
      const godot::Error rc = engage_endpoint_handle(intent.hardware_id, intent.display_name);
      if (rc != godot::OK) {
        ERR_PRINT(godot::vformat(
            "CamBANGServer: pending startup engage for hardware_id=%s failed after baseline; err=%d reason=%s.",
            intent.hardware_id,
            static_cast<int>(rc),
            godot_error_to_cstr(rc)));
        pending_endpoint_startup_intents_.erase(it);
        continue;
      }
      intent.engage_applied = true;
    }

    const uint64_t device_instance_id = resolve_endpoint_instance_id(intent.hardware_id);
    if (intent.has_still_profile && !intent.still_profile_applied) {
      if (device_instance_id == 0) {
        ERR_PRINT(godot::vformat(
            "CamBANGServer: pending startup still profile for hardware_id=%s failed after baseline; no runtime device instance is available.",
            intent.hardware_id));
        pending_endpoint_startup_intents_.erase(it);
        continue;
      }
      const godot::Error rc = set_device_still_capture_profile(
          device_instance_id,
          intent.still_profile,
          intent.still_image_bundle);
      if (rc != godot::OK) {
        ERR_PRINT(godot::vformat(
            "CamBANGServer: pending startup still profile for hardware_id=%s failed after baseline; err=%d reason=%s.",
            intent.hardware_id,
            static_cast<int>(rc),
            godot_error_to_cstr(rc)));
        pending_endpoint_startup_intents_.erase(it);
        continue;
      }
      intent.still_profile_applied = true;
    }

    if (intent.has_warm_policy) {
      if (device_instance_id == 0) {
        ERR_PRINT(godot::vformat(
            "CamBANGServer: pending startup warm policy for hardware_id=%s failed after baseline; no runtime device instance is available.",
            intent.hardware_id));
        pending_endpoint_startup_intents_.erase(it);
        continue;
      }

      bool device_is_live = false;
      if (latest_) {
        for (const DeviceState& device : latest_->devices) {
          if (device.instance_id == device_instance_id && device.engaged) {
            device_is_live = true;
            break;
          }
        }
      }
      if (!device_is_live) {
        // Opening is asynchronous. Keep the warm-policy tail only for a bounded
        // number of post-baseline drain ticks while waiting for live truth.
        ++intent.warm_policy_wait_ticks;
        if (intent.warm_policy_wait_ticks >= PENDING_ENDPOINT_WARM_POLICY_MAX_DRAIN_TICKS) {
          ERR_PRINT(godot::vformat(
              "CamBANGServer: pending startup warm policy for hardware_id=%s failed after baseline; endpoint did not become live within %d drain ticks.",
              intent.hardware_id,
              static_cast<int>(PENDING_ENDPOINT_WARM_POLICY_MAX_DRAIN_TICKS)));
          pending_endpoint_startup_intents_.erase(it);
        }
        continue;
      }

      const godot::Error rc = set_device_warm_hold_ms(device_instance_id, intent.warm_hold_ms);
      if (rc != godot::OK) {
        ERR_PRINT(godot::vformat(
            "CamBANGServer: pending startup warm policy for hardware_id=%s failed after baseline; err=%d reason=%s.",
            intent.hardware_id,
            static_cast<int>(rc),
            godot_error_to_cstr(rc)));
      }
      pending_endpoint_startup_intents_.erase(it);
      continue;
    }

    pending_endpoint_startup_intents_.erase(it);
  }
}

uint64_t CamBANGServer::trigger_rig_capture_internal_(uint64_t rig_id) {
  if (rig_id == 0 || !is_public_boundary_ready_()) {
    return 0;
  }

  uint64_t capture_id = next_capture_id_.fetch_add(1, std::memory_order_relaxed);
  if (capture_id == 0) {
    capture_id = next_capture_id_.fetch_add(1, std::memory_order_relaxed);
  }

  const auto orchestration = runtime_.orchestrate_rig_capture_with_capture_id_for_server(rig_id, capture_id);
  if (!orchestration.ok) {
    return 0;
  }
  return capture_id;
}

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

void CamBANGServer::_disconnect_tick_if_connected_() {
  if (!tick_connected_) {
    return;
  }

  tick_connected_ = false;
  last_tick_time_ns_ = 0;

  godot::Engine* engine = godot::Engine::get_singleton();
  if (!engine) {
    return;
  }

  godot::MainLoop* ml = engine->get_main_loop();
  godot::SceneTree* tree = godot::Object::cast_to<godot::SceneTree>(ml);
  if (!tree) {
    return;
  }

  godot::Callable cb(this, "_on_godot_process_frame");
  if (tree->is_connected("process_frame", cb)) {
    tree->disconnect("process_frame", cb);
  }
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
  _drain_pending_stop_and_quit_();
}

bool CamBANGServer::_consume_latest_core_snapshot() {
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

  last_seen_published_seq_ = published_seq;

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

  if (latest_) {
    std::unordered_set<uint64_t> next_stream_ids;
    next_stream_ids.reserve(snap->streams.size());
    for (const StreamState& stream : snap->streams) {
      if (stream.stream_id != 0) {
        next_stream_ids.insert(stream.stream_id);
      }
    }

    for (const StreamState& prior_stream : latest_->streams) {
      if (prior_stream.stream_id == 0) {
        continue;
      }
      if (next_stream_ids.find(prior_stream.stream_id) != next_stream_ids.end()) {
        continue;
      }
      godot_gpu_display_invalidate_stream(prior_stream.stream_id);
      CamBANGStreamResult::remove_live_stream_cpu_display_view(prior_stream.stream_id);
    }
  }

  latest_ = snap;
  _reconcile_endpoint_lifecycle_from_snapshot(*snap);

  // Export as a struct-like Variant graph for Godot inspection.
  latest_export_ = export_snapshot_to_godot(*snap, godot_gen_, godot_version_, godot_topology_version_);
  has_latest_export_ = true;
  _refresh_tracked_wrapper_live_states_from_snapshot_();

  emit_signal("state_published",
              static_cast<uint64_t>(godot_gen_),
              static_cast<uint64_t>(godot_version_),
              static_cast<uint64_t>(godot_topology_version_));

  return true;
}

void CamBANGServer::_reconcile_endpoint_lifecycle_from_snapshot(const CamBANGStateSnapshot& snap) {
  if (endpoint_lifecycle_by_hardware_id_.empty()) {
    return;
  }
  std::unordered_set<uint64_t> live_devices;
  live_devices.reserve(snap.devices.size());
  for (const auto& device : snap.devices) {
    if (device.instance_id != 0) {
      live_devices.insert(device.instance_id);
    }
  }

  for (auto& kv : endpoint_lifecycle_by_hardware_id_) {
    EndpointLifecycleState& state = kv.second;
    if (!state.close_requested || state.device_instance_id == 0) {
      continue;
    }
    if (live_devices.find(state.device_instance_id) == live_devices.end()) {
      state.device_instance_id = 0;
      state.root_id = 0;
      state.open_requested = false;
      state.close_requested = false;
    }
  }
}

void CamBANGServer::_on_godot_tick(double delta) {
  const uint64_t now_ns = last_tick_time_ns_;
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
    char line[192];
    if (runtime_.take_core_banner_line(line, sizeof(line))) {
      godot::UtilityFunctions::print(line);
    }

    // CPU_PACKED stream display views use stream-owned live ImageTexture
    // wrappers that are refreshed from latest retained stream state each tick.
    CamBANGStreamResult::refresh_live_stream_cpu_display_views(runtime_);
    synthetic_gpu_backing_drain_pending_live_display_wrapper_refreshes();
  }

  std::string timeline_line;
  while (timeline_teardown_trace_try_pop(timeline_line)) {
    _handle_timeline_teardown_trace_line(timeline_line);
    if (timeline_trace_echo_enabled_) {
      godot::UtilityFunctions::print(timeline_line.c_str());
    }
  }

  // Godot-facing snapshot truth is tick-bounded.
  // Core may publish multiple intermediate snapshots between Godot ticks.
  // We emit at most once per tick, and only if *anything* has changed since
  // the previous tick.
  std::vector<CoreBackingPlanEvaluationReport> backing_plan_reports;
  bool have_backing_plan_reports = false;
  const auto& ensure_backing_plan_reports = [&]() -> const std::vector<CoreBackingPlanEvaluationReport>& {
    if (!have_backing_plan_reports) {
      if (runtime_.is_running() && is_public_boundary_ready_() && latest_) {
        backing_plan_reports = runtime_.backing_plan_evaluation_reports();
      } else {
        backing_plan_reports.clear();
      }
      have_backing_plan_reports = true;
    }
    return backing_plan_reports;
  };

  if (_consume_latest_core_snapshot()) {
    _drain_pending_endpoint_startup_intents_after_baseline_();
    _drain_pending_scenario_start_after_baseline_();
    _arm_live_retained_result_access_calibration_from_snapshot_(
        now_ns,
        ensure_backing_plan_reports());
  } else if (!pending_endpoint_startup_intents_.empty()) {
    _drain_pending_endpoint_startup_intents_after_baseline_();
  }
  const auto& backing_reports = ensure_backing_plan_reports();
  _observe_active_stream_evaluation_calibration_identities_(now_ns, backing_reports);
  _observe_active_capture_evaluation_calibration_identities_(now_ns, backing_reports);
  _process_armed_live_retained_result_access_calibration_(now_ns);
}

void CamBANGServer::_clear_live_retained_result_access_calibration_state_() {
  pending_live_stream_retained_result_calibrations_.clear();
  completed_live_stream_retained_result_calibrations_.clear();
  pending_live_capture_retained_result_calibrations_.clear();
  completed_live_capture_retained_result_calibrations_.clear();
}

void CamBANGServer::_arm_live_retained_result_access_calibration_from_snapshot_(
    uint64_t now_ns,
    const std::vector<CoreBackingPlanEvaluationReport>& backing_plan_reports) {
  const auto metrics_t0 = std::chrono::steady_clock::now();
  ++g_live_retained_calibration_metrics.arm_calls;
  if (!runtime_.is_running() || !is_public_boundary_ready_() || !latest_) {
    _clear_live_retained_result_access_calibration_state_();
    const uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - metrics_t0)
            .count());
    g_live_retained_calibration_metrics.arm_total_ns += elapsed_ns;
    g_live_retained_calibration_metrics.arm_pending_stream_count +=
        static_cast<uint64_t>(pending_live_stream_retained_result_calibrations_.size());
    return;
  }

  std::unordered_set<uint64_t> active_stream_evaluator_ids;
  for (const CoreBackingPlanEvaluationReport& report : backing_plan_reports) {
    if (report.parent_kind ==
            CoreBackingPlanEvaluationReport::ParentKind::Stream &&
        report.evaluator_active &&
        report.stream_id != 0 &&
        report.requested.valid) {
      active_stream_evaluator_ids.insert(report.stream_id);
    }
  }

  std::unordered_set<uint64_t> live_stream_ids;
  const auto same_stream_identity =
      [](const ArmedLiveStreamRetainedResultCalibration& armed,
         const SharedStreamResultData& data) noexcept {
        return data &&
               armed.stream_id == data->stream_id &&
               armed.posture_id == data->access_posture.posture_id &&
               armed.evaluation_identity == 0 &&
               armed.display_view == data->retained_access_truth.display_view &&
               armed.to_image == data->retained_access_truth.to_image;
      };
  for (const StreamState& stream : latest_->streams) {
    if (stream.stream_id == 0 || stream.phase != CBLifecyclePhase::LIVE) {
      continue;
    }
    live_stream_ids.insert(stream.stream_id);
    if (active_stream_evaluator_ids.find(stream.stream_id) !=
        active_stream_evaluator_ids.end()) {
      continue;
    }
    SharedStreamResultData result = runtime_.get_latest_stream_result(stream.stream_id);
    if (!result || result->access_posture.posture_id == 0) {
      pending_live_stream_retained_result_calibrations_.erase(stream.stream_id);
      completed_live_stream_retained_result_calibrations_.erase(stream.stream_id);
      continue;
    }
    ArmedLiveStreamRetainedResultCalibration armed{};
    armed.stream_id = result->stream_id;
    armed.posture_id = result->access_posture.posture_id;
    armed.evaluation_identity = 0;
    armed.display_view = result->retained_access_truth.display_view;
    armed.to_image = result->retained_access_truth.to_image;
    const bool needs_settle_delay =
        result->retained_access_truth.display_view != ResultCapability::UNSUPPORTED ||
        result->retained_access_truth.to_image != ResultCapability::UNSUPPORTED;
    armed.due_after_ns = now_ns + (needs_settle_delay
                                       ? runtime_.stream_backing_plan_evaluation_settle_delay_ns()
                                       : 0);
    const auto pending_it =
        pending_live_stream_retained_result_calibrations_.find(stream.stream_id);
    if (pending_it != pending_live_stream_retained_result_calibrations_.end() &&
        same_stream_identity(pending_it->second, result)) {
      continue;
    }
    const auto completed_it =
        completed_live_stream_retained_result_calibrations_.find(stream.stream_id);
    if (completed_it != completed_live_stream_retained_result_calibrations_.end() &&
        same_stream_identity(completed_it->second, result)) {
      continue;
    }
    pending_live_stream_retained_result_calibrations_[stream.stream_id] = armed;
    completed_live_stream_retained_result_calibrations_.erase(stream.stream_id);
  }
  for (auto it = pending_live_stream_retained_result_calibrations_.begin();
       it != pending_live_stream_retained_result_calibrations_.end();) {
    if (live_stream_ids.find(it->first) == live_stream_ids.end()) {
      completed_live_stream_retained_result_calibrations_.erase(it->first);
      it = pending_live_stream_retained_result_calibrations_.erase(it);
      continue;
    }
    ++it;
  }
  for (auto it = completed_live_stream_retained_result_calibrations_.begin();
       it != completed_live_stream_retained_result_calibrations_.end();) {
    if (live_stream_ids.find(it->first) == live_stream_ids.end()) {
      it = completed_live_stream_retained_result_calibrations_.erase(it);
      continue;
    }
    ++it;
  }

  std::unordered_set<uint64_t> live_capture_device_ids;
  const auto same_capture_identity =
      [](const ArmedLiveCaptureRetainedResultCalibration& armed,
         const SharedCaptureResultData& data,
         uint64_t evaluation_identity) noexcept {
        return data &&
               armed.device_instance_id == data->device_instance_id &&
               armed.capture_id == data->capture_id &&
               armed.acquisition_session_id == data->acquisition_session_id &&
               armed.evaluation_identity == evaluation_identity &&
               armed.member_identity_signature ==
                   build_capture_member_identity_signature(data);
      };
  for (const AcquisitionSessionState& session : latest_->acquisition_sessions) {
    if (session.last_capture_id == 0 || session.device_instance_id == 0) {
      continue;
    }
    live_capture_device_ids.insert(session.device_instance_id);
    SharedCaptureResultData result =
        runtime_.get_capture_result(session.last_capture_id, session.device_instance_id);
    if (!result) {
      pending_live_capture_retained_result_calibrations_.erase(
          session.device_instance_id);
      completed_live_capture_retained_result_calibrations_.erase(
          session.device_instance_id);
      continue;
    }
    ArmedLiveCaptureRetainedResultCalibration armed{};
    armed.device_instance_id = result->device_instance_id;
    armed.capture_id = result->capture_id;
    armed.acquisition_session_id = result->acquisition_session_id;
    armed.member_identity_signature =
        build_capture_member_identity_signature(result);
    bool needs_settle_delay = false;
    for (uint32_t i = 0; i < result->image_member_count(); ++i) {
      const auto* member = result->image_member_at(i);
      if (member && member->retained_access_truth.to_image != ResultCapability::UNSUPPORTED) {
        needs_settle_delay = true;
        break;
      }
    }
    armed.due_after_ns = now_ns + (needs_settle_delay
                                       ? runtime_.capture_backing_plan_evaluation_settle_delay_ns()
                                       : 0);
    const auto pending_it = pending_live_capture_retained_result_calibrations_.find(
        session.device_instance_id);
    if (pending_it != pending_live_capture_retained_result_calibrations_.end() &&
        same_capture_identity(pending_it->second, result, armed.evaluation_identity)) {
      continue;
    }
    const auto completed_it =
        completed_live_capture_retained_result_calibrations_.find(
            session.device_instance_id);
    if (completed_it != completed_live_capture_retained_result_calibrations_.end() &&
        same_capture_identity(completed_it->second, result, armed.evaluation_identity)) {
      continue;
    }
    pending_live_capture_retained_result_calibrations_[session.device_instance_id] =
        armed;
    completed_live_capture_retained_result_calibrations_.erase(
        session.device_instance_id);
  }
  for (const RigState& rig : latest_->rigs) {
    if (rig.last_capture_id == 0) {
      continue;
    }
    std::vector<SharedCaptureResultData> results =
        runtime_.get_capture_result_set(rig.last_capture_id);
    for (const SharedCaptureResultData& result : results) {
      if (!result || result->device_instance_id == 0) {
        continue;
      }
      live_capture_device_ids.insert(result->device_instance_id);
      ArmedLiveCaptureRetainedResultCalibration armed{};
      armed.device_instance_id = result->device_instance_id;
      armed.capture_id = result->capture_id;
      armed.acquisition_session_id = result->acquisition_session_id;
      armed.member_identity_signature =
          build_capture_member_identity_signature(result);
      bool needs_settle_delay = false;
      for (uint32_t i = 0; i < result->image_member_count(); ++i) {
        const auto* member = result->image_member_at(i);
        if (member &&
            member->retained_access_truth.to_image !=
                ResultCapability::UNSUPPORTED) {
          needs_settle_delay = true;
          break;
        }
      }
      armed.due_after_ns = now_ns + (needs_settle_delay
                                         ? runtime_.capture_backing_plan_evaluation_settle_delay_ns()
                                         : 0);
      const auto pending_it =
          pending_live_capture_retained_result_calibrations_.find(
              result->device_instance_id);
      if (pending_it !=
              pending_live_capture_retained_result_calibrations_.end() &&
          same_capture_identity(pending_it->second, result, armed.evaluation_identity)) {
        continue;
      }
      const auto completed_it =
          completed_live_capture_retained_result_calibrations_.find(
              result->device_instance_id);
      if (completed_it !=
              completed_live_capture_retained_result_calibrations_.end() &&
          same_capture_identity(completed_it->second, result, armed.evaluation_identity)) {
        continue;
      }
      pending_live_capture_retained_result_calibrations_[result->device_instance_id] =
          armed;
      completed_live_capture_retained_result_calibrations_.erase(
          result->device_instance_id);
    }
  }
  for (auto it = pending_live_capture_retained_result_calibrations_.begin();
       it != pending_live_capture_retained_result_calibrations_.end();) {
    if (live_capture_device_ids.find(it->first) == live_capture_device_ids.end()) {
      completed_live_capture_retained_result_calibrations_.erase(it->first);
      it = pending_live_capture_retained_result_calibrations_.erase(it);
      continue;
    }
    ++it;
  }
  for (auto it = completed_live_capture_retained_result_calibrations_.begin();
       it != completed_live_capture_retained_result_calibrations_.end();) {
    if (live_capture_device_ids.find(it->first) == live_capture_device_ids.end()) {
      it = completed_live_capture_retained_result_calibrations_.erase(it);
      continue;
    }
    ++it;
  }
  const uint64_t elapsed_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - metrics_t0)
          .count());
  g_live_retained_calibration_metrics.arm_total_ns += elapsed_ns;
  g_live_retained_calibration_metrics.arm_pending_stream_count +=
      static_cast<uint64_t>(pending_live_stream_retained_result_calibrations_.size());
}

void CamBANGServer::_observe_active_stream_evaluation_calibration_identities_(
    uint64_t now_ns,
    const std::vector<CoreBackingPlanEvaluationReport>& backing_plan_reports) {
  const auto metrics_t0 = std::chrono::steady_clock::now();
  ++g_live_retained_calibration_metrics.observe_stream_calls;
  if (!runtime_.is_running() || !is_public_boundary_ready_() || !latest_) {
    const uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - metrics_t0)
            .count());
    g_live_retained_calibration_metrics.observe_stream_total_ns += elapsed_ns;
    g_live_retained_calibration_metrics.observe_stream_pending_stream_count +=
        static_cast<uint64_t>(pending_live_stream_retained_result_calibrations_.size());
    return;
  }

  std::unordered_set<uint64_t> active_stream_evaluator_ids;
  for (const CoreBackingPlanEvaluationReport& report : backing_plan_reports) {
    if (report.parent_kind ==
            CoreBackingPlanEvaluationReport::ParentKind::Stream &&
        report.evaluator_active &&
        report.stream_id != 0 &&
        report.requested.valid) {
      active_stream_evaluator_ids.insert(report.stream_id);
    }
  }

  const auto same_stream_identity =
      [](const ArmedLiveStreamRetainedResultCalibration& armed,
         const SharedStreamResultData& data,
         uint64_t evaluation_identity) noexcept {
        return data &&
               armed.stream_id == data->stream_id &&
               armed.posture_id == data->access_posture.posture_id &&
               armed.evaluation_identity == evaluation_identity &&
               armed.display_view == data->retained_access_truth.display_view &&
               armed.to_image == data->retained_access_truth.to_image;
      };

  for (const StreamState& stream : latest_->streams) {
    if (stream.stream_id == 0 || stream.phase != CBLifecyclePhase::LIVE) {
      continue;
    }
    if (active_stream_evaluator_ids.find(stream.stream_id) !=
        active_stream_evaluator_ids.end()) {
      continue;
    }
    const SharedStreamResultData result =
        runtime_.get_latest_stream_result(stream.stream_id);
    if (!result || result->access_posture.posture_id == 0) {
      continue;
    }

    ArmedLiveStreamRetainedResultCalibration armed{};
    armed.stream_id = result->stream_id;
    armed.posture_id = result->access_posture.posture_id;
    armed.evaluation_identity = 0;
    armed.display_view = result->retained_access_truth.display_view;
    armed.to_image = result->retained_access_truth.to_image;
    const bool needs_settle_delay =
        result->retained_access_truth.display_view !=
            ResultCapability::UNSUPPORTED ||
        result->retained_access_truth.to_image != ResultCapability::UNSUPPORTED;
    armed.due_after_ns = now_ns + (needs_settle_delay
                                       ? runtime_.stream_backing_plan_evaluation_settle_delay_ns()
                                       : 0);

    const auto pending_it =
        pending_live_stream_retained_result_calibrations_.find(stream.stream_id);
    if (pending_it != pending_live_stream_retained_result_calibrations_.end() &&
        same_stream_identity(
            pending_it->second, result, armed.evaluation_identity)) {
      continue;
    }
    const auto completed_it =
        completed_live_stream_retained_result_calibrations_.find(stream.stream_id);
    if (completed_it != completed_live_stream_retained_result_calibrations_.end() &&
        same_stream_identity(
            completed_it->second, result, armed.evaluation_identity)) {
      continue;
    }

    pending_live_stream_retained_result_calibrations_[stream.stream_id] = armed;
    completed_live_stream_retained_result_calibrations_.erase(stream.stream_id);
  }

  for (const CoreBackingPlanEvaluationReport& report : backing_plan_reports) {
    if (report.parent_kind !=
            CoreBackingPlanEvaluationReport::ParentKind::Stream ||
        !report.evaluator_active ||
        report.stream_id == 0 ||
        !report.requested.valid) {
      continue;
    }

    const SharedStreamResultData result =
        runtime_.get_latest_stream_result(report.stream_id);
    CoreProductionPostureShape observed_posture{};
    if (!infer_stream_result_posture_shape_for_calibration(
            result, observed_posture) ||
        observed_posture != report.requested.posture) {
      continue;
    }

    ArmedLiveStreamRetainedResultCalibration armed{};
    armed.stream_id = result->stream_id;
    armed.posture_id = result->access_posture.posture_id;
    armed.evaluation_identity = build_stream_evaluator_identity_signature(report);
    armed.display_view = result->retained_access_truth.display_view;
    armed.to_image = result->retained_access_truth.to_image;
    const bool needs_settle_delay =
        result->retained_access_truth.display_view !=
            ResultCapability::UNSUPPORTED ||
        result->retained_access_truth.to_image != ResultCapability::UNSUPPORTED;
    armed.due_after_ns = now_ns + (needs_settle_delay
                                       ? runtime_.stream_backing_plan_evaluation_settle_delay_ns()
                                       : 0);

    const auto pending_it =
        pending_live_stream_retained_result_calibrations_.find(report.stream_id);
    if (pending_it != pending_live_stream_retained_result_calibrations_.end() &&
        same_stream_identity(
            pending_it->second, result, armed.evaluation_identity)) {
      continue;
    }
    const auto completed_it =
        completed_live_stream_retained_result_calibrations_.find(report.stream_id);
    if (completed_it != completed_live_stream_retained_result_calibrations_.end() &&
        same_stream_identity(
            completed_it->second, result, armed.evaluation_identity)) {
      continue;
    }

    pending_live_stream_retained_result_calibrations_[report.stream_id] = armed;
    completed_live_stream_retained_result_calibrations_.erase(report.stream_id);
  }
  const uint64_t elapsed_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - metrics_t0)
          .count());
  g_live_retained_calibration_metrics.observe_stream_total_ns += elapsed_ns;
  g_live_retained_calibration_metrics.observe_stream_pending_stream_count +=
      static_cast<uint64_t>(pending_live_stream_retained_result_calibrations_.size());
}

void CamBANGServer::_observe_active_capture_evaluation_calibration_identities_(
    uint64_t now_ns,
    const std::vector<CoreBackingPlanEvaluationReport>& backing_plan_reports) {
  const auto metrics_t0 = std::chrono::steady_clock::now();
  ++g_live_retained_calibration_metrics.observe_capture_calls;
  if (!runtime_.is_running() || !is_public_boundary_ready_()) {
    const uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - metrics_t0)
            .count());
    g_live_retained_calibration_metrics.observe_capture_total_ns += elapsed_ns;
    g_live_retained_calibration_metrics.observe_capture_pending_capture_count +=
        static_cast<uint64_t>(pending_live_capture_retained_result_calibrations_.size());
    return;
  }

  const auto same_capture_identity =
      [](const ArmedLiveCaptureRetainedResultCalibration& armed,
         const SharedCaptureResultData& data,
         uint64_t evaluation_identity) noexcept {
        return data &&
               armed.device_instance_id == data->device_instance_id &&
               armed.capture_id == data->capture_id &&
               armed.acquisition_session_id == data->acquisition_session_id &&
               armed.evaluation_identity == evaluation_identity &&
               armed.member_identity_signature ==
                   build_capture_member_identity_signature(data);
      };

  for (const CoreBackingPlanEvaluationReport& report : backing_plan_reports) {
    if (report.target_kind != CoreBackingPlanEvaluationReport::TargetKind::Capture ||
        !report.evaluator_active ||
        report.device_instance_id == 0 ||
        !report.requested.valid) {
      continue;
    }

    const auto latest_capture_it =
        latest_capture_id_by_device_instance_id_.find(report.device_instance_id);
    if (latest_capture_it == latest_capture_id_by_device_instance_id_.end() ||
        latest_capture_it->second == 0) {
      continue;
    }

    const SharedCaptureResultData result = runtime_.get_capture_result(
        latest_capture_it->second, report.device_instance_id);
    CoreProductionPostureShape observed_posture{};
    if (!infer_capture_result_posture_shape_for_calibration(
            result, observed_posture) ||
        observed_posture != report.requested.posture) {
      continue;
    }

    ArmedLiveCaptureRetainedResultCalibration armed{};
    armed.device_instance_id = result->device_instance_id;
    armed.capture_id = result->capture_id;
    armed.acquisition_session_id = result->acquisition_session_id;
    armed.member_identity_signature =
        build_capture_member_identity_signature(result);
    armed.evaluation_identity =
        build_capture_evaluator_identity_signature(report);
    bool needs_settle_delay = false;
    for (uint32_t i = 0; i < result->image_member_count(); ++i) {
      const auto* member = result->image_member_at(i);
      if (member &&
          member->retained_access_truth.to_image != ResultCapability::UNSUPPORTED) {
        needs_settle_delay = true;
        break;
      }
    }
    armed.due_after_ns = now_ns + (needs_settle_delay
                                       ? runtime_.capture_backing_plan_evaluation_settle_delay_ns()
                                       : 0);

    const auto pending_it =
        pending_live_capture_retained_result_calibrations_.find(
            report.device_instance_id);
    if (pending_it != pending_live_capture_retained_result_calibrations_.end() &&
        same_capture_identity(
            pending_it->second, result, armed.evaluation_identity)) {
      continue;
    }
    const auto completed_it =
        completed_live_capture_retained_result_calibrations_.find(
            report.device_instance_id);
    if (completed_it !=
            completed_live_capture_retained_result_calibrations_.end() &&
        same_capture_identity(
            completed_it->second, result, armed.evaluation_identity)) {
      continue;
    }

    pending_live_capture_retained_result_calibrations_[report.device_instance_id] =
        armed;
    completed_live_capture_retained_result_calibrations_.erase(
        report.device_instance_id);
  }
  const uint64_t elapsed_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - metrics_t0)
          .count());
  g_live_retained_calibration_metrics.observe_capture_total_ns += elapsed_ns;
  g_live_retained_calibration_metrics.observe_capture_pending_capture_count +=
      static_cast<uint64_t>(pending_live_capture_retained_result_calibrations_.size());
}

void CamBANGServer::_process_armed_live_retained_result_access_calibration_(
    uint64_t now_ns) {
  const auto metrics_t0 = std::chrono::steady_clock::now();
  ++g_live_retained_calibration_metrics.process_calls;
  if (!runtime_.is_running() || !is_public_boundary_ready_()) {
    _clear_live_retained_result_access_calibration_state_();
    const uint64_t elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - metrics_t0)
            .count());
    g_live_retained_calibration_metrics.process_total_ns += elapsed_ns;
    g_live_retained_calibration_metrics.process_pending_stream_count +=
        static_cast<uint64_t>(pending_live_stream_retained_result_calibrations_.size());
    g_live_retained_calibration_metrics.process_pending_capture_count +=
        static_cast<uint64_t>(pending_live_capture_retained_result_calibrations_.size());
    return;
  }

  const auto same_stream_identity =
      [](const ArmedLiveStreamRetainedResultCalibration& armed,
         const SharedStreamResultData& data) noexcept {
        return data &&
               armed.stream_id == data->stream_id &&
               armed.posture_id == data->access_posture.posture_id &&
               armed.display_view == data->retained_access_truth.display_view &&
               armed.to_image == data->retained_access_truth.to_image;
      };
  for (auto it = pending_live_stream_retained_result_calibrations_.begin();
       it != pending_live_stream_retained_result_calibrations_.end();) {
    if (now_ns < it->second.due_after_ns) {
      ++it;
      continue;
    }
    SharedStreamResultData result = runtime_.get_latest_stream_result(it->first);
    if (!same_stream_identity(it->second, result)) {
      it = pending_live_stream_retained_result_calibrations_.erase(it);
      continue;
    }
    retained_result_access_calibration::calibrate_stream_result(result, &runtime_);
    completed_live_stream_retained_result_calibrations_[it->first] = it->second;
    it = pending_live_stream_retained_result_calibrations_.erase(it);
  }

  const auto same_capture_identity =
      [](const ArmedLiveCaptureRetainedResultCalibration& armed,
         const SharedCaptureResultData& data,
         uint64_t evaluation_identity) noexcept {
        return data &&
               armed.device_instance_id == data->device_instance_id &&
               armed.capture_id == data->capture_id &&
               armed.acquisition_session_id == data->acquisition_session_id &&
               armed.evaluation_identity == evaluation_identity &&
               armed.member_identity_signature ==
                   build_capture_member_identity_signature(data);
      };
  for (auto it = pending_live_capture_retained_result_calibrations_.begin();
       it != pending_live_capture_retained_result_calibrations_.end();) {
    if (now_ns < it->second.due_after_ns) {
      ++it;
      continue;
    }
    SharedCaptureResultData result =
        runtime_.get_capture_result(it->second.capture_id, it->first);
    if (!same_capture_identity(
            it->second, result, it->second.evaluation_identity)) {
      it = pending_live_capture_retained_result_calibrations_.erase(it);
      continue;
    }
    retained_result_access_calibration::calibrate_capture_result(result, &runtime_);
    completed_live_capture_retained_result_calibrations_[it->first] =
        it->second;
    it = pending_live_capture_retained_result_calibrations_.erase(it);
  }
  const uint64_t elapsed_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - metrics_t0)
          .count());
  g_live_retained_calibration_metrics.process_total_ns += elapsed_ns;
  g_live_retained_calibration_metrics.process_pending_stream_count +=
      static_cast<uint64_t>(pending_live_stream_retained_result_calibrations_.size());
  g_live_retained_calibration_metrics.process_pending_capture_count +=
      static_cast<uint64_t>(pending_live_capture_retained_result_calibrations_.size());
}

void CamBANGServer::_drain_pending_stop_and_quit_() {
  if (!pending_stop_and_quit_) {
    return;
  }

  if (pending_stop_and_quit_frames_remaining_ > 0) {
    --pending_stop_and_quit_frames_remaining_;
    if (pending_stop_and_quit_frames_remaining_ > 0) {
      return;
    }
  }

  godot::Engine* engine = godot::Engine::get_singleton();
  godot::SceneTree* tree = nullptr;
  if (engine) {
    tree = godot::Object::cast_to<godot::SceneTree>(engine->get_main_loop());
  }
  if (!tree) {
    pending_stop_and_quit_ = false;
    godot::UtilityFunctions::push_warning(
        "CamBANGServer.stop_and_quit() dropped pending quit because no SceneTree main loop is available.");
    return;
  }

  const int exit_code = pending_stop_and_quit_exit_code_;
  pending_stop_and_quit_ = false;
  pending_stop_and_quit_frames_remaining_ = 0;
  pending_stop_and_quit_exit_code_ = 0;
  tree->quit(exit_code);
}

godot::Variant CamBANGServer::get_state_snapshot() const {
  if (!has_latest_export_) {
    return godot::Variant();
  }
  return latest_export_;
}


godot::Variant CamBANGServer::get_synthetic_metrics_snapshot() const {
  if (!runtime_.is_running()) {
    return godot::Variant();
  }
  const ProviderBroker* broker = dynamic_cast<const ProviderBroker*>(provider_.get());
  if (!broker) {
    return godot::Variant();
  }
  SyntheticMetricsSnapshot snap{};
  if (!broker->get_synthetic_metrics_snapshot_for_host(snap)) {
    return godot::Variant();
  }
  godot::Dictionary d;
  d["current_virtual_timeline_ns"] = static_cast<uint64_t>(snap.current_virtual_timeline_ns);
  d["total_emitted_frames"] = static_cast<uint64_t>(snap.total_emitted_frames);
  d["gpu_update_attempts"] = static_cast<uint64_t>(snap.gpu_update_attempts);
  d["gpu_update_demand_skipped"] = static_cast<uint64_t>(snap.gpu_update_demand_skipped);
  d["gpu_texture_update_calls"] = static_cast<uint64_t>(snap.gpu_texture_update_calls);
  d["frame_copy_calls"] = static_cast<uint64_t>(snap.frame_copy_calls);
  d["frame_render_total_ms"] = snap.frame_render_total_ms;
  d["pattern_overlay_total_ms"] = snap.pattern_overlay_total_ms;
  d["pattern_base_copy_total_ms"] = snap.pattern_base_copy_total_ms;
  d["gpu_update_total_total_ms"] = snap.gpu_update_total_total_ms;
  d["gpu_upload_copy_total_ms"] = snap.gpu_upload_copy_total_ms;
  d["gpu_texture_update_total_ms"] = snap.gpu_texture_update_total_ms;
  d["catchup_ticks_capped"] = static_cast<uint64_t>(snap.catchup_ticks_capped);
  d["catchup_frames_dropped"] = static_cast<uint64_t>(snap.catchup_frames_dropped);
  const godot::Dictionary cpu_display_metrics =
      CamBANGStreamResult::get_live_stream_cpu_display_metrics_snapshot();
  const godot::Array cpu_display_metric_keys = cpu_display_metrics.keys();
  for (int i = 0; i < cpu_display_metric_keys.size(); ++i) {
    const godot::Variant key = cpu_display_metric_keys[i];
    d.set(key, cpu_display_metrics.get(key, godot::Variant()));
  }
  const godot::Dictionary live_retained_calibration_metrics =
      live_retained_calibration_metrics_snapshot();
  const godot::Array live_retained_calibration_metric_keys =
      live_retained_calibration_metrics.keys();
  for (int i = 0; i < live_retained_calibration_metric_keys.size(); ++i) {
    const godot::Variant key = live_retained_calibration_metric_keys[i];
    d.set(key, live_retained_calibration_metrics.get(key, godot::Variant()));
  }
  d.set(
      godot::Variant(godot::String("result_access_timing_evidence")),
      godot::Variant(result_access_cost_evidence::snapshot()));
  godot::Array evaluation_reports;
  for (const CoreBackingPlanEvaluationReport& report :
       runtime_.backing_plan_evaluation_reports()) {
    evaluation_reports.append(backing_plan_evaluation_report_to_dictionary(report));
  }
  d.set(
      godot::Variant(godot::String("backing_plan_evaluation_reports")),
      godot::Variant(evaluation_reports));
  return d;
}

godot::Variant CamBANGServer::get_active_provider_config() const {
  if (!runtime_.is_running()) {
    return godot::Variant();
  }
  const ProviderBroker* broker = dynamic_cast<const ProviderBroker*>(provider_.get());
  if (!broker) {
    return godot::Variant();
  }
  godot::Dictionary d;
  const RuntimeMode mode = broker->runtime_mode_latched();
  d["provider_kind"] = runtime_mode_to_provider_kind_int(mode);
  d["timeline_reconciliation"] = godot::Variant();
  if (mode == RuntimeMode::synthetic) {
    const SyntheticRole role = broker->synthetic_role_latched();
    const TimingDriver timing_driver = broker->synthetic_timing_driver_latched();
    d["synthetic_role"] = static_cast<int>(role);
    d["timing_driver"] = static_cast<int>(timing_driver);
    if (role == SyntheticRole::Timeline && timing_driver == TimingDriver::VirtualTime) {
      const TimelineReconciliation reconciliation = broker->synthetic_timeline_reconciliation_latched();
      d["timeline_reconciliation"] =
          (reconciliation == TimelineReconciliation::CompletionGated)
              ? godot::String("completion_gated")
              : godot::String("strict");
    }
  } else {
    d["synthetic_role"] = godot::Variant();
    d["timing_driver"] = godot::Variant();
  }
  return d;
}

godot::Error CamBANGServer::select_builtin_scenario(const godot::String& scenario_name) {
  if (!is_synthetic_timeline_session_active_()) {
    ERR_PRINT("CamBANGServer: select_builtin_scenario requires synthetic timeline mode.");
    _reset_scenario_session_state_();
    return godot::ERR_UNAVAILABLE;
  }
  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  const std::string scenario_utf8 = scenario_name.utf8().get_data();
  const ProviderResult pr = broker->select_timeline_builtin_scenario_for_host(scenario_utf8);
  if (pr.ok()) {
    scenario_config_staged_for_session_ = true;
    strict_scenario_unmet_logged_ = false;
  } else {
    _reset_scenario_session_state_();
    ERR_PRINT(godot::vformat(
        "CamBANGServer: select_builtin_scenario failed; provider_error=%s.",
        cambang::to_string(pr.code)));
  }
  return map_provider_result_to_godot_error(pr);
}

godot::Error CamBANGServer::load_external_scenario(const godot::String& json_text) {
  if (!is_synthetic_timeline_session_active_()) {
    ERR_PRINT("CamBANGServer: load_external_scenario requires synthetic timeline mode.");
    _reset_scenario_session_state_();
    return godot::ERR_UNAVAILABLE;
  }
  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  const std::string text_utf8 = json_text.utf8().get_data();
  const ProviderResult pr = broker->load_timeline_canonical_scenario_from_json_text_for_host(text_utf8);
  if (pr.ok()) {
    scenario_config_staged_for_session_ = true;
    strict_scenario_unmet_logged_ = false;
  } else {
    _reset_scenario_session_state_();
    ERR_PRINT(godot::vformat(
        "CamBANGServer: load_external_scenario failed; provider_error=%s.",
        cambang::to_string(pr.code)));
  }
  return map_provider_result_to_godot_error(pr);
}

godot::Error CamBANGServer::_start_scenario_now_() {
  if (!is_synthetic_timeline_session_active_()) {
    ERR_PRINT("CamBANGServer: start_scenario requires synthetic timeline mode.");
    return godot::ERR_UNAVAILABLE;
  }
  if (!scenario_config_staged_for_session_) {
    ERR_PRINT("CamBANGServer: start_scenario rejected because no scenario is staged for this session.");
    return godot::ERR_INVALID_PARAMETER;
  }

  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  const ProviderResult pr = broker->start_timeline_scenario_for_host();
  if (!pr.ok()) {
    ERR_PRINT(godot::vformat(
        "CamBANGServer: start_scenario rejected by provider; provider_error=%s.",
        cambang::to_string(pr.code)));
    return map_provider_result_to_godot_error(pr);
  }

  strict_scenario_unmet_logged_ = false;
  std::vector<SyntheticStagedRigTopology> staged_rigs;
  if (broker->get_synthetic_staged_rig_topology_for_host(staged_rigs)) {
    for (const auto& r : staged_rigs) {
      if (!runtime_.retain_rig_member_hardware_ids(r.rig_id, r.member_hardware_ids)) {
        (void)broker->stop_timeline_scenario_for_host();
        ERR_PRINT("CamBANGServer: start_scenario failed because staged rig topology could not be admitted to the runtime.");
        return godot::ERR_BUSY;
      }
    }
  }

  // Deterministic host-stepper guarantee for current-time timeline work:
  // - deferred scenario starts are drained only after the clean baseline has been
  //   emitted and latched at the Godot boundary;
  // - start_timeline_scenario_for_host() arms/schedules provider-owned events but
  //   does not itself advance or pump synthetic time;
  // - this dt=0 pump executes events already due at the current provider virtual
  //   time (notably at_ns=0 startup events) without depending on a later Godot
  //   process_frame tick.
  // This dispatches due timeline work only. It must not force publication here;
  // descendant realization remains provider-fact-driven through callbacks, core
  // fact dispatch, and the normal coalesced snapshot publish path.
  if (!broker->try_tick_virtual_time(0)) {
    WARN_PRINT("CamBANGServer: deferred scenario current-time pump was unavailable after successful scenario start; publication remains provider-fact-driven.");
  }

  return godot::OK;
}

godot::Error CamBANGServer::start_scenario() {
  if (is_public_boundary_ready_()) {
    return _start_scenario_now_();
  }

  if (!is_synthetic_timeline_session_active_()) {
    ERR_PRINT("CamBANGServer: start_scenario requires synthetic timeline mode.");
    return godot::ERR_UNAVAILABLE;
  }
  if (!scenario_config_staged_for_session_) {
    ERR_PRINT("CamBANGServer: start_scenario rejected because no scenario is staged for this session.");
    return godot::ERR_INVALID_PARAMETER;
  }

  // Narrow startup exception: before the first Godot-visible baseline,
  // start_scenario() records only high-level playback intent. The intent is
  // scoped to the active boundary session and is not a general pre-baseline
  // runtime command queue for devices, streams, captures, or timeline advances.
  pending_scenario_start_after_baseline_ = true;
  pending_scenario_start_session_id_ = active_session_id_;
  godot::UtilityFunctions::print(
      "CamBANGServer: start_scenario accepted during startup; playback will begin after baseline state_published(gen, 0, 0).");
  return godot::OK;
}

void CamBANGServer::_drain_pending_scenario_start_after_baseline_() {
  if (!pending_scenario_start_after_baseline_) {
    return;
  }
  if (pending_scenario_start_session_id_ != active_session_id_) {
    // Discard stale startup playback intent from a prior stop/start boundary.
    _clear_pending_scenario_start_();
    return;
  }
  if (!is_public_boundary_ready_()) {
    return;
  }

  // Actual provider playback starts only after the baseline snapshot is visible
  // at the Godot boundary. Scenario effects after this point must still arrive
  // through provider facts and the normal coalesced publication path.
  const godot::Error rc = _start_scenario_now_();
  const bool apply_pending_pause = pending_timeline_pause_after_scenario_start_;
  const bool pending_pause_value = pending_timeline_pause_value_;
  _clear_pending_scenario_start_();

  if (rc != godot::OK) {
    ERR_PRINT(godot::vformat(
        "CamBANGServer: deferred scenario playback failed after baseline; err=%d reason=%s.",
        static_cast<int>(rc),
        godot_error_to_cstr(rc)));
    return;
  }

  if (apply_pending_pause) {
    ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
    if (!broker) {
      ERR_PRINT("CamBANGServer: deferred scenario pause could not be applied after baseline; synthetic timeline provider is unavailable.");
      return;
    }
    const ProviderResult pr = broker->set_timeline_scenario_paused_for_host(pending_pause_value);
    if (!pr.ok()) {
      ERR_PRINT(godot::vformat(
          "CamBANGServer: deferred scenario pause failed after baseline; provider_error=%s.",
          cambang::to_string(pr.code)));
    }
  }
}

godot::Error CamBANGServer::stop_scenario() {
  if (pending_scenario_start_after_baseline_ && pending_scenario_start_session_id_ == active_session_id_) {
    _clear_pending_scenario_start_();
    return godot::OK;
  }
  if (!is_public_boundary_ready_()) {
    ERR_PRINT("CamBANGServer: stop_scenario rejected because runtime baseline is not observable yet.");
    return godot::ERR_BUSY;
  }
  if (!is_synthetic_timeline_session_active_()) {
    ERR_PRINT("CamBANGServer: stop_scenario requires synthetic timeline mode.");
    return godot::ERR_UNAVAILABLE;
  }
  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  return map_provider_result_to_godot_error(broker->stop_timeline_scenario_for_host());
}

godot::Error CamBANGServer::set_timeline_paused(bool paused) {
  if (!is_synthetic_timeline_session_active_()) {
    ERR_PRINT("CamBANGServer: set_timeline_paused requires synthetic timeline mode.");
    return godot::ERR_UNAVAILABLE;
  }
  if (!is_public_boundary_ready_()) {
    if (pending_scenario_start_after_baseline_ && pending_scenario_start_session_id_ == active_session_id_) {
      pending_timeline_pause_after_scenario_start_ = true;
      pending_timeline_pause_value_ = paused;
      return godot::OK;
    }
    ERR_PRINT("CamBANGServer: set_timeline_paused rejected because runtime baseline is not observable yet.");
    return godot::ERR_BUSY;
  }
  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  return map_provider_result_to_godot_error(broker->set_timeline_scenario_paused_for_host(paused));
}

godot::Error CamBANGServer::advance_timeline(uint64_t dt_ns) {
  if (!is_public_boundary_ready_()) {
    ERR_PRINT("CamBANGServer: advance_timeline rejected because runtime baseline is not observable yet; wait for state_published(gen, 0, 0) after start().");
    return godot::ERR_BUSY;
  }
  if (!is_synthetic_timeline_session_active_()) {
    ERR_PRINT("CamBANGServer: advance_timeline requires synthetic timeline mode.");
    return godot::ERR_UNAVAILABLE;
  }
  ProviderBroker* broker = dynamic_cast<ProviderBroker*>(provider_.get());
  return map_provider_result_to_godot_error(broker->advance_timeline_for_host(dt_ns));
}


void CamBANGServer::_refresh_timeline_teardown_trace_mode() {
  const bool strict_timeline_monitor =
      runtime_.is_running() && active_runtime_mode_ == RuntimeMode::synthetic &&
      active_synthetic_role_ == SyntheticRole::Timeline &&
      !completion_gated_destructive_sequencing_enabled_;
  timeline_teardown_trace_set_enabled(timeline_trace_echo_enabled_ || strict_timeline_monitor);
}

void CamBANGServer::_handle_timeline_teardown_trace_line(const std::string& line) {
  const bool strict_timeline_monitor =
      runtime_.is_running() && active_runtime_mode_ == RuntimeMode::synthetic &&
      active_synthetic_role_ == SyntheticRole::Timeline &&
      !completion_gated_destructive_sequencing_enabled_;
  if (!strict_timeline_monitor || strict_scenario_unmet_logged_) {
    return;
  }

  if (!(line_contains_token(line, "[timeline_teardown][FAIL] DestroyStream") ||
        line_contains_token(line, "[timeline_teardown][FAIL] CloseDevice"))) {
    return;
  }

  godot::UtilityFunctions::push_warning(
      "[CamBANG][Synthetic][Strict] Scenario conditions unmet. Strict destructive sequencing could not be satisfied in-band for the active timeline scenario.");
  strict_scenario_unmet_logged_ = true;
}

bool CamBANGServer::_ensure_provider_attached_and_initialized(
    RuntimeMode mode,
    SyntheticRole synthetic_role,
    TimingDriver timing_driver) {
  if (!runtime_.is_running()) {
    return false;
  }

  // If we already have an attached provider, nothing to do.
  if (provider_ && runtime_.attached_provider() == provider_.get()) {
    return true;
  }

  // Fresh broker per start cycle (latched provider configuration).
  {
    auto broker = std::make_unique<ProviderBroker>();
    broker->set_synthetic_timeline_request_dispatch_hook(
        make_synthetic_timeline_request_dispatch_hook(runtime_));
    ProviderResult sr = broker->set_runtime_mode_requested(mode);
    if (!sr.ok()) {
      ERR_PRINT(godot::vformat(
          "CamBANGServer: provider_mode='%s' is not supported in this build.",
          mode_to_cstr(mode)));
      return false;
    }
    ProviderResult role_req = broker->set_synthetic_role_requested(synthetic_role);
    if (!role_req.ok()) {
      ERR_PRINT("CamBANGServer: requested synthetic role configuration rejected by provider broker.");
      return false;
    }
    ProviderResult timing_req = broker->set_synthetic_timing_driver_requested(timing_driver);
    if (!timing_req.ok()) {
      ERR_PRINT("CamBANGServer: requested synthetic timing_driver configuration rejected by provider broker.");
      return false;
    }
    if (mode == RuntimeMode::synthetic) {
      if (!apply_synthetic_producer_output_form_cmdline_to_project_setting()) {
        ERR_PRINT("CamBANGServer: invalid duplicate or unsupported Synthetic producer output-form maintainer setting.");
        return false;
      }
      if (!apply_synthetic_stream_capability_downgrade_cmdline_to_project_setting()) {
        ERR_PRINT("CamBANGServer: invalid duplicate or unsupported Synthetic stream capability downgrade maintainer setting.");
        return false;
      }
      if (!apply_synthetic_capture_capability_downgrade_cmdline_to_project_setting()) {
        ERR_PRINT("CamBANGServer: invalid duplicate or unsupported Synthetic capture capability downgrade maintainer setting.");
        return false;
      }
      SyntheticProducerOutputFormMode producer_output_form_mode = SyntheticProducerOutputFormMode::Auto;
      if (!read_synthetic_producer_output_form_project_setting(producer_output_form_mode)) {
        ERR_PRINT("CamBANGServer: invalid Synthetic producer output-form maintainer project setting.");
        return false;
      }
      std::vector<SyntheticStreamCapabilityDowngradeCondition>
          stream_capability_downgrade_conditions{};
      if (!read_synthetic_stream_capability_downgrade_project_setting(
              stream_capability_downgrade_conditions)) {
        ERR_PRINT("CamBANGServer: invalid Synthetic stream capability downgrade maintainer project setting.");
        return false;
      }
      std::vector<SyntheticCaptureCapabilityDowngradeCondition>
          capture_capability_downgrade_conditions{};
      if (!read_synthetic_capture_capability_downgrade_project_setting(
              capture_capability_downgrade_conditions)) {
        ERR_PRINT("CamBANGServer: invalid Synthetic capture capability downgrade maintainer project setting.");
        return false;
      }
      ProviderResult output_form_req =
          broker->set_synthetic_producer_output_form_mode_requested(producer_output_form_mode);
      if (!output_form_req.ok()) {
        ERR_PRINT("CamBANGServer: requested Synthetic producer output-form configuration rejected by provider broker.");
        return false;
      }
      ProviderResult stream_downgrade_req =
          broker->set_synthetic_stream_capability_downgrade_conditions_requested(
              std::move(stream_capability_downgrade_conditions));
      if (!stream_downgrade_req.ok()) {
        ERR_PRINT("CamBANGServer: requested Synthetic stream capability downgrade configuration rejected by provider broker.");
        return false;
      }
      ProviderResult capture_downgrade_req =
          broker->set_synthetic_capture_capability_downgrade_conditions_requested(
              std::move(capture_capability_downgrade_conditions));
      if (!capture_downgrade_req.ok()) {
        ERR_PRINT("CamBANGServer: requested Synthetic capture capability downgrade configuration rejected by provider broker.");
        return false;
      }
    }
    const bool reconciliation_applicable =
        mode == RuntimeMode::synthetic &&
        synthetic_role == SyntheticRole::Timeline &&
        timing_driver == TimingDriver::VirtualTime;
    if (reconciliation_applicable) {
      ProviderResult recon_req = broker->set_synthetic_timeline_reconciliation_requested(
          completion_gated_destructive_sequencing_enabled_
              ? TimelineReconciliation::CompletionGated
              : TimelineReconciliation::Strict);
      if (!recon_req.ok()) {
        ERR_PRINT("CamBANGServer: requested synthetic timeline_reconciliation configuration rejected by provider broker.");
        return false;
      }
    }
    provider_ = std::move(broker);
  }
  ProviderResult pr = provider_->initialize(runtime_.provider_callbacks());
  if (!pr.ok()) {
    provider_.reset();
    ERR_PRINT("CamBANGServer: provider initialize failed.");
    return false;
  }

  runtime_.attach_provider(provider_.get());

  // Banner 1: Godot-facing provider selection (effective runtime attachment).
  const ProviderBannerInfo bi = describe_provider_for_banner(provider_.get());
  godot::UtilityFunctions::print("[CamBANG] provider selected: ", bi.provider_mode, " / ", bi.provider_name);

  return true;
}
void CamBANGServer::_bind_methods() {
  godot::ClassDB::bind_method(
      godot::D_METHOD("start", "provider_kind", "role", "timing_driver", "timeline_reconciliation"),
      &CamBANGServer::start,
      DEFVAL(godot::Variant()),
      DEFVAL(godot::Variant()),
      DEFVAL(godot::Variant()),
      DEFVAL(godot::Variant()));
  godot::ClassDB::bind_method(godot::D_METHOD("stop"), &CamBANGServer::stop);
  godot::ClassDB::bind_method(godot::D_METHOD("stop_and_quit", "exit_code"), &CamBANGServer::stop_and_quit, DEFVAL(0));
  godot::ClassDB::bind_method(godot::D_METHOD("is_running"), &CamBANGServer::is_running);
  godot::ClassDB::bind_method(godot::D_METHOD("get_active_provider_config"), &CamBANGServer::get_active_provider_config);
  godot::ClassDB::bind_method(godot::D_METHOD("get_provider_support"), &CamBANGServer::get_provider_support);
  godot::ClassDB::bind_method(godot::D_METHOD("select_builtin_scenario", "scenario_name"), &CamBANGServer::select_builtin_scenario);
  godot::ClassDB::bind_method(godot::D_METHOD("load_external_scenario", "json_text"), &CamBANGServer::load_external_scenario);
  godot::ClassDB::bind_method(godot::D_METHOD("start_scenario"), &CamBANGServer::start_scenario);
  godot::ClassDB::bind_method(godot::D_METHOD("stop_scenario"), &CamBANGServer::stop_scenario);
  godot::ClassDB::bind_method(godot::D_METHOD("set_timeline_paused", "paused"), &CamBANGServer::set_timeline_paused);
  godot::ClassDB::bind_method(godot::D_METHOD("advance_timeline", "dt_ns"), &CamBANGServer::advance_timeline);
  godot::ClassDB::bind_method(godot::D_METHOD("get_state_snapshot"), &CamBANGServer::get_state_snapshot);
  godot::ClassDB::bind_method(godot::D_METHOD("get_synthetic_metrics_snapshot"), &CamBANGServer::get_synthetic_metrics_snapshot);
  godot::ClassDB::bind_method(godot::D_METHOD("enumerate_devices"), &CamBANGServer::enumerate_devices);
  godot::ClassDB::bind_method(godot::D_METHOD("get_device_for_hardware_id", "hardware_id"), &CamBANGServer::get_device_for_hardware_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_device", "device_instance_id"), &CamBANGServer::get_device);
  godot::ClassDB::bind_method(godot::D_METHOD("get_rig", "rig_id"), &CamBANGServer::get_rig);
  godot::ClassDB::bind_method(godot::D_METHOD("get_stream_result_by_stream_id", "stream_id"), &CamBANGServer::get_stream_result_by_stream_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_result_by_id", "capture_id", "device_instance_id"), &CamBANGServer::get_capture_result_by_id);
  godot::ClassDB::bind_method(godot::D_METHOD("get_capture_result_set_by_id", "capture_id"), &CamBANGServer::get_capture_result_set_by_id);

  // Internal tick hook (connected to SceneTree.process_frame).
  godot::ClassDB::bind_method(godot::D_METHOD("_on_godot_process_frame"), &CamBANGServer::_on_godot_process_frame);

  BIND_CONSTANT(PROVIDER_KIND_PLATFORM_BACKED);
  BIND_CONSTANT(PROVIDER_KIND_SYNTHETIC);
  BIND_CONSTANT(SYNTHETIC_ROLE_NOMINAL);
  BIND_CONSTANT(SYNTHETIC_ROLE_TIMELINE);
  BIND_CONSTANT(TIMING_DRIVER_REAL_TIME);
  BIND_CONSTANT(TIMING_DRIVER_VIRTUAL_TIME);
  BIND_CONSTANT(TIMELINE_RECONCILIATION_COMPLETION_GATED);
  BIND_CONSTANT(TIMELINE_RECONCILIATION_STRICT);

  ADD_SIGNAL(godot::MethodInfo(
      "state_published",
      godot::PropertyInfo(godot::Variant::INT, "gen"),
      godot::PropertyInfo(godot::Variant::INT, "version"),
      godot::PropertyInfo(godot::Variant::INT, "topology_version")));
}

} // namespace cambang
