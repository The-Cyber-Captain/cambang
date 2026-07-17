// src/core/core_runtime.cpp

#include "core/core_runtime.h"

#include "core/camera_concurrency_adc.h"
#include "core/adc_camera_description.h"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <limits>
#include <memory>
#include <map>
#include <future>
#include <vector>

#include <utility>

#include "imaging/broker/banner_info.h"
#include "core/resource_aggregate_telemetry.h"
#include "imaging/api/timeline_teardown_trace.h"

namespace cambang {

namespace capture_latency_trace_diagnostics {
inline uint32_t capture_inflight() noexcept { return 0u; }
inline uint32_t active_capture_count() noexcept { return 0u; }
inline void note_capture_admitted(uint32_t) noexcept {}
inline void note_capture_finished() noexcept {}
inline void reset_trace_group_seen() noexcept {}
inline void print_trace_group_seen_summary() noexcept {}
inline void print_line(const char*) noexcept {}
} // namespace capture_latency_trace_diagnostics

namespace {

constexpr uint64_t kNsPerMs = 1000000ull;
constexpr uint64_t kCaptureObservationRetryDelayNs = 1'000'000ull;
constexpr uint64_t kCaptureRetainedPlanOrphanRetentionWindowNs =
    5ull * 1000ull * 1000ull * 1000ull;

bool is_valid_spec_patch_view(SpecPatchView patch) noexcept {
  return patch.size_bytes == 0 || patch.data != nullptr;
}

std::vector<uint8_t> copy_spec_patch_payload(SpecPatchView patch) {
  std::vector<uint8_t> out;
  if (patch.size_bytes == 0) {
    return out;
  }
  const auto* bytes = static_cast<const uint8_t*>(patch.data);
  out.assign(bytes, bytes + patch.size_bytes);
  return out;
}

CoreRuntime::RigTriggerOrchestrationResult make_rig_orchestration_result_base(
    uint64_t rig_id,
    uint64_t capture_id) {
  CoreRuntime::RigTriggerOrchestrationResult out{};
  out.rig_id = rig_id;
  out.capture_id = capture_id;
  return out;
}

CoreRuntime::RigTriggerOrchestrationResult make_rig_orchestration_preflight_failure(
    uint64_t rig_id,
    uint64_t capture_id,
    CoreRuntime::RigPreflightFailure failure) {
  CoreRuntime::RigTriggerOrchestrationResult out =
      make_rig_orchestration_result_base(rig_id, capture_id);
  out.failure = CoreRuntime::RigOrchestrationFailure::PreflightFailed;
  out.preflight_failure = failure;
  return out;
}

CoreRuntime::RigTriggerOrchestrationResult make_rig_orchestration_invalid_capture_id(
    uint64_t rig_id,
    uint64_t capture_id) {
  CoreRuntime::RigTriggerOrchestrationResult out =
      make_rig_orchestration_result_base(rig_id, capture_id);
  out.failure = CoreRuntime::RigOrchestrationFailure::InvalidCaptureId;
  return out;
}

CoreRuntime::RigTriggerOrchestrationResult make_rig_orchestration_admission_failure(
    uint64_t rig_id,
    uint64_t capture_id,
    CoreRuntime::RigCohortAdmissionFailure failure) {
  CoreRuntime::RigTriggerOrchestrationResult out =
      make_rig_orchestration_result_base(rig_id, capture_id);
  out.failure = CoreRuntime::RigOrchestrationFailure::AdmissionFailed;
  out.admission_failure = failure;
  return out;
}

CoreRuntime::RigTriggerOrchestrationResult make_rig_orchestration_submission_failure(
    const CoreRuntime::RigSubmissionResult& submitted) {
  CoreRuntime::RigTriggerOrchestrationResult out =
      make_rig_orchestration_result_base(submitted.rig_id, submitted.capture_id);
  out.failure = CoreRuntime::RigOrchestrationFailure::SubmissionFailed;
  out.submission_failure = submitted.failure;
  out.submitted_count = submitted.submitted_count;
  out.failed_index = submitted.failed_index;
  out.failed_device_instance_id = submitted.failed_device_instance_id;
  out.provider_error_code = submitted.provider_error_code;
  return out;
}

CoreRuntime::RigTriggerOrchestrationResult make_rig_orchestration_success(
    uint64_t rig_id,
    uint64_t capture_id,
    size_t submitted_count) {
  CoreRuntime::RigTriggerOrchestrationResult out =
      make_rig_orchestration_result_base(rig_id, capture_id);
  out.ok = true;
  out.failure = CoreRuntime::RigOrchestrationFailure::None;
  out.submitted_count = submitted_count;
  return out;
}

CoreRuntime::RigPreflightResult make_rig_preflight_result_base(
    uint64_t rig_id) {
  CoreRuntime::RigPreflightResult out{};
  out.rig_id = rig_id;
  return out;
}

CoreRuntime::RigPreflightResult make_rig_preflight_failure(
    uint64_t rig_id,
    CoreRuntime::RigPreflightFailure failure,
    size_t failure_member_index = 0,
    std::string failure_hardware_id = {},
    uint64_t failure_device_instance_id = 0) {
  CoreRuntime::RigPreflightResult out = make_rig_preflight_result_base(rig_id);
  out.failure = failure;
  out.failure_member_index = failure_member_index;
  out.failure_hardware_id = std::move(failure_hardware_id);
  out.failure_device_instance_id = failure_device_instance_id;
  return out;
}

CoreRuntime::RigPreflightResult make_rig_preflight_success(
    uint64_t rig_id,
    std::vector<CoreRuntime::RigPreflightParticipant> participants) {
  CoreRuntime::RigPreflightResult out = make_rig_preflight_result_base(rig_id);
  out.ok = true;
  out.failure = CoreRuntime::RigPreflightFailure::None;
  out.participants = std::move(participants);
  return out;
}

CoreRuntime::RigAdmittedRequestBundle make_rig_admitted_result_base(
    uint64_t rig_id,
    uint64_t capture_id) {
  CoreRuntime::RigAdmittedRequestBundle out{};
  out.rig_id = rig_id;
  out.capture_id = capture_id;
  return out;
}

CoreRuntime::RigAdmittedRequestBundle make_rig_admitted_failure(
    uint64_t rig_id,
    uint64_t capture_id,
    CoreRuntime::RigCohortAdmissionFailure failure) {
  CoreRuntime::RigAdmittedRequestBundle out =
      make_rig_admitted_result_base(rig_id, capture_id);
  out.failure = failure;
  return out;
}

CoreRuntime::RigAdmittedRequestBundle make_rig_admitted_success(
    uint64_t rig_id,
    uint64_t capture_id,
    std::vector<CoreRuntime::RigAdmittedParticipantRequest> participants) {
  CoreRuntime::RigAdmittedRequestBundle out =
      make_rig_admitted_result_base(rig_id, capture_id);
  out.ok = true;
  out.failure = CoreRuntime::RigCohortAdmissionFailure::None;
  out.participants = std::move(participants);
  return out;
}

CoreRuntime::RigSubmissionResult make_rig_submission_result_base(
    uint64_t rig_id,
    uint64_t capture_id) {
  CoreRuntime::RigSubmissionResult out{};
  out.rig_id = rig_id;
  out.capture_id = capture_id;
  return out;
}

CoreRuntime::RigSubmissionResult make_rig_submission_failure(
    uint64_t rig_id,
    uint64_t capture_id,
    CoreRuntime::RigSubmissionFailure failure) {
  CoreRuntime::RigSubmissionResult out =
      make_rig_submission_result_base(rig_id, capture_id);
  out.failure = failure;
  return out;
}

CoreRuntime::RigSubmissionResult make_rig_submission_trigger_failed(
    uint64_t rig_id,
    uint64_t capture_id,
    size_t failed_index,
    uint64_t failed_device_instance_id,
    uint32_t provider_error_code) {
  CoreRuntime::RigSubmissionResult out =
      make_rig_submission_failure(
          rig_id, capture_id, CoreRuntime::RigSubmissionFailure::TriggerFailed);
  out.failed_index = failed_index;
  out.failed_device_instance_id = failed_device_instance_id;
  out.provider_error_code = provider_error_code;
  return out;
}

CoreRuntime::RigSubmissionResult make_rig_submission_provider_unavailable(
    uint64_t rig_id,
    uint64_t capture_id,
    uint32_t provider_error_code) {
  CoreRuntime::RigSubmissionResult out =
      make_rig_submission_failure(
          rig_id, capture_id, CoreRuntime::RigSubmissionFailure::ProviderUnavailable);
  out.provider_error_code = provider_error_code;
  return out;
}

CoreRuntime::RigSubmissionResult make_rig_submission_success(
    uint64_t rig_id,
    uint64_t capture_id,
    size_t submitted_count) {
  CoreRuntime::RigSubmissionResult out =
      make_rig_submission_result_base(rig_id, capture_id);
  out.ok = true;
  out.failure = CoreRuntime::RigSubmissionFailure::None;
  out.submitted_count = submitted_count;
  return out;
}

CoreRetainedProductionPlan make_retained_plan(
    CoreProductionPostureShape posture) noexcept {
  CoreRetainedProductionPlan plan{};
  plan.valid = true;
  plan.posture = posture;
  return plan;
}

bool same_retained_plan(CoreRetainedProductionPlan a,
                        CoreRetainedProductionPlan b) noexcept {
  return a.valid == b.valid && (!a.valid || a.posture == b.posture);
}

bool same_picture_config(const PictureConfig& a,
                         const PictureConfig& b) noexcept {
  return a.preset == b.preset &&
         a.seed == b.seed &&
         a.generator_fps_num == b.generator_fps_num &&
         a.generator_fps_den == b.generator_fps_den &&
         a.overlay_frame_index_offsets == b.overlay_frame_index_offsets &&
         a.overlay_moving_bar == b.overlay_moving_bar &&
         a.solid_r == b.solid_r &&
         a.solid_g == b.solid_g &&
         a.solid_b == b.solid_b &&
         a.solid_a == b.solid_a &&
         a.checker_size_px == b.checker_size_px;
}

bool same_bundle_members(const CaptureStillImageBundle& a,
                        const CaptureStillImageBundle& b) noexcept {
  if (a.members.size() != b.members.size()) {
    return false;
  }
  for (size_t i = 0; i < a.members.size(); ++i) {
    const auto& am = a.members[i];
    const auto& bm = b.members[i];
    if (am.image_member_index != bm.image_member_index ||
        am.role != bm.role ||
        am.intended_exposure_compensation_milli_ev !=
            bm.intended_exposure_compensation_milli_ev) {
      return false;
    }
  }
  return true;
}

bool same_backing_capabilities(
    ProducerBackingCapabilities a,
    ProducerBackingCapabilities b) noexcept {
  return a.cpu_backed_available == b.cpu_backed_available &&
         a.gpu_backed_available == b.gpu_backed_available &&
         a.gpu_with_cpu_sidecar_available ==
             b.gpu_with_cpu_sidecar_available;
}

size_t retained_plan_evidence_index(CoreProductionPostureShape posture) noexcept {
  switch (posture) {
    case CoreProductionPostureShape::CpuPrimary:
      return 0u;
    case CoreProductionPostureShape::GpuPrimaryNoCpuSidecar:
      return 1u;
    case CoreProductionPostureShape::GpuPrimaryWithCpuSidecar:
      return 2u;
  }
  return 0u;
}

uint64_t saturating_add_u64(uint64_t a, uint64_t b) noexcept {
  if (a > std::numeric_limits<uint64_t>::max() - b) {
    return std::numeric_limits<uint64_t>::max();
  }
  return a + b;
}

} // namespace

bool CoreRuntime::infer_stream_result_posture_shape_(
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

bool CoreRuntime::infer_capture_member_posture_shape_(
    const CoreCaptureResultData::ImageMemberData& member,
    CoreProductionPostureShape& out) noexcept {
  if (member.payload_kind == ResultPayloadKind::GPU_SURFACE &&
      member.retained_gpu_backing) {
    out = member.access_posture.has_retained_cpu_payload
              ? CoreProductionPostureShape::GpuPrimaryWithCpuSidecar
              : CoreProductionPostureShape::GpuPrimaryNoCpuSidecar;
    return true;
  }
  if (member.payload_kind == ResultPayloadKind::CPU_PACKED) {
    out = CoreProductionPostureShape::CpuPrimary;
    return true;
  }
  return false;
}

void CoreRuntime::fill_stream_observation_identity_(
    MeasuredPlanEvidence& evidence,
    const SharedStreamResultData& result,
    CoreProductionPostureShape observed_posture) noexcept {
  evidence.has_observed_posture = true;
  evidence.observed_posture = observed_posture;
  evidence.observed_access_posture_id = result->access_posture.posture_id;
  evidence.observed_stream_id = result->stream_id;
  evidence.observed_capture_id = 0;
  evidence.observed_acquisition_session_id = 0;
  evidence.observed_image_member_index = 0;
  evidence.observed_payload_kind = result->payload_kind;
  evidence.observed_has_retained_cpu_payload =
      result->access_posture.has_retained_cpu_payload;
  evidence.observed_has_retained_gpu_backing =
      result->access_posture.has_retained_gpu_backing;
  evidence.observed_gpu_materialization_available =
      result->access_posture.gpu_materialization_available;
  evidence.observed_gpu_materialization_requires_readback =
      result->access_posture.gpu_materialization_requires_readback;
}

void CoreRuntime::fill_capture_observation_identity_(
    MeasuredPlanEvidence& evidence,
    const SharedCaptureResultData& result,
    const CoreCaptureResultData::ImageMemberData& member,
    CoreProductionPostureShape observed_posture) noexcept {
  evidence.has_observed_posture = true;
  evidence.observed_posture = observed_posture;
  evidence.observed_access_posture_id = member.access_posture.posture_id;
  evidence.observed_stream_id = member.access_posture.stream_id;
  evidence.observed_capture_id = result->capture_id;
  evidence.observed_acquisition_session_id = result->acquisition_session_id;
  evidence.observed_image_member_index = member.image_member_index;
  evidence.observed_payload_kind = member.payload_kind;
  evidence.observed_has_retained_cpu_payload =
      member.access_posture.has_retained_cpu_payload;
  evidence.observed_has_retained_gpu_backing =
      member.access_posture.has_retained_gpu_backing;
  evidence.observed_gpu_materialization_available =
      member.access_posture.gpu_materialization_available;
  evidence.observed_gpu_materialization_requires_readback =
      member.access_posture.gpu_materialization_requires_readback;
}

bool CoreRuntime::same_observation_identity_(const MeasuredPlanEvidence& a,
                                             const MeasuredPlanEvidence& b) noexcept {
  return a.has_observed_posture &&
         b.has_observed_posture &&
         a.observed_posture == b.observed_posture &&
         a.observed_access_posture_id == b.observed_access_posture_id &&
         a.observed_stream_id == b.observed_stream_id &&
         a.observed_capture_id == b.observed_capture_id &&
         a.observed_acquisition_session_id == b.observed_acquisition_session_id &&
         a.observed_image_member_index == b.observed_image_member_index &&
         a.observed_payload_kind == b.observed_payload_kind &&
         a.observed_has_retained_cpu_payload ==
             b.observed_has_retained_cpu_payload &&
         a.observed_has_retained_gpu_backing ==
             b.observed_has_retained_gpu_backing &&
         a.observed_gpu_materialization_available ==
             b.observed_gpu_materialization_available &&
         a.observed_gpu_materialization_requires_readback ==
             b.observed_gpu_materialization_requires_readback;
}

bool CoreRuntime::same_capture_observation_family_(
    const MeasuredPlanEvidence& a,
    const MeasuredPlanEvidence& b) noexcept {
  return a.has_observed_posture &&
         b.has_observed_posture &&
         a.observed_capture_id != 0 &&
         b.observed_capture_id != 0 &&
         a.observed_posture == b.observed_posture &&
         a.observed_access_posture_id == b.observed_access_posture_id &&
         a.observed_stream_id == b.observed_stream_id &&
         a.observed_capture_id == b.observed_capture_id &&
         a.observed_acquisition_session_id == b.observed_acquisition_session_id &&
         a.observed_payload_kind == b.observed_payload_kind &&
         a.observed_has_retained_cpu_payload ==
             b.observed_has_retained_cpu_payload &&
         a.observed_has_retained_gpu_backing ==
             b.observed_has_retained_gpu_backing &&
         a.observed_gpu_materialization_available ==
             b.observed_gpu_materialization_available &&
         a.observed_gpu_materialization_requires_readback ==
             b.observed_gpu_materialization_requires_readback;
}

bool CoreRuntime::capture_member_matches_required_bundle_(
    const CaptureStillImageBundle& bundle,
    const CoreCaptureResultData::ImageMemberData& member) noexcept {
  if (member.image_member_index >= bundle.members.size()) {
    return false;
  }
  const CaptureStillImageMember& required =
      bundle.members[member.image_member_index];
  if (required.image_member_index != member.image_member_index) {
    return false;
  }
  const CoreCaptureResultData::ImageMemberRole expected_role =
      required.image_member_index == 0
          ? CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED
          : CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
  return member.role == expected_role;
}

void CoreRuntime::recompute_capture_materialization_aggregate_(
    MeasuredPlanEvidence& evidence,
    const CaptureStillImageBundle& bundle) noexcept {
  evidence.provisional_to_image = ResultCapability::UNSUPPORTED;
  evidence.has_materialization_elapsed_ns = false;
  evidence.materialization_elapsed_ns = 0;
  evidence.required_capture_member_count =
      static_cast<uint32_t>(bundle.members.size());
  evidence.observed_capture_member_count = 0;
  evidence.materialized_capture_member_count = 0;
  evidence.has_first_missing_required_capture_member_index = false;
  evidence.first_missing_required_capture_member_index = 0;
  evidence.capture_evidence_incomplete_reason =
      CaptureEvidenceIncompleteReason::None;
  evidence.has_normalized_cost_units = false;
  evidence.normalized_cost_units = 0;

  uint32_t dominant_member_index = evidence.observed_image_member_index;
  bool have_any_observation = false;
  bool have_any_provisional_to_image = false;
  bool have_all_required_materialization = true;
  bool have_all_required_supported_materialization = true;
  bool saw_required_member_missing_materialization = false;
  bool saw_required_member_unsupported = false;

  auto note_first_missing_required_member = [&](uint32_t member_index) {
    if (!evidence.has_first_missing_required_capture_member_index) {
      evidence.has_first_missing_required_capture_member_index = true;
      evidence.first_missing_required_capture_member_index = member_index;
    }
  };

  for (const CaptureStillImageMember& required : bundle.members) {
    if (required.image_member_index >=
        evidence.capture_member_materialization.size()) {
      have_all_required_materialization = false;
      have_all_required_supported_materialization = false;
      saw_required_member_missing_materialization = true;
      note_first_missing_required_member(required.image_member_index);
      continue;
    }

    const auto& member_evidence =
        evidence.capture_member_materialization[required.image_member_index];
    if (!member_evidence.observed) {
      have_all_required_materialization = false;
      have_all_required_supported_materialization = false;
      saw_required_member_missing_materialization = true;
      note_first_missing_required_member(required.image_member_index);
      continue;
    }

    ++evidence.observed_capture_member_count;
    have_any_observation = true;
    if (!have_any_provisional_to_image ||
        static_cast<uint8_t>(member_evidence.provisional_to_image) >
            static_cast<uint8_t>(evidence.provisional_to_image)) {
      evidence.provisional_to_image = member_evidence.provisional_to_image;
      have_any_provisional_to_image = true;
    }

    if (member_evidence.has_materialization_elapsed_ns) {
      ++evidence.materialized_capture_member_count;
      if (!evidence.has_materialization_elapsed_ns ||
          member_evidence.materialization_elapsed_ns >
              evidence.materialization_elapsed_ns) {
        evidence.has_materialization_elapsed_ns = true;
        evidence.materialization_elapsed_ns =
            member_evidence.materialization_elapsed_ns;
        dominant_member_index = required.image_member_index;
      }
    } else {
      have_all_required_materialization = false;
      saw_required_member_missing_materialization = true;
      note_first_missing_required_member(required.image_member_index);
    }

    if (member_evidence.has_normalized_cost_units) {
      if (!evidence.has_normalized_cost_units ||
          member_evidence.normalized_cost_units >
              evidence.normalized_cost_units) {
        evidence.has_normalized_cost_units = true;
        evidence.normalized_cost_units = member_evidence.normalized_cost_units;
      }
    }

    if (member_evidence.provisional_to_image == ResultCapability::UNSUPPORTED ||
        !member_evidence.has_materialization_elapsed_ns) {
      have_all_required_supported_materialization = false;
      if (member_evidence.provisional_to_image ==
          ResultCapability::UNSUPPORTED) {
        saw_required_member_unsupported = true;
        note_first_missing_required_member(required.image_member_index);
      }
    }
  }

  if (have_any_observation) {
    evidence.observed_image_member_index = dominant_member_index;
  }

  if (evidence.has_capture_ready_elapsed_ns &&
      have_all_required_materialization &&
      evidence.has_materialization_elapsed_ns) {
    evidence.has_total_elapsed_ns = true;
    evidence.total_elapsed_ns = saturating_add_u64(
        evidence.capture_ready_elapsed_ns,
        evidence.materialization_elapsed_ns);
  } else {
    evidence.has_total_elapsed_ns = false;
    evidence.total_elapsed_ns = 0;
  }

  evidence.capture_evidence_complete =
      evidence.has_capture_ready_elapsed_ns &&
      have_all_required_supported_materialization &&
      evidence.has_total_elapsed_ns;
  evidence.capture_evidence_accepted = evidence.capture_evidence_complete;
  if (bundle.members.empty()) {
    evidence.capture_evidence_incomplete_reason =
        CaptureEvidenceIncompleteReason::NoRequiredBundle;
  } else if (!evidence.has_capture_ready_elapsed_ns) {
    evidence.capture_evidence_incomplete_reason =
        CaptureEvidenceIncompleteReason::AwaitingCaptureReady;
  } else if (saw_required_member_unsupported) {
    evidence.capture_evidence_incomplete_reason =
        CaptureEvidenceIncompleteReason::RequiredMemberUnsupported;
  } else if (saw_required_member_missing_materialization) {
    evidence.capture_evidence_incomplete_reason =
        CaptureEvidenceIncompleteReason::AwaitingRequiredMemberMaterialization;
  } else {
    evidence.capture_evidence_incomplete_reason =
        CaptureEvidenceIncompleteReason::None;
  }
}

CoreBackingPlanCandidateEvidenceReport
CoreRuntime::build_candidate_evidence_report_(
    CoreRetainedProductionPlan candidate,
    const MeasuredPlanEvidence& evidence) noexcept {
  CoreBackingPlanCandidateEvidenceReport report{};
  report.candidate = candidate;
  report.observation_seen =
      evidence.observed_display_view || evidence.observed_to_image;
  report.evidence_complete =
      evidence.display_view_evidence_complete || evidence.capture_evidence_complete;
  report.evidence_accepted =
      evidence.display_view_evidence_accepted || evidence.capture_evidence_accepted;
  report.provisional_display_view = evidence.provisional_display_view;
  report.has_display_view_elapsed_ns = evidence.has_display_view_elapsed_ns;
  report.display_view_elapsed_ns = evidence.display_view_elapsed_ns;
  report.provisional_to_image = evidence.provisional_to_image;
  report.has_materialization_elapsed_ns =
      evidence.has_materialization_elapsed_ns;
  report.materialization_elapsed_ns = evidence.materialization_elapsed_ns;
  report.has_capture_ready_elapsed_ns =
      evidence.has_capture_ready_elapsed_ns;
  report.capture_ready_elapsed_ns = evidence.capture_ready_elapsed_ns;
  report.has_total_elapsed_ns = evidence.has_total_elapsed_ns;
  report.total_elapsed_ns = evidence.total_elapsed_ns;
  report.required_capture_member_count = evidence.required_capture_member_count;
  report.observed_capture_member_count = evidence.observed_capture_member_count;
  report.materialized_capture_member_count =
      evidence.materialized_capture_member_count;
  report.has_first_missing_required_capture_member_index =
      evidence.has_first_missing_required_capture_member_index;
  report.first_missing_required_capture_member_index =
      evidence.first_missing_required_capture_member_index;
  report.capture_evidence_incomplete_reason =
      evidence.capture_evidence_incomplete_reason;
  report.has_normalized_cost_units = evidence.has_normalized_cost_units;
  report.normalized_cost_units = evidence.normalized_cost_units;
  report.has_observed_posture = evidence.has_observed_posture;
  report.observed_posture = evidence.observed_posture;
  report.observed_access_posture_id = evidence.observed_access_posture_id;
  report.observed_stream_id = evidence.observed_stream_id;
  report.observed_capture_id = evidence.observed_capture_id;
  report.observed_acquisition_session_id =
      evidence.observed_acquisition_session_id;
  report.observed_image_member_index = evidence.observed_image_member_index;
  report.observed_payload_kind = evidence.observed_payload_kind;
  report.observed_has_retained_cpu_payload =
      evidence.observed_has_retained_cpu_payload;
  report.observed_has_retained_gpu_backing =
      evidence.observed_has_retained_gpu_backing;
  report.observed_gpu_materialization_available =
      evidence.observed_gpu_materialization_available;
  report.observed_gpu_materialization_requires_readback =
      evidence.observed_gpu_materialization_requires_readback;
  return report;
}

namespace {

struct RetainedPlanResetDecision {
  CoreRetainedProductionPlan requested{};
  CoreRetainedProductionPlan steady{};
  bool evaluation_active = false;
  uint8_t candidate_count = 0;
  CoreProductionPostureShape candidate_sequence[3]{};
};

RetainedPlanResetDecision build_retained_plan_reset_decision(
    BackingPlanEvaluationPrimaryFunction primary_function,
    const ProducerBackingCapabilities& caps,
    CoreRetainedProductionPlan preferred_requested) noexcept;

size_t build_viable_candidate_order(
    BackingPlanEvaluationPrimaryFunction primary_function,
    const ProducerBackingCapabilities& caps,
    CoreProductionPostureShape* out,
    size_t cap) noexcept {
  size_t count = 0;
  auto append = [&](CoreProductionPostureShape posture) {
    if (count < cap && caps.viable(posture)) {
      out[count++] = posture;
    }
  };

  // Ordering is only a probe/tie-break heuristic. The settled winning plan is
  // chosen from bounded parent-scoped evidence across all viable candidates.
  if (primary_function ==
      BackingPlanEvaluationPrimaryFunction::StreamDisplayView) {
    append(CoreProductionPostureShape::GpuPrimaryNoCpuSidecar);
    append(CoreProductionPostureShape::GpuPrimaryWithCpuSidecar);
    append(CoreProductionPostureShape::CpuPrimary);
    return count;
  }

  append(CoreProductionPostureShape::CpuPrimary);
  append(CoreProductionPostureShape::GpuPrimaryNoCpuSidecar);
  append(CoreProductionPostureShape::GpuPrimaryWithCpuSidecar);
  return count;
}

RetainedPlanResetDecision build_retained_plan_reset_decision(
    BackingPlanEvaluationPrimaryFunction primary_function,
    const ProducerBackingCapabilities& caps) noexcept {
  return build_retained_plan_reset_decision(
      primary_function, caps, CoreRetainedProductionPlan{});
}

RetainedPlanResetDecision build_retained_plan_reset_decision(
    BackingPlanEvaluationPrimaryFunction primary_function,
    const ProducerBackingCapabilities& caps,
    CoreRetainedProductionPlan preferred_requested) noexcept {
  RetainedPlanResetDecision decision{};
  CoreProductionPostureShape ordered[3]{};
  const size_t ordered_count =
      build_viable_candidate_order(primary_function, caps, ordered, 3u);
  if (ordered_count == 0u) {
    return decision;
  }
  if (preferred_requested.valid &&
      caps.viable(preferred_requested.posture)) {
    for (size_t i = 0; i < ordered_count; ++i) {
      if (ordered[i] != preferred_requested.posture) {
        continue;
      }
      for (size_t j = i; j > 0; --j) {
        ordered[j] = ordered[j - 1u];
      }
      ordered[0] = preferred_requested.posture;
      break;
    }
  }
  decision.requested = make_retained_plan(ordered[0]);
  decision.candidate_count = static_cast<uint8_t>(ordered_count);
  for (size_t i = 0; i < ordered_count; ++i) {
    decision.candidate_sequence[i] = ordered[i];
  }
  if (ordered_count == 1u) {
    decision.steady = decision.requested;
    return decision;
  }

  decision.evaluation_active = true;
  return decision;
}

const char* backing_plan_primary_function_name(
    BackingPlanEvaluationPrimaryFunction primary_function) noexcept {
  switch (primary_function) {
    case BackingPlanEvaluationPrimaryFunction::StreamDisplayView:
      return "stream_display_view";
    case BackingPlanEvaluationPrimaryFunction::CaptureReadyAndMaterialize:
      return "capture_ready_and_materialize";
  }
  return "unknown";
}


// Stage A command-fairness bounds: provider facts remain FIFO and non-dropping,
// but Core yields to pending request work after deterministic slices so sustained
// stream/provider-event production cannot starve public commands. When requests
// are already queued, use the smallest provider-fact service slice before the
// request turn to honor capture-over-stream command fairness promptly.
constexpr size_t kMaxProviderFactsPerCoreTurn = 64;
constexpr size_t kMaxProviderFactsBeforeRequestWhenRequestsPending = 1;

// BEGIN TEMPORARY CAPTURE LATENCY DIAGNOSTICS
uint64_t capture_latency_trace_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

#define capture_latency_trace_printf(...) ((void)0)
// END TEMPORARY CAPTURE LATENCY DIAGNOSTICS


class CoreRuntimeDiagnosticPhaseScope final {
public:
  CoreRuntimeDiagnosticPhaseScope(CoreThread& core_thread, CoreThread::DiagnosticPhase phase)
      : core_thread_(core_thread) {
    core_thread_.diagnostic_set_phase_from_core(phase);
  }
  ~CoreRuntimeDiagnosticPhaseScope() = default;

  CoreRuntimeDiagnosticPhaseScope(const CoreRuntimeDiagnosticPhaseScope&) = delete;
  CoreRuntimeDiagnosticPhaseScope& operator=(const CoreRuntimeDiagnosticPhaseScope&) = delete;

private:
  CoreThread& core_thread_;
};

void capture_latency_trace_emit_core_command_wait_context(
    const char* kind,
    uint64_t capture_id,
    uint64_t target_id,
    const char* target_label,
    uint64_t wait_us,
    const CoreThread::DiagnosticSnapshot& post_snapshot,
    const CoreThread::DiagnosticSnapshot& start_snapshot,
    size_t runtime_requests_depth_at_start,
    size_t runtime_provider_facts_depth_at_start) {
  capture_latency_trace_printf(
      "core_command_wait_context kind=%s capture_id=%llu %s=%llu wait_us=%llu "
      "phase_at_post=%s phase_age_at_post_us=%llu previous_phase_at_post=%s previous_phase_ended_before_post_us=%llu "
      "essential_depth_at_post=%llu command_depth_at_post=%llu ordinary_depth_at_post=%llu timer_requested_at_post=%u timer_running_at_post=%u "
      "phase_at_start=%s phase_age_at_start_us=%llu previous_phase_at_start=%s previous_phase_ended_before_start_us=%llu "
      "essential_depth_at_start=%llu command_depth_at_start=%llu ordinary_depth_at_start=%llu timer_requested_at_start=%u timer_running_at_start=%u "
      "runtime_request_depth_at_start=%llu runtime_provider_fact_depth_at_start=%llu",
      kind,
      static_cast<unsigned long long>(capture_id),
      target_label,
      static_cast<unsigned long long>(target_id),
      static_cast<unsigned long long>(wait_us),
      CoreThread::diagnostic_phase_name(post_snapshot.phase),
      static_cast<unsigned long long>(post_snapshot.phase_age_us),
      CoreThread::diagnostic_phase_name(post_snapshot.previous_phase),
      static_cast<unsigned long long>(post_snapshot.previous_phase_ended_before_us),
      static_cast<unsigned long long>(post_snapshot.essential_queue_depth),
      static_cast<unsigned long long>(post_snapshot.command_queue_depth),
      static_cast<unsigned long long>(post_snapshot.ordinary_queue_depth),
      post_snapshot.timer_requested ? 1u : 0u,
      post_snapshot.timer_running ? 1u : 0u,
      CoreThread::diagnostic_phase_name(start_snapshot.phase),
      static_cast<unsigned long long>(start_snapshot.phase_age_us),
      CoreThread::diagnostic_phase_name(start_snapshot.previous_phase),
      static_cast<unsigned long long>(start_snapshot.previous_phase_ended_before_us),
      static_cast<unsigned long long>(start_snapshot.essential_queue_depth),
      static_cast<unsigned long long>(start_snapshot.command_queue_depth),
      static_cast<unsigned long long>(start_snapshot.ordinary_queue_depth),
      start_snapshot.timer_requested ? 1u : 0u,
      start_snapshot.timer_running ? 1u : 0u,
      static_cast<unsigned long long>(runtime_requests_depth_at_start),
      static_cast<unsigned long long>(runtime_provider_facts_depth_at_start));
}

// Stage B.1 provider-fact classification. Temporary diagnostics above remain
// in place; this helper implements the existing capture-over-stream policy at
// the provider-fact integration seam without changing public/provider APIs.
enum class ProviderFactClass : uint8_t {
  CriticalNonLossy = 0,
  RigCaptureCritical,
  DeviceCaptureCritical,
  RepeatingStreamFrame,
  OtherStream,
  UnknownNonLossy,
};

struct ProviderFactSummary {
  ProviderFactClass fact_class = ProviderFactClass::UnknownNonLossy;
  ProviderToCoreCommandType type = ProviderToCoreCommandType::INVALID;
  uint64_t capture_id = 0;
  uint64_t rig_id = 0;
  uint64_t device_instance_id = 0;
  uint64_t stream_id = 0;
  uint64_t acquisition_session_id = 0;
  uint32_t image_member_index = 0;
};

const char* provider_fact_class_name(ProviderFactClass fact_class) noexcept {
  switch (fact_class) {
    case ProviderFactClass::CriticalNonLossy: return "critical_non_lossy";
    case ProviderFactClass::RigCaptureCritical: return "rig_capture_critical";
    case ProviderFactClass::DeviceCaptureCritical: return "device_capture_critical";
    case ProviderFactClass::RepeatingStreamFrame: return "repeating_stream_frame";
    case ProviderFactClass::OtherStream: return "other_stream";
    case ProviderFactClass::UnknownNonLossy: return "unknown_non_lossy";
  }
  return "unknown_non_lossy";
}

bool provider_fact_is_capture_critical(ProviderFactClass fact_class) noexcept {
  return fact_class == ProviderFactClass::RigCaptureCritical ||
         fact_class == ProviderFactClass::DeviceCaptureCritical;
}

bool provider_fact_has_capture_id_for_priority(const ProviderToCoreCommand& cmd) {
  switch (cmd.type) {
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_STARTED:
      return std::get<CmdProviderCaptureStarted>(cmd.payload).capture_id != 0;
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_COMPLETED:
      return std::get<CmdProviderCaptureCompleted>(cmd.payload).capture_id != 0;
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_FAILED:
      return std::get<CmdProviderCaptureFailed>(cmd.payload).capture_id != 0;
    case ProviderToCoreCommandType::PROVIDER_FRAME:
      return std::get<CmdProviderFrame>(cmd.payload).frame.capture_id != 0;
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_IMAGE_FACTS:
      return std::get<CmdProviderCaptureImageFacts>(cmd.payload).capture_id != 0;
    default:
      return false;
  }
}

ProviderFactClass provider_capture_fact_class(uint64_t capture_id,
                                              const CoreCaptureCohortRegistry& capture_cohorts) noexcept {
  if (capture_id == 0) {
    return ProviderFactClass::UnknownNonLossy;
  }
  return capture_cohorts.contains(capture_id)
      ? ProviderFactClass::RigCaptureCritical
      : ProviderFactClass::DeviceCaptureCritical;
}

ProviderFactSummary summarize_provider_fact(const ProviderToCoreCommand& cmd,
                                            const CoreCaptureCohortRegistry& capture_cohorts) {
  ProviderFactSummary out{};
  out.type = cmd.type;

  switch (cmd.type) {
    case ProviderToCoreCommandType::PROVIDER_DEVICE_OPENED: {
      const auto& p = std::get<CmdProviderDeviceOpened>(cmd.payload);
      out.device_instance_id = p.device_instance_id;
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_DEVICE_CLOSED: {
      const auto& p = std::get<CmdProviderDeviceClosed>(cmd.payload);
      out.device_instance_id = p.device_instance_id;
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_CREATED: {
      const auto& p = std::get<CmdProviderStreamCreated>(cmd.payload);
      out.stream_id = p.stream_id;
      out.fact_class = ProviderFactClass::OtherStream;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_DESTROYED: {
      const auto& p = std::get<CmdProviderStreamDestroyed>(cmd.payload);
      out.stream_id = p.stream_id;
      out.fact_class = ProviderFactClass::OtherStream;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_STARTED: {
      const auto& p = std::get<CmdProviderStreamStarted>(cmd.payload);
      out.stream_id = p.stream_id;
      out.fact_class = ProviderFactClass::OtherStream;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_STOPPED: {
      const auto& p = std::get<CmdProviderStreamStopped>(cmd.payload);
      out.stream_id = p.stream_id;
      out.fact_class = ProviderFactClass::OtherStream;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_STARTED: {
      const auto& p = std::get<CmdProviderCaptureStarted>(cmd.payload);
      out.capture_id = p.capture_id;
      out.device_instance_id = p.device_instance_id;
      out.fact_class = provider_capture_fact_class(p.capture_id, capture_cohorts);
      if (const auto cohort = capture_cohorts.find(p.capture_id)) {
        out.rig_id = cohort->rig_id;
      }
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_COMPLETED: {
      const auto& p = std::get<CmdProviderCaptureCompleted>(cmd.payload);
      out.capture_id = p.capture_id;
      out.device_instance_id = p.device_instance_id;
      out.fact_class = provider_capture_fact_class(p.capture_id, capture_cohorts);
      if (const auto cohort = capture_cohorts.find(p.capture_id)) {
        out.rig_id = cohort->rig_id;
      }
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_FAILED: {
      const auto& p = std::get<CmdProviderCaptureFailed>(cmd.payload);
      out.capture_id = p.capture_id;
      out.device_instance_id = p.device_instance_id;
      out.fact_class = provider_capture_fact_class(p.capture_id, capture_cohorts);
      if (const auto cohort = capture_cohorts.find(p.capture_id)) {
        out.rig_id = cohort->rig_id;
      }
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_CAMERA_STATIC_FACTS: {
      const auto& p = std::get<CmdProviderCameraStaticFacts>(cmd.payload);
      out.device_instance_id = p.device_instance_id;
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_IMAGE_FACTS: {
      const auto& p = std::get<CmdProviderCaptureImageFacts>(cmd.payload);
      out.capture_id = p.capture_id;
      out.device_instance_id = p.device_instance_id;
      out.image_member_index = p.image_member_index;
      out.fact_class = provider_capture_fact_class(p.capture_id, capture_cohorts);
      if (const auto cohort = capture_cohorts.find(p.capture_id); cohort.has_value()) {
        out.rig_id = cohort->rig_id;
      }
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_FRAME: {
      const auto& p = std::get<CmdProviderFrame>(cmd.payload);
      out.capture_id = p.frame.capture_id;
      out.device_instance_id = p.frame.device_instance_id;
      out.stream_id = p.frame.stream_id;
      out.acquisition_session_id = p.frame.acquisition_session_id;
      out.image_member_index = p.frame.capture_image.image_member_index;
      if (p.frame.capture_id != 0) {
        out.fact_class = provider_capture_fact_class(p.frame.capture_id, capture_cohorts);
        if (const auto cohort = capture_cohorts.find(p.frame.capture_id)) {
          out.rig_id = cohort->rig_id;
        }
      } else if (p.frame.stream_id != 0) {
        out.fact_class = ProviderFactClass::RepeatingStreamFrame;
      } else {
        out.fact_class = ProviderFactClass::UnknownNonLossy;
      }
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_DEVICE_ERROR: {
      const auto& p = std::get<CmdProviderDeviceError>(cmd.payload);
      out.device_instance_id = p.device_instance_id;
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_ERROR: {
      const auto& p = std::get<CmdProviderStreamError>(cmd.payload);
      out.stream_id = p.stream_id;
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_CREATED: {
      const auto& p = std::get<CmdProviderNativeObjectCreated>(cmd.payload);
      out.device_instance_id = p.owner_device_instance_id;
      out.stream_id = p.owner_stream_id;
      out.rig_id = p.owner_rig_id;
      out.acquisition_session_id = p.owner_acquisition_session_id;
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_DESTROYED:
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    case ProviderToCoreCommandType::TIMER_TICK:
    case ProviderToCoreCommandType::INVALID:
      out.fact_class = ProviderFactClass::UnknownNonLossy;
      break;
  }

  return out;
}

bool promote_capture_fact_over_repeating_stream_prefix(
    std::deque<ProviderToCoreCommand>& provider_facts,
    const CoreCaptureCohortRegistry& capture_cohorts) {
  size_t skipped_stream_frames = 0;
  for (auto it = provider_facts.begin(); it != provider_facts.end(); ++it) {
    const ProviderFactSummary summary = summarize_provider_fact(*it, capture_cohorts);
    if (summary.fact_class == ProviderFactClass::RepeatingStreamFrame) {
      ++skipped_stream_frames;
      continue;
    }

    if (skipped_stream_frames != 0 && provider_fact_is_capture_critical(summary.fact_class)) {
      ProviderToCoreCommand promoted = std::move(*it);
      provider_facts.erase(it);
      provider_facts.push_front(std::move(promoted));
      capture_latency_trace_printf(
          "capture_fact_promoted_over_stream class=%s capture_id=%llu rig_id=%llu device_id=%llu stream_frames_skipped=%llu provider_fact_depth=%llu type=%u member=%u",
          provider_fact_class_name(summary.fact_class),
          static_cast<unsigned long long>(summary.capture_id),
          static_cast<unsigned long long>(summary.rig_id),
          static_cast<unsigned long long>(summary.device_instance_id),
          static_cast<unsigned long long>(skipped_stream_frames),
          static_cast<unsigned long long>(provider_facts.size()),
          static_cast<unsigned>(summary.type),
          static_cast<unsigned>(summary.image_member_index));
      return true;
    }

    return false;
  }

  return false;
}

bool has_newer_repeating_stream_frame_before_barrier(
    const std::deque<ProviderToCoreCommand>& provider_facts,
    const ProviderFactSummary& front_summary,
    const CoreCaptureCohortRegistry& capture_cohorts) {
  if (front_summary.fact_class != ProviderFactClass::RepeatingStreamFrame ||
      front_summary.stream_id == 0) {
    return false;
  }

  bool skipped_front = false;
  for (const ProviderToCoreCommand& cmd : provider_facts) {
    if (!skipped_front) {
      skipped_front = true;
      continue;
    }

    const ProviderFactSummary summary = summarize_provider_fact(cmd, capture_cohorts);
    if (summary.fact_class != ProviderFactClass::RepeatingStreamFrame) {
      return false;
    }
    if (summary.stream_id == front_summary.stream_id &&
        summary.acquisition_session_id == front_summary.acquisition_session_id) {
      return true;
    }
  }

  return false;
}

struct StreamFrameCoalesceResult {
  bool coalesced = false;
  bool stream_counters_changed = false;
};

StreamFrameCoalesceResult coalesce_front_repeating_stream_frame_if_superseded(
    std::deque<ProviderToCoreCommand>& provider_facts,
    const ProviderFactSummary& front_summary,
    const CoreCaptureCohortRegistry& capture_cohorts,
    CoreStreamRegistry& streams) {
  StreamFrameCoalesceResult out{};
  if (!has_newer_repeating_stream_frame_before_barrier(provider_facts, front_summary, capture_cohorts)) {
    return out;
  }

  ProviderToCoreCommand cmd = std::move(provider_facts.front());
  provider_facts.pop_front();
  auto& frame = std::get<CmdProviderFrame>(cmd.payload).frame;
  const uint64_t integrated_ts_ns = capture_latency_trace_now_ns();
  const bool received_counted = streams.on_frame_received(frame.stream_id, integrated_ts_ns);
  const bool dropped_counted = streams.on_frame_dropped(frame.stream_id);
  frame.release_now();
  global_resource_aggregate_telemetry().lease_released(make_framebuffer_lease_scoped_resource_telemetry_key(
      frame.stream_id,
      frame.acquisition_session_id));
  frame.release = nullptr;
  frame.release_user = nullptr;

  out.coalesced = true;
  out.stream_counters_changed = received_counted || dropped_counted;
  capture_latency_trace_printf(
      "stream_frame_coalesced stream_id=%llu acquisition_session_id=%llu provider_fact_depth_after=%llu received_counted=%u dropped_counted=%u delivered=0 publication_requested=1",
      static_cast<unsigned long long>(front_summary.stream_id),
      static_cast<unsigned long long>(front_summary.acquisition_session_id),
      static_cast<unsigned long long>(provider_facts.size()),
      received_counted ? 1u : 0u,
      dropped_counted ? 1u : 0u);
  return out;
}

constexpr uint64_t kProviderFactDispatchSlowThresholdUs = 5000;

void emit_provider_fact_dispatch_slow_if_needed(const ProviderFactSummary& summary,
                                                uint64_t dispatch_us,
                                                size_t provider_fact_depth_after_dispatch) {
  if (dispatch_us < kProviderFactDispatchSlowThresholdUs) {
    return;
  }
  capture_latency_trace_printf(
      "provider_fact_dispatch_slow class=%s type=%u capture_id=%llu rig_id=%llu device_id=%llu stream_id=%llu acquisition_session_id=%llu member=%u dispatch_us=%llu provider_fact_depth_after=%llu",
      provider_fact_class_name(summary.fact_class),
      static_cast<unsigned>(summary.type),
      static_cast<unsigned long long>(summary.capture_id),
      static_cast<unsigned long long>(summary.rig_id),
      static_cast<unsigned long long>(summary.device_instance_id),
      static_cast<unsigned long long>(summary.stream_id),
      static_cast<unsigned long long>(summary.acquisition_session_id),
      static_cast<unsigned>(summary.image_member_index),
      static_cast<unsigned long long>(dispatch_us),
      static_cast<unsigned long long>(provider_fact_depth_after_dispatch));
}

uint64_t warm_delay_ns(uint32_t warm_hold_ms) {
  if (warm_hold_ms == 0) {
    return 0;
  }
  if (warm_hold_ms >= (std::numeric_limits<uint64_t>::max() / kNsPerMs)) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(warm_hold_ms) * kNsPerMs;
}

bool seed_retained_device_still_profile_from_template(CoreDeviceRegistry& devices,
                                                      uint64_t device_instance_id,
                                                      const CaptureTemplate& capture_tmpl) {
  if (device_instance_id == 0) {
    return false;
  }
  if (const auto* rec = devices.find(device_instance_id)) {
    if (rec->capture_profile_version != 0 ||
        rec->capture_width != 0 ||
        rec->capture_height != 0 ||
        rec->capture_format != 0) {
      return false;
    }
  }

  const uint32_t format = capture_tmpl.profile.format_fourcc == 0
      ? FOURCC_RGBA
      : capture_tmpl.profile.format_fourcc;
  return devices.retain_capture_profile(device_instance_id,
                                         capture_tmpl.profile.width,
                                         capture_tmpl.profile.height,
                                         format,
                                         /*capture_profile_version=*/0);
}

} // namespace

bool CoreRuntime::plan_is_strictly_better_for_stream_(
    const MeasuredPlanEvidence& candidate,
    const MeasuredPlanEvidence* current_best) noexcept {
  if (!candidate.display_view_evidence_accepted ||
      candidate.provisional_display_view == ResultCapability::UNSUPPORTED ||
      !candidate.has_display_view_elapsed_ns) {
    return false;
  }
  if (!current_best) {
    return true;
  }
  if (!current_best->display_view_evidence_accepted ||
      !current_best->has_display_view_elapsed_ns ||
      current_best->provisional_display_view == ResultCapability::UNSUPPORTED) {
    return true;
  }
  return candidate.display_view_elapsed_ns < current_best->display_view_elapsed_ns;
}

bool CoreRuntime::plan_is_strictly_better_for_capture_(
    const MeasuredPlanEvidence& candidate,
    const MeasuredPlanEvidence* current_best) noexcept {
  if (!candidate.capture_evidence_accepted ||
      !candidate.capture_evidence_complete ||
      candidate.provisional_to_image == ResultCapability::UNSUPPORTED ||
      !candidate.has_capture_ready_elapsed_ns ||
      !candidate.has_materialization_elapsed_ns ||
      !candidate.has_total_elapsed_ns) {
    return false;
  }
  if (!current_best) {
    return true;
  }
  if (!current_best->capture_evidence_accepted ||
      !current_best->capture_evidence_complete ||
      !current_best->has_capture_ready_elapsed_ns ||
      !current_best->has_materialization_elapsed_ns ||
      !current_best->has_total_elapsed_ns) {
    return true;
  }
  return candidate.total_elapsed_ns < current_best->total_elapsed_ns;
}

CoreRuntime::RetainedPlanDecisionProvenance
CoreRuntime::build_decision_provenance_(
    const RetainedPlanEvaluatorState& state,
    CoreRetainedProductionPlan selected) noexcept {
  RetainedPlanDecisionProvenance provenance;
  provenance.device_instance_id = state.device_instance_id;
  provenance.acquisition_session_id = state.acquisition_session_id;
  provenance.valid = selected.valid;
  provenance.from_evaluation = true;
  provenance.primary_function = state.primary_function;
  provenance.completion_reason = state.completion_reason;
  provenance.capture_priming_seed_signature =
      state.capture_priming_seed_signature;
  provenance.selected = selected;
  provenance.candidate_count = state.candidate_count;
  for (uint8_t i = 0; i < state.candidate_count; ++i) {
    provenance.candidate_sequence[i] = state.candidate_sequence[i];
    provenance.evidence[i] =
        state.evidence[retained_plan_evidence_index(
            state.candidate_sequence[i].posture)];
  }
  return provenance;
}

CoreRuntime::RetainedPlanDecisionProvenance
CoreRuntime::build_non_evaluated_decision_provenance_(
    BackingPlanEvaluationPrimaryFunction primary_function,
    uint64_t device_instance_id,
    uint64_t acquisition_session_id,
    CoreRetainedProductionPlan requested,
    CoreRetainedProductionPlan steady,
    uint8_t candidate_count,
    const CoreRetainedProductionPlan* candidate_sequence) noexcept {
  RetainedPlanDecisionProvenance provenance;
  provenance.device_instance_id = device_instance_id;
  provenance.acquisition_session_id = acquisition_session_id;
  provenance.valid = requested.valid;
  provenance.primary_function = primary_function;
  provenance.selected = steady.valid ? steady : requested;
  provenance.candidate_count = candidate_count;
  for (uint8_t i = 0; i < candidate_count; ++i) {
    provenance.candidate_sequence[i] =
        candidate_sequence != nullptr ? candidate_sequence[i]
                                      : CoreRetainedProductionPlan{};
  }
  if (candidate_count == 1u && provenance.selected.valid) {
    provenance.completion_reason =
        BackingPlanEvaluationCompletionReason::SingleViableCandidate;
  }
  return provenance;
}

CoreRuntime::CapturePrimingSeedSignature
CoreRuntime::build_capture_priming_seed_signature_(
    uint64_t device_instance_id,
    const CaptureRequest& effective,
    ProducerBackingCapabilities runtime_backing_capabilities,
    ProducerBackingCapabilities parent_context_backing_capabilities) const {
  CapturePrimingSeedSignature signature{};
  if (const auto* rec = devices_.find(device_instance_id)) {
    signature.hardware_id = rec->hardware_id;
  }
  signature.width = effective.width;
  signature.height = effective.height;
  signature.format_fourcc = effective.format_fourcc;
  signature.picture = effective.picture;
  signature.still_image_bundle = effective.still_image_bundle;
  signature.runtime_backing_capabilities = runtime_backing_capabilities;
  signature.parent_context_backing_capabilities =
      parent_context_backing_capabilities;
  return signature;
}

bool CoreRuntime::try_find_capture_priming_seed_(
    const CapturePrimingSeedSignature& signature,
    CoreRetainedProductionPlan& out_selected) const {
  if (signature.hardware_id.empty()) {
    return false;
  }
  const auto it = capture_priming_seeds_.find(signature.hardware_id);
  if (it == capture_priming_seeds_.end()) {
    return false;
  }
  const CapturePrimingSeed& seed = it->second;
  if (seed.signature.width != signature.width ||
      seed.signature.height != signature.height ||
      seed.signature.format_fourcc != signature.format_fourcc ||
      !same_picture_config(seed.signature.picture, signature.picture) ||
      !same_bundle_members(seed.signature.still_image_bundle,
                           signature.still_image_bundle) ||
      !same_backing_capabilities(
          seed.signature.runtime_backing_capabilities,
          signature.runtime_backing_capabilities) ||
      !same_backing_capabilities(
          seed.signature.parent_context_backing_capabilities,
          signature.parent_context_backing_capabilities)) {
    return false;
  }
  out_selected = seed.selected;
  return out_selected.valid;
}

void CoreRuntime::remember_capture_priming_seed_(
    const CapturePrimingSeedSignature& signature,
    CoreRetainedProductionPlan selected) {
  if (signature.hardware_id.empty() || !selected.valid) {
    return;
  }
  capture_priming_seeds_[signature.hardware_id] =
      CapturePrimingSeed{signature, selected};
}

void CoreRuntime::release_capture_parent_priming_(
    uint64_t device_instance_id,
    const char* reason) {
  if (device_instance_id == 0) {
    return;
  }
  if (device_has_active_capture_evaluator_(device_instance_id)) {
    return;
  }
  const auto it = capture_parent_priming_states_.find(device_instance_id);
  if (it == capture_parent_priming_states_.end() ||
      !it->second.provider_hold_active) {
    capture_parent_priming_states_.erase(device_instance_id);
    return;
  }
  if (ICameraProvider* prov = provider_.load(std::memory_order_acquire)) {
    (void)prov->release_capture_parent_priming(device_instance_id);
  }
  capture_parent_priming_states_.erase(it);
}

bool CoreRuntime::device_has_any_stream_(uint64_t device_instance_id) const noexcept {
  if (device_instance_id == 0) {
    return false;
  }
  for (const auto& [stream_id, rec] : streams_.all()) {
    (void)stream_id;
    if (rec.device_instance_id == device_instance_id && rec.created) {
      return true;
    }
  }
  return false;
}

bool CoreRuntime::device_has_active_capture_evaluator_(
    uint64_t device_instance_id) const noexcept {
  if (device_instance_id == 0) {
    return false;
  }
  for (const auto& [key, state] : capture_retained_plan_evaluators_) {
    if (!state.active) {
      continue;
    }
    if (state.device_instance_id == device_instance_id ||
        (key.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming &&
         key.id == device_instance_id)) {
      return true;
    }
  }
  return false;
}

bool CoreRuntime::sync_capture_parent_priming_(
    uint64_t device_instance_id,
    const CaptureRequest& effective,
    const ProducerBackingCapabilities& runtime_backing_capabilities,
    const ProducerBackingCapabilities& parent_context_backing_capabilities) {
  assert(core_thread_.is_core_thread());
  const bool active_capture_evaluator =
      device_has_active_capture_evaluator_(device_instance_id);
  const uint64_t live_session_id =
      acquisition_sessions_.resolve_live_session_id_for_device(
          device_instance_id);
  const bool any_stream = device_has_any_stream_(device_instance_id);
  if (device_instance_id == 0 ||
      (!active_capture_evaluator && any_stream)) {
    return false;
  }
  if (!active_capture_evaluator &&
      live_session_id != 0) {
    return false;
  }

  const CapturePrimingSeedSignature signature =
      build_capture_priming_seed_signature_(
          device_instance_id,
          effective,
          runtime_backing_capabilities,
          parent_context_backing_capabilities);
  auto& state = capture_parent_priming_states_[device_instance_id];
  if (state.provider_hold_active &&
      state.signature.hardware_id == signature.hardware_id &&
      state.signature.width == signature.width &&
      state.signature.height == signature.height &&
      state.signature.format_fourcc == signature.format_fourcc &&
      same_picture_config(state.signature.picture, signature.picture) &&
      same_bundle_members(
          state.signature.still_image_bundle, signature.still_image_bundle) &&
      same_backing_capabilities(
          state.signature.runtime_backing_capabilities,
          signature.runtime_backing_capabilities) &&
      same_backing_capabilities(
          state.signature.parent_context_backing_capabilities,
          signature.parent_context_backing_capabilities)) {
    return true;
  }

  if (ICameraProvider* prov = provider_.load(std::memory_order_acquire)) {
    const ProviderResult primed = prov->sync_capture_parent_priming(effective);
    if (primed.ok()) {
      state.signature = signature;
      state.provider_hold_active = true;
      return true;
    }
    if (state.provider_hold_active) {
      release_capture_parent_priming_(
          device_instance_id,
          "refresh.non_evaluated_priming_terminal");
    } else {
      capture_parent_priming_states_.erase(device_instance_id);
    }
  }
  return false;
}

static bool display_demand_trace_enabled() noexcept {
  const char* v = std::getenv("CAMBANG_DEV_DISPLAY_DEMAND_TRACE");
  return v && v[0] != '\0' && v[0] != '0';
}

CoreRuntime::CoreRuntime()
    : core_thread_(),
      devices_(),
      streams_(),
      gen_counter_(0),
      current_gen_(0),
      version_(0),
      topology_version_(0),
      last_topology_sig_(0),
      dispatcher_(&streams_, &acquisition_sessions_, &devices_, &native_objects_, &current_gen_, [this]() -> uint64_t {
        return ns_since_epoch_();
      }, [this]() -> bool {
        return state_.load(std::memory_order_acquire) == CoreRuntimeState::LIVE;
      }),
      ingress_(&core_thread_, [this](ProviderToCoreCommand&& cmd) {
        // This lambda is executed ONLY on the core thread (posted by ingress).
        // Provider callbacks are "facts"; we enqueue them and process them before requests
        // on each core pump tick.
        assert(core_thread_.is_core_thread());
        enqueue_provider_fact(std::move(cmd));
      }, [this]() -> uint64_t {
        // Core monotonic timebase: ns since core epoch (session-relative).
        // IProviderCallbacks::core_monotonic_now_ns is documented safe to call from
        // any provider thread; ns_since_epoch_() honors that (epoch_steady_ns_ is atomic).
        return ns_since_epoch_();
      }, [this,
          demand_last = std::make_shared<std::map<uint64_t, bool>>(),
          demand_last_mu = std::make_shared<std::mutex>()](uint64_t stream_id) -> bool {
        // IProviderCallbacks::is_stream_display_demand_active is documented safe to
        // call from any provider thread. demand_last is trace-only bookkeeping (not
        // the functional return value) but was previously captured as bare mutable
        // lambda state with no synchronization, which is a real data race the moment
        // more than one provider thread calls this concurrently. Protect it explicitly
        // via a shared mutex rather than relying on single-caller-thread luck.
        const uint64_t now_ns = ns_since_epoch_();
        const auto state = result_store_.get_stream_display_demand_state(stream_id, now_ns);
        if (display_demand_trace_enabled()) {
          bool changed = false;
          {
            std::lock_guard<std::mutex> lock(*demand_last_mu);
            bool& prev = (*demand_last)[stream_id];
            changed = prev != state.active;
            if (changed) {
              prev = state.active;
            }
          }
          if (changed) {
            const char* reason = "none";
            if (state.reason == CoreResultStore::DisplayDemandReason::PERSISTENT_REFCOUNT) {
              reason = "persistent_refcount";
            } else if (state.reason == CoreResultStore::DisplayDemandReason::LEASE) {
              reason = "lease";
            }
            std::printf("[CamBANG][DemandTrace] demand_transition stream_id=%llu active=%d reason=%s refcount=%u\n",
                        static_cast<unsigned long long>(stream_id),
                        state.active ? 1 : 0,
                        reason,
                        state.refcount);
          }
        }
        return state.active;
      }) {
  dispatcher_.set_result_store(&result_store_);
  dispatcher_.set_capture_assembly_registry(&capture_assembly_registry_);
  dispatcher_.set_provider_camera_fact_state(&provider_camera_fact_state_);
  dispatcher_.set_capture_lifecycle_ingress_sink(
      [this](const CoreCaptureLifecycleIngressEvent& event) {
        note_capture_lifecycle_ingress_(event);
      });
}

CoreRuntime::~CoreRuntime() {
  stop();
}

SharedCaptureResultData CoreRuntime::get_capture_result(uint64_t capture_id, uint64_t device_instance_id) const {
  if (!capture_assembly_registry_.is_assembly_successful(
          capture_id, device_instance_id)) {
    return nullptr;
  }
  return result_store_.get_capture_result(capture_id, device_instance_id);
}

void CoreRuntime::report_stream_retained_to_image_observation(
    uint64_t stream_id,
    uint64_t posture_id,
    ResultCapability provisional_to_image,
    bool has_normalized_cost_units,
    uint64_t normalized_cost_units) {
  const CoreThread::PostResult pr = try_post(
      [this,
       stream_id,
       posture_id,
       provisional_to_image,
       has_normalized_cost_units,
       normalized_cost_units]() {
        handle_stream_retained_to_image_observation_(
            stream_id,
            posture_id,
            provisional_to_image,
            has_normalized_cost_units,
            normalized_cost_units);
      });
  (void)pr;
}

void CoreRuntime::report_stream_retained_display_view_observation(
    uint64_t stream_id,
    uint64_t posture_id,
    ResultCapability provisional_display_view,
    bool has_display_view_elapsed_ns,
    uint64_t display_view_elapsed_ns) {
  const CoreThread::PostResult pr = try_post(
      [this,
       stream_id,
       posture_id,
       provisional_display_view,
       has_display_view_elapsed_ns,
       display_view_elapsed_ns]() {
        handle_stream_retained_display_view_observation_(
            stream_id,
            posture_id,
            provisional_display_view,
            has_display_view_elapsed_ns,
            display_view_elapsed_ns);
      });
  (void)pr;
}

void CoreRuntime::report_capture_retained_to_image_observation(
    uint64_t device_instance_id,
    uint64_t capture_id,
    uint64_t acquisition_session_id_hint,
    uint64_t posture_id,
    ResultCapability provisional_to_image,
    bool has_materialization_elapsed_ns,
    uint64_t materialization_elapsed_ns,
    bool has_normalized_cost_units,
    uint64_t normalized_cost_units,
    uint32_t image_member_index) {
  constexpr uint8_t kCaptureObservationDeferredRetryCount = 4;
  const CoreThread::PostResult pr = try_post(
       [this,
        device_instance_id,
        capture_id,
        acquisition_session_id_hint,
        posture_id,
        provisional_to_image,
        has_materialization_elapsed_ns,
        materialization_elapsed_ns,
        has_normalized_cost_units,
        normalized_cost_units,
        image_member_index]() {
        handle_capture_retained_to_image_observation_(
            device_instance_id,
            capture_id,
            acquisition_session_id_hint,
            posture_id,
            provisional_to_image,
            has_materialization_elapsed_ns,
            materialization_elapsed_ns,
            has_normalized_cost_units,
            normalized_cost_units,
            image_member_index,
            kCaptureObservationDeferredRetryCount);
      });
  (void)pr;
}

uint64_t CoreRuntime::stream_backing_plan_evaluation_settle_delay_ns() const noexcept {
  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  return prov ? prov->stream_backing_plan_evaluation_settle_delay_ns() : 0;
}

uint64_t CoreRuntime::capture_backing_plan_evaluation_settle_delay_ns() const noexcept {
  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  return prov ? prov->capture_backing_plan_evaluation_settle_delay_ns() : 0;
}

bool CoreRuntime::build_effective_capture_request_without_retained_plan_(
    uint64_t device_instance_id,
    CaptureRequest& out) const {
  assert(core_thread_.is_core_thread());

  if (device_instance_id == 0) {
    return false;
  }
  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return false;
  }
  const bool supports_multi_image = prov->supports_multi_image_still_sequence();

  const CaptureTemplate tmpl = prov->capture_template();
  out = CaptureRequest{};
  out.device_instance_id = device_instance_id;
  out.rig_id = 0;
  out.width = tmpl.profile.width;
  out.height = tmpl.profile.height;
  out.format_fourcc =
      tmpl.profile.format_fourcc == 0 ? FOURCC_RGBA : tmpl.profile.format_fourcc;
  out.profile_version = 0;
  out.picture = tmpl.picture;
  out.still_image_bundle = make_default_metered_still_image_bundle();

  if (const auto* rec = devices_.find(device_instance_id)) {
    if (rec->capture_width > 0) {
      out.width = rec->capture_width;
    }
    if (rec->capture_height > 0) {
      out.height = rec->capture_height;
    }
    if (rec->capture_format != 0) {
      out.format_fourcc = rec->capture_format;
    }
    out.profile_version = rec->capture_profile_version;
    out.picture = rec->capture_picture;
    out.still_image_bundle = rec->capture_still_image_bundle;
  }

  if (!(out.width > 0 && out.height > 0)) {
    return false;
  }
  return is_valid_capture_still_image_bundle(
      out.still_image_bundle, supports_multi_image);
}

bool CoreRuntime::resolve_stream_backing_capabilities_(
    uint64_t device_instance_id,
    uint64_t stream_id,
    StreamIntent intent,
    const CaptureProfile& profile,
    const PictureConfig& picture,
    ProducerBackingCapabilities& runtime_backing_capabilities,
    ProducerBackingCapabilities& parent_context_backing_capabilities) {
  assert(core_thread_.is_core_thread());

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return false;
  }
  runtime_backing_capabilities =
      prov->stream_backing_capabilities(profile, picture);
  parent_context_backing_capabilities =
      prov->stream_parent_context_backing_capabilities(
          device_instance_id, stream_id, intent, profile, picture);
  return true;
}

bool CoreRuntime::resolve_capture_backing_capabilities_(
    uint64_t device_instance_id,
    const CaptureRequest& request,
    ProducerBackingCapabilities& runtime_backing_capabilities,
    ProducerBackingCapabilities& parent_context_backing_capabilities) {
  assert(core_thread_.is_core_thread());

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return false;
  }
  runtime_backing_capabilities = prov->capture_backing_capabilities(request);
  parent_context_backing_capabilities =
      prov->capture_parent_context_backing_capabilities(
          device_instance_id, request);
  return true;
}

bool CoreRuntime::refresh_stream_retained_plan_state_(
    uint64_t stream_id,
    bool apply_to_provider,
    bool requested_bump_access_posture_epoch) {
  assert(core_thread_.is_core_thread());

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  const CoreStreamRegistry::StreamRecord* rec = streams_.find(stream_id);
  if (!prov || !rec) {
    return false;
  }
  ProducerBackingCapabilities runtime_caps{};
  ProducerBackingCapabilities parent_context_caps{};
  if (!resolve_stream_backing_capabilities_(
          rec->device_instance_id,
          stream_id,
          rec->intent,
          rec->profile,
          rec->picture,
          runtime_caps,
          parent_context_caps)) {
    return false;
  }
  (void)streams_.set_backing_capabilities(
      stream_id, runtime_caps, parent_context_caps);
  const RetainedPlanResetDecision decision =
      build_retained_plan_reset_decision(
          BackingPlanEvaluationPrimaryFunction::StreamDisplayView,
          parent_context_caps);
  if (!decision.requested.valid) {
    (void)streams_.set_requested_retained_plan(
        stream_id, CoreRetainedProductionPlan{}, requested_bump_access_posture_epoch);
    (void)streams_.clear_steady_retained_plan(stream_id);
    stream_retained_plan_evaluators_.erase(stream_id);
    stream_retained_plan_decisions_.erase(stream_id);
    return false;
  }

  const CoreRetainedProductionPlan previous_requested =
      rec->requested_retained_plan;
  (void)streams_.set_requested_retained_plan(
      stream_id, decision.requested, requested_bump_access_posture_epoch);
  if (decision.steady.valid) {
    (void)streams_.set_steady_retained_plan(stream_id, decision.steady);
  } else {
    (void)streams_.clear_steady_retained_plan(stream_id);
  }

  if (decision.evaluation_active) {
    RetainedPlanEvaluatorState state;
    state.device_instance_id = rec->device_instance_id;
    state.primary_function =
        BackingPlanEvaluationPrimaryFunction::StreamDisplayView;
    state.completion_reason = BackingPlanEvaluationCompletionReason::None;
    state.active = true;
    state.candidate_count = decision.candidate_count;
    state.current_candidate_index = 0;
    for (uint8_t i = 0; i < decision.candidate_count; ++i) {
      state.candidate_sequence[i] = make_retained_plan(
          decision.candidate_sequence[i]);
    }
    stream_retained_plan_evaluators_[stream_id] = state;
    stream_retained_plan_decisions_.erase(stream_id);
  } else {
    stream_retained_plan_evaluators_.erase(stream_id);
    CoreRetainedProductionPlan candidate_sequence[3]{};
    for (uint8_t i = 0; i < decision.candidate_count; ++i) {
      candidate_sequence[i] = make_retained_plan(decision.candidate_sequence[i]);
    }
    RetainedPlanDecisionProvenance provenance =
        build_non_evaluated_decision_provenance_(
            BackingPlanEvaluationPrimaryFunction::StreamDisplayView,
            rec->device_instance_id,
            0,
            decision.requested,
            decision.steady,
            decision.candidate_count,
            candidate_sequence);
    stream_retained_plan_decisions_[stream_id] = provenance;
  }

  if (apply_to_provider &&
      !same_retained_plan(previous_requested, decision.requested)) {
    const bool ok = prov->update_stream_retained_production_plan(
               stream_id, decision.requested)
        .ok();
    (void)refresh_capture_retained_plan_state_(
        rec->device_instance_id,
        /*requested_bump_access_posture_epoch=*/false);
    return ok;
  }
  (void)refresh_capture_retained_plan_state_(
      rec->device_instance_id,
      /*requested_bump_access_posture_epoch=*/false);
  return true;
}

CoreRuntime::ResolvedCaptureRetainedPlanParent
CoreRuntime::resolve_capture_retained_plan_parent_(
    uint64_t device_instance_id,
    uint64_t preferred_acquisition_session_id) const {
  ResolvedCaptureRetainedPlanParent resolved{};
  resolved.device_instance_id = device_instance_id;
  resolved.key.kind = CaptureRetainedPlanParentKey::Kind::CapturePriming;
  resolved.key.id = device_instance_id;

  const auto priming_state_it =
      capture_parent_priming_states_.find(device_instance_id);
  const bool priming_hold_active =
      priming_state_it != capture_parent_priming_states_.end() &&
      priming_state_it->second.provider_hold_active;
  auto session_has_capture_activity =
      [](const CoreAcquisitionSessionRegistry::AcquisitionSessionEntry& entry)
          noexcept {
        return entry.captures_triggered != 0 ||
               entry.captures_completed != 0 ||
               entry.captures_failed != 0 ||
               entry.last_capture_id != 0;
      };

  uint64_t live_session_id = 0;
  uint32_t live_session_count = 0;
  bool live_session_has_capture_activity = false;
  for (const auto& [session_id, entry] : acquisition_sessions_.all()) {
    (void)session_id;
    if (entry.device_instance_id != device_instance_id ||
        entry.phase != CBLifecyclePhase::LIVE) {
      continue;
    }
    ++live_session_count;
    live_session_id = entry.acquisition_session_id;
    live_session_has_capture_activity =
        session_has_capture_activity(entry);
    if (live_session_count > 1u) {
      break;
    }
  }
  if (live_session_count == 1u &&
      live_session_id != 0 &&
      (!priming_hold_active || live_session_has_capture_activity)) {
    resolved.key.kind = CaptureRetainedPlanParentKey::Kind::AcquisitionSession;
    resolved.key.id = live_session_id;
    resolved.acquisition_session_id = live_session_id;
    resolved.provisional = false;
    return resolved;
  }
  if (preferred_acquisition_session_id != 0) {
    if (const auto* preferred =
            acquisition_sessions_.find(preferred_acquisition_session_id);
        preferred != nullptr &&
        preferred->device_instance_id == device_instance_id &&
        preferred->phase == CBLifecyclePhase::LIVE &&
        (!priming_hold_active || session_has_capture_activity(*preferred))) {
      resolved.key.kind =
          CaptureRetainedPlanParentKey::Kind::AcquisitionSession;
      resolved.key.id = preferred_acquisition_session_id;
      resolved.acquisition_session_id = preferred_acquisition_session_id;
      resolved.provisional = false;
      return resolved;
    }
  }
  uint64_t preserved_session_id = 0;
  bool preserved_session_ambiguous = false;
  uint32_t preserved_session_evaluator_count = 0;
  uint32_t preserved_session_decision_count = 0;
  uint32_t created_stream_count = 0;
  for (const auto& [stream_id, rec] : streams_.all()) {
    (void)stream_id;
    if (rec.device_instance_id == device_instance_id && rec.created) {
      ++created_stream_count;
    }
  }
  auto consider_preserved_session =
      [&](uint64_t acquisition_session_id) noexcept {
        if (acquisition_session_id == 0 || preserved_session_ambiguous) {
          return;
        }
        if (preserved_session_id == 0) {
          preserved_session_id = acquisition_session_id;
          return;
        }
        if (preserved_session_id != acquisition_session_id) {
          preserved_session_ambiguous = true;
        }
      };
  for (const auto& [key, state] : capture_retained_plan_evaluators_) {
    if (key.kind != CaptureRetainedPlanParentKey::Kind::AcquisitionSession ||
        state.device_instance_id != device_instance_id) {
      continue;
    }
    const auto* session = acquisition_sessions_.find(key.id);
    const bool session_live =
        session != nullptr && session->phase == CBLifecyclePhase::LIVE;
    if (!session_live && state.orphan_retire_after_ns == 0) {
      continue;
    }
    ++preserved_session_evaluator_count;
    consider_preserved_session(key.id);
  }
  for (const auto& [key, decision] : capture_retained_plan_decisions_) {
    if (key.kind != CaptureRetainedPlanParentKey::Kind::AcquisitionSession ||
        decision.device_instance_id != device_instance_id) {
      continue;
    }
    const auto* session = acquisition_sessions_.find(key.id);
    const bool session_live =
        session != nullptr && session->phase == CBLifecyclePhase::LIVE;
    if (!session_live && decision.orphan_retire_after_ns == 0) {
      continue;
    }
    ++preserved_session_decision_count;
    consider_preserved_session(key.id);
  }
  const bool has_live_session_successor = live_session_count != 0;
  if (!has_live_session_successor &&
      !preserved_session_ambiguous &&
      preserved_session_id != 0) {
    resolved.key.kind = CaptureRetainedPlanParentKey::Kind::AcquisitionSession;
    resolved.key.id = preserved_session_id;
    resolved.acquisition_session_id = preserved_session_id;
    resolved.provisional = false;
    return resolved;
  }

  if (priming_hold_active && !device_has_any_stream_(device_instance_id)) {
    return resolved;
  }

  if (!device_has_any_stream_(device_instance_id)) {
    if (preserved_session_ambiguous) {
      capture_latency_trace_printf(
          "capture_parent_resolve_priming_with_session_state device_id=%llu created_stream_count=%u priming_hold_active=%u preserved_session_id=%llu preserved_session_ambiguous=%u evaluator_count=%u decision_count=%u",
          static_cast<unsigned long long>(device_instance_id),
          static_cast<unsigned>(created_stream_count),
          priming_hold_active ? 1u : 0u,
          static_cast<unsigned long long>(preserved_session_id),
          preserved_session_ambiguous ? 1u : 0u,
          static_cast<unsigned>(preserved_session_evaluator_count),
          static_cast<unsigned>(preserved_session_decision_count));
    }
    return resolved;
  }
  if (resolved.key.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming &&
      (preserved_session_evaluator_count != 0 ||
       preserved_session_decision_count != 0)) {
    capture_latency_trace_printf(
        "capture_parent_resolve_priming_with_session_state device_id=%llu created_stream_count=%u priming_hold_active=%u preserved_session_id=%llu preserved_session_ambiguous=%u evaluator_count=%u decision_count=%u",
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned>(created_stream_count),
        priming_hold_active ? 1u : 0u,
        static_cast<unsigned long long>(preserved_session_id),
        preserved_session_ambiguous ? 1u : 0u,
        static_cast<unsigned>(preserved_session_evaluator_count),
        static_cast<unsigned>(preserved_session_decision_count));
  }
  return resolved;
}

bool CoreRuntime::integrate_pending_provider_facts_before_capture_request_() {
  assert(core_thread_.is_core_thread());

  bool changed = false;
  while (!provider_facts_.empty()) {
    if (provider_capture_facts_queued_ != 0) {
      (void)promote_capture_fact_over_repeating_stream_prefix(
          provider_facts_, capture_cohort_registry_);
    }
    const ProviderFactSummary summary =
        summarize_provider_fact(provider_facts_.front(),
                                capture_cohort_registry_);
    if (summary.fact_class == ProviderFactClass::RepeatingStreamFrame) {
      break;
    }

    ProviderToCoreCommand cmd = std::move(provider_facts_.front());
    provider_facts_.pop_front();
    if (provider_fact_is_capture_critical(summary.fact_class) &&
        provider_capture_facts_queued_ != 0) {
      --provider_capture_facts_queued_;
    }

    uint64_t capture_parent_device_instance_id = 0;
    uint64_t preferred_acquisition_session_id = 0;
    if (summary.type == ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_CREATED) {
      const auto& p = std::get<CmdProviderNativeObjectCreated>(cmd.payload);
      if (p.type == static_cast<uint32_t>(NativeObjectType::AcquisitionSession)) {
        capture_parent_device_instance_id = p.owner_device_instance_id;
        preferred_acquisition_session_id = p.native_id;
      }
    } else if (summary.type ==
               ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_DESTROYED) {
      const auto& p = std::get<CmdProviderNativeObjectDestroyed>(cmd.payload);
      if (const auto* session = acquisition_sessions_.find(p.native_id);
          session != nullptr) {
        capture_parent_device_instance_id = session->device_instance_id;
      }
    } else if (summary.type ==
                   ProviderToCoreCommandType::PROVIDER_CAPTURE_STARTED ||
               summary.type ==
                   ProviderToCoreCommandType::PROVIDER_CAPTURE_COMPLETED ||
               summary.type ==
                   ProviderToCoreCommandType::PROVIDER_CAPTURE_FAILED) {
      capture_parent_device_instance_id = summary.device_instance_id;
    }

    dispatcher_.dispatch(std::move(cmd));
    if (capture_parent_device_instance_id != 0 &&
        rehome_capture_retained_plan_parent_state_(
            capture_parent_device_instance_id,
            preferred_acquisition_session_id)) {
      changed = true;
    }
    if (provider_fact_is_capture_critical(summary.fact_class)) {
      release_result_safe_capture_stream_preemptions_();
    }
  }

  if (dispatcher_.consume_relevant_state_changed()) {
    changed = true;
  }
  if (changed) {
    request_publish_from_core_unchecked();
  }
  return changed;
}

void CoreRuntime::erase_capture_retained_plan_state_for_device_(
    uint64_t device_instance_id,
    const CaptureRetainedPlanParentKey* keep) {
  assert(core_thread_.is_core_thread());

  for (auto it = capture_retained_plan_evaluators_.begin();
       it != capture_retained_plan_evaluators_.end();) {
    if (it->second.device_instance_id == device_instance_id &&
        (!keep || it->first < *keep || *keep < it->first)) {
      it = capture_retained_plan_evaluators_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = capture_retained_plan_decisions_.begin();
       it != capture_retained_plan_decisions_.end();) {
    const bool keep_matching =
        keep && !((*keep < it->first) || (it->first < *keep));
    const bool same_device =
        it->second.device_instance_id == device_instance_id ||
        (it->first.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming &&
         it->first.id == device_instance_id);
    if (same_device && !keep_matching) {
      it = capture_retained_plan_decisions_.erase(it);
    } else {
      ++it;
    }
  }
}

bool CoreRuntime::rehome_capture_retained_plan_parent_state_(
    uint64_t device_instance_id,
    uint64_t preferred_acquisition_session_id) {
  assert(core_thread_.is_core_thread());
  if (device_instance_id == 0) {
    return false;
  }

  const CoreDeviceRegistry::DeviceRecord* device =
      devices_.find(device_instance_id);
  if (device == nullptr) {
    return false;
  }

  const ResolvedCaptureRetainedPlanParent parent =
      resolve_capture_retained_plan_parent_(
          device_instance_id, preferred_acquisition_session_id);
  auto same_parent_key = [](const CaptureRetainedPlanParentKey& a,
                            const CaptureRetainedPlanParentKey& b) noexcept {
    return !(a < b) && !(b < a);
  };
  auto can_migrate_priming_evaluator_to_parent =
      [&](const CaptureRetainedPlanParentKey& key,
          const RetainedPlanEvaluatorState& state) noexcept {
        return parent.key.kind ==
                   CaptureRetainedPlanParentKey::Kind::AcquisitionSession &&
               key.kind ==
                   CaptureRetainedPlanParentKey::Kind::CapturePriming &&
               parent.acquisition_session_id != 0 &&
               state.acquisition_session_id == 0;
      };
  auto retired_real_session_parent =
      [&](const CaptureRetainedPlanParentKey& key) noexcept {
        return key.kind ==
                   CaptureRetainedPlanParentKey::Kind::AcquisitionSession &&
               (parent.key.kind !=
                    CaptureRetainedPlanParentKey::Kind::AcquisitionSession ||
                key.id != parent.key.id);
      };
  const uint64_t now_ns = ns_since_epoch_();
  const bool has_live_session_successor =
      acquisition_sessions_.resolve_live_session_id_for_device(
          device_instance_id) != 0;
  const bool preserve_retired_real_session_parent_as_orphan =
      parent.key.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming &&
      !device_has_any_stream_(device_instance_id) &&
      !has_live_session_successor;
  bool changed = false;
  bool restart_from_seed = false;
  bool preserved_orphan_session_parent = false;
  if (parent.acquisition_session_id != 0) {
    changed =
        acquisition_sessions_.set_backing_capabilities(
            parent.acquisition_session_id,
            device->runtime_backing_capabilities,
            device->parent_context_backing_capabilities) ||
        changed;
  }

  auto evaluator_matches_device =
      [device_instance_id](const CaptureRetainedPlanParentKey& key,
                           const RetainedPlanEvaluatorState& state) noexcept {
        return state.device_instance_id == device_instance_id ||
               (key.kind ==
                    CaptureRetainedPlanParentKey::Kind::CapturePriming &&
                key.id == device_instance_id);
      };
  auto decision_matches_device =
      [device_instance_id](const CaptureRetainedPlanParentKey& key,
                           const RetainedPlanDecisionProvenance& decision) noexcept {
        return decision.device_instance_id == device_instance_id ||
               (key.kind ==
                    CaptureRetainedPlanParentKey::Kind::CapturePriming &&
                key.id == device_instance_id);
      };

  auto current_eval_it =
      capture_retained_plan_evaluators_.find(parent.key);
  if (current_eval_it == capture_retained_plan_evaluators_.end()) {
    for (auto it = capture_retained_plan_evaluators_.begin();
         it != capture_retained_plan_evaluators_.end();
         ++it) {
      if (!evaluator_matches_device(it->first, it->second) ||
          same_parent_key(it->first, parent.key)) {
        continue;
      }
      if (!can_migrate_priming_evaluator_to_parent(it->first, it->second)) {
        if (retired_real_session_parent(it->first) && it->second.active) {
          restart_from_seed = true;
        }
        continue;
      }
      RetainedPlanEvaluatorState moved = it->second;
      moved.acquisition_session_id = parent.acquisition_session_id;
      moved.orphan_retire_after_ns = 0;
      capture_latency_trace_printf(
          "capture_plan_state_rehome device_id=%llu dst_parent_kind=%u dst_parent_id=%llu src_parent_kind=%u src_parent_id=%llu src_candidate_index=%u src_candidate_count=%u",
          static_cast<unsigned long long>(device_instance_id),
          static_cast<unsigned>(parent.key.kind),
          static_cast<unsigned long long>(parent.key.id),
          static_cast<unsigned>(it->first.kind),
          static_cast<unsigned long long>(it->first.id),
          static_cast<unsigned>(moved.current_candidate_index),
          static_cast<unsigned>(moved.candidate_count));
      capture_retained_plan_evaluators_.erase(it);
      current_eval_it =
          capture_retained_plan_evaluators_
              .insert_or_assign(parent.key, moved)
              .first;
      changed = true;
      break;
    }
  } else if (current_eval_it->second.acquisition_session_id !=
             parent.acquisition_session_id) {
    current_eval_it->second.acquisition_session_id =
        parent.acquisition_session_id;
    current_eval_it->second.orphan_retire_after_ns = 0;
    changed = true;
  }
  for (auto it = capture_retained_plan_evaluators_.begin();
       it != capture_retained_plan_evaluators_.end();) {
    if (!evaluator_matches_device(it->first, it->second) ||
        same_parent_key(it->first, parent.key)) {
      ++it;
      continue;
    }
    if (preserve_retired_real_session_parent_as_orphan &&
        retired_real_session_parent(it->first)) {
      if (it->second.orphan_retire_after_ns == 0) {
        it->second.orphan_retire_after_ns =
            now_ns + kCaptureRetainedPlanOrphanRetentionWindowNs;
        changed = true;
      }
      preserved_orphan_session_parent = true;
      ++it;
      continue;
    }
    if (retired_real_session_parent(it->first) && it->second.active) {
      restart_from_seed = true;
    }
    capture_latency_trace_printf(
        "capture_plan_state_rehome_drop device_id=%llu dst_parent_kind=%u dst_parent_id=%llu src_parent_kind=%u src_parent_id=%llu action=discard_without_merge",
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned>(parent.key.kind),
        static_cast<unsigned long long>(parent.key.id),
        static_cast<unsigned>(it->first.kind),
        static_cast<unsigned long long>(it->first.id));
    it = capture_retained_plan_evaluators_.erase(it);
    changed = true;
  }

  auto current_decision_it =
      capture_retained_plan_decisions_.find(parent.key);
  if (current_decision_it == capture_retained_plan_decisions_.end()) {
    for (auto it = capture_retained_plan_decisions_.begin();
         it != capture_retained_plan_decisions_.end();
         ++it) {
      if (!decision_matches_device(it->first, it->second) ||
          same_parent_key(it->first, parent.key)) {
        continue;
      }
      if (!retired_real_session_parent(it->first)) {
        continue;
      }

      // A direct, non-evaluated decision is capability-derived rather than
      // measurement-epoch evidence. When a transient capture-owned session
      // retires and CapturePriming becomes the current parent again, carry that
      // decision back to the current parent instead of hiding it under an
      // orphaned session key. Evaluated decisions remain session-epoch truth
      // and continue through the bounded orphan/seed-restart path below.
      if (parent.key.kind ==
              CaptureRetainedPlanParentKey::Kind::CapturePriming &&
          !it->second.from_evaluation &&
          it->second.valid &&
          it->second.selected.valid) {
        RetainedPlanDecisionProvenance moved = it->second;
        const CaptureRetainedPlanParentKey source_key = it->first;
        capture_retained_plan_decisions_.erase(it);
        moved.acquisition_session_id = 0;
        moved.orphan_retire_after_ns = 0;
        current_decision_it =
            capture_retained_plan_decisions_
                .insert_or_assign(parent.key, moved)
                .first;
        capture_latency_trace_printf(
            "capture_plan_state_rehome device_id=%llu dst_parent_kind=%u dst_parent_id=%llu src_parent_kind=%u src_parent_id=%llu source=direct_decision selected_posture=%d",
            static_cast<unsigned long long>(device_instance_id),
            static_cast<unsigned>(parent.key.kind),
            static_cast<unsigned long long>(parent.key.id),
            static_cast<unsigned>(source_key.kind),
            static_cast<unsigned long long>(source_key.id),
            static_cast<int>(moved.selected.posture));
        changed = true;
        break;
      }

      restart_from_seed = true;
    }
  } else if (current_decision_it->second.acquisition_session_id !=
             parent.acquisition_session_id) {
    current_decision_it->second.acquisition_session_id =
        parent.acquisition_session_id;
    current_decision_it->second.orphan_retire_after_ns = 0;
    changed = true;
  }
  for (auto it = capture_retained_plan_decisions_.begin();
       it != capture_retained_plan_decisions_.end();) {
    if (!decision_matches_device(it->first, it->second) ||
        same_parent_key(it->first, parent.key)) {
      ++it;
      continue;
    }
    if (preserve_retired_real_session_parent_as_orphan &&
        retired_real_session_parent(it->first)) {
      if (it->second.orphan_retire_after_ns == 0) {
        it->second.orphan_retire_after_ns =
            now_ns + kCaptureRetainedPlanOrphanRetentionWindowNs;
        changed = true;
      }
      preserved_orphan_session_parent = true;
      ++it;
      continue;
    }
    if (current_eval_it == capture_retained_plan_evaluators_.end() &&
        current_decision_it == capture_retained_plan_decisions_.end() &&
        it->first.kind ==
            CaptureRetainedPlanParentKey::Kind::CapturePriming &&
        it->first.id == device_instance_id) {
      ++it;
      continue;
    }
    if (retired_real_session_parent(it->first)) {
      restart_from_seed = true;
    }
    it = capture_retained_plan_decisions_.erase(it);
    changed = true;
  }
  if (current_eval_it == capture_retained_plan_evaluators_.end() &&
      current_decision_it != capture_retained_plan_decisions_.end() &&
      parent.acquisition_session_id != 0) {
    if (current_decision_it->second.from_evaluation) {
      release_capture_parent_priming_(
          device_instance_id,
          "sync_capture_parent_priming.provider_sync_failed");
    }
  }

  if (parent.key.kind ==
          CaptureRetainedPlanParentKey::Kind::AcquisitionSession &&
      parent.acquisition_session_id != 0 &&
      current_eval_it == capture_retained_plan_evaluators_.end() &&
      current_decision_it == capture_retained_plan_decisions_.end() &&
      !device->steady_retained_plan.valid) {
    restart_from_seed = true;
  }

  if (!preserved_orphan_session_parent &&
      restart_from_seed &&
      (parent.key.kind ==
           CaptureRetainedPlanParentKey::Kind::AcquisitionSession ||
       parent.key.kind ==
            CaptureRetainedPlanParentKey::Kind::CapturePriming)) {
    return refresh_capture_retained_plan_state_(
               device_instance_id,
               /*requested_bump_access_posture_epoch=*/false) ||
           changed;
  }

  if (parent.acquisition_session_id != 0) {
    const bool session_owns_capture_plan_state =
        current_eval_it != capture_retained_plan_evaluators_.end() ||
        current_decision_it != capture_retained_plan_decisions_.end();
    const auto* session_rec =
        acquisition_sessions_.find(parent.acquisition_session_id);
    const bool session_requested_unset =
        session_rec == nullptr || !session_rec->requested_retained_plan.valid;
    if ((!session_owns_capture_plan_state || session_requested_unset) &&
        device->requested_retained_plan.valid) {
      changed =
          acquisition_sessions_.set_requested_retained_plan(
              parent.acquisition_session_id,
              device->requested_retained_plan,
              /*bump_capture_access_posture_epoch=*/false) ||
          changed;
    } else if (!session_owns_capture_plan_state) {
      changed =
          acquisition_sessions_.set_requested_retained_plan(
              parent.acquisition_session_id,
              CoreRetainedProductionPlan{},
              /*bump_capture_access_posture_epoch=*/false) ||
          changed;
    }
    if (!session_owns_capture_plan_state &&
        device->steady_retained_plan.valid) {
      changed =
          acquisition_sessions_.set_steady_retained_plan(
              parent.acquisition_session_id,
              device->steady_retained_plan) ||
          changed;
    } else if (!session_owns_capture_plan_state) {
      changed =
          acquisition_sessions_.clear_steady_retained_plan(
              parent.acquisition_session_id) ||
          changed;
    }
  }

  return changed;
}

bool CoreRuntime::refresh_capture_retained_plan_state_(
    uint64_t device_instance_id,
    bool requested_bump_access_posture_epoch) {
  assert(core_thread_.is_core_thread());

  CaptureRequest effective{};
  if (!build_effective_capture_request_without_retained_plan_(
          device_instance_id, effective)) {
    erase_capture_retained_plan_state_for_device_(device_instance_id, nullptr);
    release_capture_parent_priming_(
        device_instance_id,
        "refresh.build_effective_failed");
    return false;
  }
  ProducerBackingCapabilities runtime_caps{};
  ProducerBackingCapabilities parent_context_caps{};
  if (!resolve_capture_backing_capabilities_(
          device_instance_id,
          effective,
          runtime_caps,
          parent_context_caps)) {
    erase_capture_retained_plan_state_for_device_(device_instance_id, nullptr);
    release_capture_parent_priming_(
        device_instance_id,
        "refresh.resolve_backing_caps_failed");
    return false;
  }
  auto evaluator_matches_device =
      [device_instance_id](const CaptureRetainedPlanParentKey& key,
                           const RetainedPlanEvaluatorState& state) noexcept {
        return state.device_instance_id == device_instance_id ||
               (key.kind ==
                    CaptureRetainedPlanParentKey::Kind::CapturePriming &&
                key.id == device_instance_id);
      };
  auto decision_matches_device =
      [device_instance_id](const CaptureRetainedPlanParentKey& key,
                           const RetainedPlanDecisionProvenance& decision) noexcept {
        return decision.device_instance_id == device_instance_id ||
               (key.kind ==
                    CaptureRetainedPlanParentKey::Kind::CapturePriming &&
                key.id == device_instance_id);
      };
  auto same_parent_key = [](const CaptureRetainedPlanParentKey& a,
                            const CaptureRetainedPlanParentKey& b) noexcept {
    return !(a < b) && !(b < a);
  };
  (void)devices_.set_backing_capabilities(
      device_instance_id, runtime_caps, parent_context_caps);
  const ResolvedCaptureRetainedPlanParent parent =
      resolve_capture_retained_plan_parent_(device_instance_id);
  if (parent.acquisition_session_id != 0) {
    (void)acquisition_sessions_.set_backing_capabilities(
        parent.acquisition_session_id,
        runtime_caps,
        parent_context_caps);
  }
  const CapturePrimingSeedSignature seed_signature =
      build_capture_priming_seed_signature_(
          device_instance_id, effective, runtime_caps, parent_context_caps);
  CoreRetainedProductionPlan preferred_requested{};
  (void)try_find_capture_priming_seed_(seed_signature, preferred_requested);
  const RetainedPlanResetDecision decision =
      build_retained_plan_reset_decision(
          BackingPlanEvaluationPrimaryFunction::CaptureReadyAndMaterialize,
          parent_context_caps,
          preferred_requested);
  auto preserved_evaluator_matches_decision =
      [&](const RetainedPlanEvaluatorState& state) noexcept {
        if (!state.active || !decision.evaluation_active ||
            state.candidate_count != decision.candidate_count) {
          return false;
        }
        for (uint8_t i = 0; i < decision.candidate_count; ++i) {
          if (!state.candidate_sequence[i].valid ||
              state.candidate_sequence[i].posture !=
                  decision.candidate_sequence[i]) {
            return false;
          }
        }
        return true;
      };
  auto preserved_evaluator_matches_signature =
      [&](const RetainedPlanEvaluatorState& state) noexcept {
        const CapturePrimingSeedSignature& signature =
            state.capture_priming_seed_signature;
        return signature.hardware_id == seed_signature.hardware_id &&
               signature.width == seed_signature.width &&
               signature.height == seed_signature.height &&
               signature.format_fourcc == seed_signature.format_fourcc &&
               same_picture_config(signature.picture, seed_signature.picture) &&
               same_bundle_members(
                   signature.still_image_bundle,
                   seed_signature.still_image_bundle) &&
               same_backing_capabilities(
                   signature.runtime_backing_capabilities,
                   seed_signature.runtime_backing_capabilities) &&
               same_backing_capabilities(
                   signature.parent_context_backing_capabilities,
                   seed_signature.parent_context_backing_capabilities);
      };
  auto can_preserve_existing_evaluator =
      [&](const CaptureRetainedPlanParentKey& key,
          const RetainedPlanEvaluatorState& state) noexcept {
        if (same_parent_key(key, parent.key)) {
          return true;
        }
        return key.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming &&
               parent.key.kind ==
                   CaptureRetainedPlanParentKey::Kind::AcquisitionSession &&
               parent.acquisition_session_id != 0 &&
               state.acquisition_session_id == 0;
      };
  auto retained_plan_evaluator_has_evidence =
      [](const RetainedPlanEvaluatorState& state) noexcept {
        for (const MeasuredPlanEvidence& evidence : state.evidence) {
          if (evidence.observed_display_view ||
              evidence.has_display_view_elapsed_ns ||
              evidence.observed_to_image ||
              evidence.has_materialization_elapsed_ns ||
              evidence.has_capture_ready_elapsed_ns ||
              evidence.has_total_elapsed_ns ||
              evidence.has_normalized_cost_units) {
            return true;
          }
        }
        return false;
      };
  auto should_prefer_preserved_evaluator =
      [&](const RetainedPlanEvaluatorState& current,
          const RetainedPlanEvaluatorState& candidate) noexcept {
        if (candidate.active != current.active) {
          return candidate.active;
        }
        if (candidate.current_candidate_index != current.current_candidate_index) {
          return candidate.current_candidate_index >
                 current.current_candidate_index;
        }
        const bool current_has_evidence =
            retained_plan_evaluator_has_evidence(current);
        const bool candidate_has_evidence =
            retained_plan_evaluator_has_evidence(candidate);
        if (candidate_has_evidence != current_has_evidence) {
          return candidate_has_evidence;
        }
        return false;
      };
  RetainedPlanEvaluatorState preserved_evaluator;
  CaptureRetainedPlanParentKey preserved_evaluator_key{};
  bool have_preserved_evaluator = false;
  for (const auto& [key, state] : capture_retained_plan_evaluators_) {
    if (!evaluator_matches_device(key, state) ||
        !can_preserve_existing_evaluator(key, state) ||
        !preserved_evaluator_matches_signature(state) ||
        !preserved_evaluator_matches_decision(state)) {
      continue;
    }
    if (!have_preserved_evaluator ||
        should_prefer_preserved_evaluator(preserved_evaluator, state)) {
      preserved_evaluator = state;
      preserved_evaluator_key = key;
      have_preserved_evaluator = true;
    }
  }
  const bool preserve_existing_evaluator = have_preserved_evaluator;
  auto preserved_decision_matches_signature =
      [&](const RetainedPlanDecisionProvenance& provenance) noexcept {
        const CapturePrimingSeedSignature& signature =
            provenance.capture_priming_seed_signature;
        if (!provenance.valid || !provenance.selected.valid ||
            provenance.primary_function !=
                BackingPlanEvaluationPrimaryFunction::
                    CaptureReadyAndMaterialize) {
          return false;
        }
        const bool same_base_signature =
            signature.hardware_id == seed_signature.hardware_id &&
            signature.width == seed_signature.width &&
            signature.height == seed_signature.height &&
            signature.format_fourcc == seed_signature.format_fourcc &&
            same_picture_config(signature.picture, seed_signature.picture) &&
            same_bundle_members(
                signature.still_image_bundle, seed_signature.still_image_bundle);
        const bool same_non_evaluated_seed_signature =
            same_base_signature &&
            same_backing_capabilities(
                signature.runtime_backing_capabilities,
                seed_signature.runtime_backing_capabilities);
        const bool same_evaluated_seed_signature =
            same_non_evaluated_seed_signature &&
            same_backing_capabilities(
                signature.parent_context_backing_capabilities,
                seed_signature.parent_context_backing_capabilities);
        if (!same_base_signature) {
          return false;
        }
        if (provenance.from_evaluation) {
          if (!same_evaluated_seed_signature) {
            return false;
          }
          return parent_context_caps.viable(provenance.selected.posture);
        }
        if (same_non_evaluated_seed_signature) {
          return true;
        }
        const CoreRetainedProductionPlan current_selected =
            decision.steady.valid ? decision.steady : decision.requested;
        if (!same_retained_plan(provenance.selected, current_selected) ||
            provenance.candidate_count != decision.candidate_count) {
          return false;
        }
        for (uint8_t i = 0; i < decision.candidate_count; ++i) {
          const CoreRetainedProductionPlan current_candidate =
              make_retained_plan(decision.candidate_sequence[i]);
          if (!same_retained_plan(
                  provenance.candidate_sequence[i], current_candidate)) {
            return false;
          }
        }
        return parent_context_caps.viable(provenance.selected.posture);
      };
  auto can_preserve_existing_decision =
      [&](const CaptureRetainedPlanParentKey& key,
          const RetainedPlanDecisionProvenance& provenance) noexcept {
        if (same_parent_key(key, parent.key)) {
          return true;
        }
        return key.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming &&
               parent.key.kind ==
                   CaptureRetainedPlanParentKey::Kind::AcquisitionSession &&
               parent.acquisition_session_id != 0 &&
               !provenance.from_evaluation;
      };
  auto should_prefer_preserved_decision =
      [&](const CaptureRetainedPlanParentKey& current_key,
          const RetainedPlanDecisionProvenance& current,
          const CaptureRetainedPlanParentKey& candidate_key,
          const RetainedPlanDecisionProvenance& candidate) noexcept {
        const bool candidate_is_current_parent =
            same_parent_key(candidate_key, parent.key);
        const bool current_is_current_parent =
            same_parent_key(current_key, parent.key);
        if (candidate_is_current_parent != current_is_current_parent) {
          return candidate_is_current_parent;
        }
        if (candidate.from_evaluation != current.from_evaluation) {
          return candidate.from_evaluation;
        }
        const bool candidate_is_session =
            candidate_key.kind ==
            CaptureRetainedPlanParentKey::Kind::AcquisitionSession;
        const bool current_is_session =
            current_key.kind ==
            CaptureRetainedPlanParentKey::Kind::AcquisitionSession;
        if (candidate_is_session != current_is_session) {
          return candidate_is_session;
        }
        return false;
      };
  RetainedPlanDecisionProvenance preserved_decision;
  CaptureRetainedPlanParentKey preserved_decision_key{};
  bool have_preserved_decision = false;
  for (const auto& [key, provenance] : capture_retained_plan_decisions_) {
    if (!decision_matches_device(key, provenance) ||
        !can_preserve_existing_decision(key, provenance) ||
        !preserved_decision_matches_signature(provenance)) {
      continue;
    }
    if (!have_preserved_decision ||
        should_prefer_preserved_decision(
            preserved_decision_key,
            preserved_decision,
            key,
            provenance)) {
      preserved_decision = provenance;
      preserved_decision_key = key;
      have_preserved_decision = true;
    }
  }
  const bool preserve_existing_decision = have_preserved_decision;
  auto decision_matches_current_non_evaluated_shape =
      [&](const RetainedPlanDecisionProvenance& provenance) noexcept {
        if (!provenance.valid || provenance.from_evaluation) {
          return false;
        }
        const CoreRetainedProductionPlan current_selected =
            decision.steady.valid ? decision.steady : decision.requested;
        if (!same_retained_plan(provenance.selected, current_selected) ||
            provenance.candidate_count != decision.candidate_count) {
          return false;
        }
        for (uint8_t i = 0; i < decision.candidate_count; ++i) {
          if (!same_retained_plan(
                  provenance.candidate_sequence[i],
                  make_retained_plan(decision.candidate_sequence[i]))) {
            return false;
          }
        }
        return true;
      };
  const auto existing_parent_decision_it =
      capture_retained_plan_decisions_.find(parent.key);
  const bool same_non_evaluated_decision_already_installed =
      existing_parent_decision_it != capture_retained_plan_decisions_.end() &&
      decision_matches_current_non_evaluated_shape(
          existing_parent_decision_it->second);

  erase_capture_retained_plan_state_for_device_(
      device_instance_id, &parent.key);
  if (!decision.requested.valid) {
    const char* const parent_kind_name =
        parent.acquisition_session_id != 0
            ? "acquisition_session"
            : "capture_priming";
    capture_latency_trace_printf(
        "capture_plan_state_refresh_rejected device_id=%llu parent_kind=%s parent_id=%llu reason=no_viable_parent_context_postures primary_function=%s runtime_caps_cpu=%u runtime_caps_gpu=%u runtime_caps_gpu_cpu_sidecar=%u parent_context_caps_cpu=%u parent_context_caps_gpu=%u parent_context_caps_gpu_cpu_sidecar=%u",
        static_cast<unsigned long long>(device_instance_id),
        parent_kind_name,
        static_cast<unsigned long long>(parent.key.id),
        backing_plan_primary_function_name(
            BackingPlanEvaluationPrimaryFunction::CaptureReadyAndMaterialize),
        runtime_caps.cpu_backed_available ? 1u : 0u,
        runtime_caps.gpu_backed_available ? 1u : 0u,
        runtime_caps.gpu_with_cpu_sidecar_available ? 1u : 0u,
        parent_context_caps.cpu_backed_available ? 1u : 0u,
        parent_context_caps.gpu_backed_available ? 1u : 0u,
        parent_context_caps.gpu_with_cpu_sidecar_available ? 1u : 0u);
    (void)devices_.set_requested_retained_plan(
        device_instance_id,
        CoreRetainedProductionPlan{},
        requested_bump_access_posture_epoch);
    (void)devices_.clear_steady_retained_plan(device_instance_id);
    if (parent.acquisition_session_id != 0) {
      (void)acquisition_sessions_.set_requested_retained_plan(
          parent.acquisition_session_id,
          CoreRetainedProductionPlan{},
          requested_bump_access_posture_epoch);
      (void)acquisition_sessions_.clear_steady_retained_plan(
          parent.acquisition_session_id);
    }
    capture_retained_plan_evaluators_.erase(parent.key);
    capture_retained_plan_decisions_.erase(parent.key);
    release_capture_parent_priming_(
        device_instance_id,
        "refresh.no_viable_requested_plan");
    return false;
  }

  if (preserve_existing_evaluator) {
    RetainedPlanEvaluatorState state = preserved_evaluator;
    state.device_instance_id = device_instance_id;
    state.acquisition_session_id = parent.acquisition_session_id;
    state.orphan_retire_after_ns = 0;
    state.capture_priming_seed_signature = seed_signature;
    CoreRetainedProductionPlan requested = decision.requested;
    if (state.current_candidate_index < state.candidate_count &&
        state.candidate_sequence[state.current_candidate_index].valid) {
      requested = state.candidate_sequence[state.current_candidate_index];
    }
    (void)devices_.set_requested_retained_plan(
        device_instance_id,
        requested,
        requested_bump_access_posture_epoch);
    (void)devices_.clear_steady_retained_plan(device_instance_id);
    if (parent.acquisition_session_id != 0) {
      (void)acquisition_sessions_.set_requested_retained_plan(
          parent.acquisition_session_id,
          requested,
          requested_bump_access_posture_epoch);
      (void)acquisition_sessions_.clear_steady_retained_plan(
          parent.acquisition_session_id);
    }
    capture_latency_trace_printf(
        "capture_plan_state_refresh_preserve device_id=%llu parent_kind=%u parent_id=%llu source=evaluator source_parent_kind=%u source_parent_id=%llu current_candidate_index=%u requested_posture=%d",
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned>(parent.key.kind),
        static_cast<unsigned long long>(parent.key.id),
        static_cast<unsigned>(preserved_evaluator_key.kind),
        static_cast<unsigned long long>(preserved_evaluator_key.id),
        static_cast<unsigned>(state.current_candidate_index),
        static_cast<int>(requested.posture));
    capture_retained_plan_evaluators_[parent.key] = state;
    capture_retained_plan_decisions_.erase(parent.key);
    return true;
  }

  if (preserve_existing_decision) {
    RetainedPlanDecisionProvenance provenance = preserved_decision;
    provenance.device_instance_id = device_instance_id;
    provenance.acquisition_session_id = parent.acquisition_session_id;
    provenance.orphan_retire_after_ns = 0;
    provenance.capture_priming_seed_signature = seed_signature;
    (void)devices_.set_requested_retained_plan(
        device_instance_id,
        provenance.selected,
        requested_bump_access_posture_epoch);
    (void)devices_.set_steady_retained_plan(
        device_instance_id, provenance.selected);
    if (parent.acquisition_session_id != 0) {
      (void)acquisition_sessions_.set_requested_retained_plan(
          parent.acquisition_session_id,
          provenance.selected,
          requested_bump_access_posture_epoch);
      (void)acquisition_sessions_.set_steady_retained_plan(
          parent.acquisition_session_id, provenance.selected);
    }
    capture_latency_trace_printf(
        "capture_plan_state_refresh_preserve device_id=%llu parent_kind=%u parent_id=%llu source=decision source_parent_kind=%u source_parent_id=%llu selected_posture=%d from_evaluation=%u",
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned>(parent.key.kind),
        static_cast<unsigned long long>(parent.key.id),
        static_cast<unsigned>(preserved_decision_key.kind),
        static_cast<unsigned long long>(preserved_decision_key.id),
        static_cast<int>(provenance.selected.posture),
        provenance.from_evaluation ? 1u : 0u);
    capture_retained_plan_evaluators_.erase(parent.key);
    capture_retained_plan_decisions_[parent.key] = provenance;
    if (provenance.from_evaluation) {
      release_capture_parent_priming_(
          device_instance_id,
          "rehome.evaluated_decision_on_real_parent");
    }
    return true;
  }

  (void)devices_.set_requested_retained_plan(
      device_instance_id,
      decision.requested,
      requested_bump_access_posture_epoch);
  if (parent.acquisition_session_id != 0) {
    (void)acquisition_sessions_.set_requested_retained_plan(
        parent.acquisition_session_id,
        decision.requested,
        requested_bump_access_posture_epoch);
  }
  if (decision.steady.valid) {
    (void)devices_.set_steady_retained_plan(device_instance_id, decision.steady);
    if (parent.acquisition_session_id != 0) {
      (void)acquisition_sessions_.set_steady_retained_plan(
          parent.acquisition_session_id, decision.steady);
    }
  } else {
    (void)devices_.clear_steady_retained_plan(device_instance_id);
    if (parent.acquisition_session_id != 0) {
      (void)acquisition_sessions_.clear_steady_retained_plan(
          parent.acquisition_session_id);
    }
  }

  if (decision.evaluation_active) {
    RetainedPlanEvaluatorState state;
    state.device_instance_id = device_instance_id;
    state.acquisition_session_id = parent.acquisition_session_id;
    state.orphan_retire_after_ns = 0;
    state.current_candidate_ready_after_ns =
        ns_since_epoch_() + capture_backing_plan_evaluation_settle_delay_ns();
    state.primary_function =
        BackingPlanEvaluationPrimaryFunction::CaptureReadyAndMaterialize;
    state.completion_reason = BackingPlanEvaluationCompletionReason::None;
    state.capture_priming_seed_signature = seed_signature;
    state.active = true;
    state.candidate_count = decision.candidate_count;
    state.current_candidate_index = 0;
    for (uint8_t i = 0; i < decision.candidate_count; ++i) {
      state.candidate_sequence[i] = make_retained_plan(
          decision.candidate_sequence[i]);
    }
    capture_latency_trace_printf(
        "capture_plan_state_refresh device_id=%llu parent_kind=%u parent_id=%llu requested_posture=%d steady_valid=%u candidate_count=%u preferred_requested_valid=%u preferred_requested_posture=%d",
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned>(parent.key.kind),
        static_cast<unsigned long long>(parent.key.id),
        static_cast<int>(decision.requested.posture),
        decision.steady.valid ? 1u : 0u,
        static_cast<unsigned>(decision.candidate_count),
        preferred_requested.valid ? 1u : 0u,
        static_cast<int>(preferred_requested.posture));
    capture_retained_plan_evaluators_[parent.key] = state;
    capture_retained_plan_decisions_.erase(parent.key);
  } else {
    capture_retained_plan_evaluators_.erase(parent.key);
    remember_capture_priming_seed_(
        seed_signature,
        decision.steady.valid ? decision.steady : decision.requested);
    CoreRetainedProductionPlan candidate_sequence[3]{};
    for (uint8_t i = 0; i < decision.candidate_count; ++i) {
      candidate_sequence[i] = make_retained_plan(decision.candidate_sequence[i]);
    }
    RetainedPlanDecisionProvenance provenance =
        build_non_evaluated_decision_provenance_(
            BackingPlanEvaluationPrimaryFunction::CaptureReadyAndMaterialize,
            device_instance_id,
            parent.acquisition_session_id,
            decision.requested,
            decision.steady,
            decision.candidate_count,
            candidate_sequence);
    provenance.orphan_retire_after_ns = 0;
    provenance.capture_priming_seed_signature = seed_signature;
    capture_retained_plan_decisions_[parent.key] = provenance;
    if (parent.acquisition_session_id == 0 &&
        !same_non_evaluated_decision_already_installed) {
      (void)sync_capture_parent_priming_(
          device_instance_id, effective, runtime_caps, parent_context_caps);
      release_capture_parent_priming_(
          device_instance_id,
          "refresh.preserve_evaluated_decision");
    }
    return true;
  }
  (void)sync_capture_parent_priming_(
      device_instance_id, effective, runtime_caps, parent_context_caps);
  return true;
}

void CoreRuntime::handle_stream_retained_to_image_observation_(
    uint64_t stream_id,
    uint64_t posture_id,
    ResultCapability provisional_to_image,
    bool has_normalized_cost_units,
    uint64_t normalized_cost_units) {
  assert(core_thread_.is_core_thread());
  if (stream_id == 0 || posture_id == 0) {
    return;
  }
  auto state_it = stream_retained_plan_evaluators_.find(stream_id);
  const CoreStreamRegistry::StreamRecord* rec = streams_.find(stream_id);
  if (state_it == stream_retained_plan_evaluators_.end() ||
      rec == nullptr ||
      !rec->requested_retained_plan.valid) {
    return;
  }

  RetainedPlanEvaluatorState& state = state_it->second;
  if (!state.active || state.current_candidate_index >= state.candidate_count) {
    return;
  }
  const CoreRetainedProductionPlan expected_plan =
      state.candidate_sequence[state.current_candidate_index];
  if (!same_retained_plan(rec->requested_retained_plan, expected_plan)) {
    return;
  }

  MeasuredPlanEvidence& evidence =
      state.evidence[retained_plan_evidence_index(
          rec->requested_retained_plan.posture)];
  evidence.observed_to_image = true;
  evidence.provisional_to_image = provisional_to_image;
  if (has_normalized_cost_units) {
    evidence.has_normalized_cost_units = true;
    evidence.normalized_cost_units = normalized_cost_units;
  }
}

void CoreRuntime::handle_stream_retained_display_view_observation_(
    uint64_t stream_id,
    uint64_t posture_id,
    ResultCapability provisional_display_view,
    bool has_display_view_elapsed_ns,
    uint64_t display_view_elapsed_ns) {
  assert(core_thread_.is_core_thread());
  if (stream_id == 0 || posture_id == 0) {
    return;
  }
  auto state_it = stream_retained_plan_evaluators_.find(stream_id);
  const CoreStreamRegistry::StreamRecord* rec = streams_.find(stream_id);
  if (state_it == stream_retained_plan_evaluators_.end() ||
      rec == nullptr ||
      !rec->requested_retained_plan.valid) {
    return;
  }

  RetainedPlanEvaluatorState& state = state_it->second;
  if (!state.active || state.current_candidate_index >= state.candidate_count) {
    return;
  }
  const CoreRetainedProductionPlan expected_plan =
      state.candidate_sequence[state.current_candidate_index];
  if (!same_retained_plan(rec->requested_retained_plan, expected_plan)) {
    return;
  }
  const SharedStreamResultData observed_result =
      result_store_.get_latest_stream_result(stream_id);
  CoreProductionPostureShape observed_posture{};
  if (!observed_result ||
      observed_result->access_posture.posture_id != posture_id ||
      !infer_stream_result_posture_shape_(observed_result, observed_posture) ||
      observed_posture != expected_plan.posture) {
    return;
  }

  MeasuredPlanEvidence& evidence =
      state.evidence[retained_plan_evidence_index(
          rec->requested_retained_plan.posture)];
  evidence.observed_display_view = true;
  evidence.provisional_display_view = provisional_display_view;
  fill_stream_observation_identity_(evidence, observed_result, observed_posture);
  if (has_display_view_elapsed_ns) {
    evidence.has_display_view_elapsed_ns = true;
    evidence.display_view_elapsed_ns = display_view_elapsed_ns;
  }
  evidence.display_view_evidence_complete =
      provisional_display_view != ResultCapability::UNSUPPORTED &&
      evidence.has_display_view_elapsed_ns;
  evidence.display_view_evidence_accepted =
      evidence.display_view_evidence_complete;
  for (uint8_t i = 0; i < state.candidate_count; ++i) {
    const CoreRetainedProductionPlan candidate = state.candidate_sequence[i];
    if (!candidate.valid || candidate.posture == expected_plan.posture) {
      continue;
    }
    const MeasuredPlanEvidence& other =
        state.evidence[retained_plan_evidence_index(candidate.posture)];
    if (other.display_view_evidence_accepted &&
        same_observation_identity_(other, evidence)) {
      evidence.display_view_evidence_accepted = false;
      evidence.display_view_evidence_complete = false;
      return;
    }
  }

  auto finalize_stream_evaluation =
      [&](CoreRetainedProductionPlan fallback_plan,
          BackingPlanEvaluationCompletionReason completion_reason) {
        CoreRetainedProductionPlan chosen{};
        const MeasuredPlanEvidence* chosen_evidence = nullptr;
        for (uint8_t i = 0; i < state.candidate_count; ++i) {
          const CoreRetainedProductionPlan candidate =
              state.candidate_sequence[i];
          const MeasuredPlanEvidence& candidate_evidence =
              state.evidence[retained_plan_evidence_index(candidate.posture)];
          if (!plan_is_strictly_better_for_stream_(
                  candidate_evidence, chosen_evidence)) {
            continue;
          }
          chosen = candidate;
          chosen_evidence = &candidate_evidence;
        }
        if (!chosen.valid) {
          chosen = fallback_plan.valid ? fallback_plan : state.candidate_sequence[0];
        }

        if (!same_retained_plan(rec->requested_retained_plan, chosen)) {
          (void)streams_.set_requested_retained_plan(stream_id, chosen, true);
          if (ICameraProvider* prov = provider_.load(std::memory_order_acquire)) {
            (void)prov->update_stream_retained_production_plan(stream_id, chosen);
          }
        }
        (void)streams_.set_steady_retained_plan(stream_id, chosen);
        state.completion_reason = completion_reason;
        RetainedPlanDecisionProvenance provenance =
            build_decision_provenance_(state, chosen);
        stream_retained_plan_decisions_[stream_id] = provenance;
        stream_retained_plan_evaluators_.erase(state_it);
        (void)refresh_capture_retained_plan_state_(
            rec->device_instance_id,
            /*requested_bump_access_posture_epoch=*/false);
      };

  if (state.current_candidate_index + 1u < state.candidate_count) {
    if (provisional_display_view == ResultCapability::UNSUPPORTED ||
        has_display_view_elapsed_ns) {
      const CoreRetainedProductionPlan next_plan =
          state.candidate_sequence[state.current_candidate_index + 1u];
      const bool crosses_display_backing_family =
          rec->requested_retained_plan.primary_cpu() != next_plan.primary_cpu();
      if (crosses_display_backing_family) {
        const uint64_t now_ns = ns_since_epoch_();
        if (result_store_.is_stream_display_demand_active(stream_id, now_ns)) {
          // Once a live display view is being held by public consumers, do not
          // flip the stream across CPU/GPU display families mid-evaluation.
          // That family transition can invalidate an already published live
          // display binding. Preserve the best evidence gathered so far and
          // settle within the current family.
          finalize_stream_evaluation(
              rec->requested_retained_plan,
              BackingPlanEvaluationCompletionReason::
                  LiveDisplayDemandFamilyCrossing);
          return;
        }
      }
      ++state.current_candidate_index;
      if (!same_retained_plan(rec->requested_retained_plan, next_plan)) {
        (void)streams_.set_requested_retained_plan(stream_id, next_plan, true);
        (void)streams_.clear_steady_retained_plan(stream_id);
        if (ICameraProvider* prov =
                provider_.load(std::memory_order_acquire)) {
          (void)prov->update_stream_retained_production_plan(
              stream_id, next_plan);
        }
        (void)refresh_capture_retained_plan_state_(
            rec->device_instance_id,
            /*requested_bump_access_posture_epoch=*/false);
      }
    }
    return;
  }
  finalize_stream_evaluation(
      state.candidate_sequence[0],
      BackingPlanEvaluationCompletionReason::AllViableCandidatesEvaluated);
}

void CoreRuntime::enqueue_pending_capture_observation_(
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
    uint64_t not_before_ns) {
  assert(core_thread_.is_core_thread());
  pending_capture_observations_.push_back(PendingCaptureObservation{
      device_instance_id,
      capture_id,
      acquisition_session_id_hint,
      posture_id,
      provisional_to_image,
      has_materialization_elapsed_ns,
      materialization_elapsed_ns,
      has_normalized_cost_units,
      normalized_cost_units,
      image_member_index,
      deferred_retries_remaining,
      not_before_ns});
  core_thread_.request_timer_tick();
}

void CoreRuntime::process_pending_capture_observations_(
    uint64_t now_ns,
    bool& has_next_delay,
    uint64_t& next_delay_ns) {
  assert(core_thread_.is_core_thread());

  std::deque<PendingCaptureObservation> due;
  for (auto it = pending_capture_observations_.begin();
       it != pending_capture_observations_.end();) {
    if (it->not_before_ns <= now_ns) {
      due.push_back(*it);
      it = pending_capture_observations_.erase(it);
      continue;
    }
    const uint64_t remaining_ns = it->not_before_ns - now_ns;
    if (!has_next_delay || remaining_ns < next_delay_ns) {
      has_next_delay = true;
      next_delay_ns = remaining_ns;
    }
    ++it;
  }

  while (!due.empty()) {
    PendingCaptureObservation pending = due.front();
    due.pop_front();
    handle_capture_retained_to_image_observation_(
        pending.device_instance_id,
        pending.capture_id,
        pending.acquisition_session_id_hint,
        pending.posture_id,
        pending.provisional_to_image,
        pending.has_materialization_elapsed_ns,
        pending.materialization_elapsed_ns,
        pending.has_normalized_cost_units,
        pending.normalized_cost_units,
        pending.image_member_index,
        pending.deferred_retries_remaining);
  }

  for (const PendingCaptureObservation& pending : pending_capture_observations_) {
    if (pending.not_before_ns <= now_ns) {
      continue;
    }
    const uint64_t remaining_ns = pending.not_before_ns - now_ns;
    if (!has_next_delay || remaining_ns < next_delay_ns) {
      has_next_delay = true;
      next_delay_ns = remaining_ns;
    }
  }
}

void CoreRuntime::mark_capture_retained_plan_state_orphaned_for_device_(
    uint64_t device_instance_id,
    uint64_t retire_after_ns) {
  assert(core_thread_.is_core_thread());
  if (device_instance_id == 0 || retire_after_ns == 0) {
    return;
  }
  for (auto& [key, state] : capture_retained_plan_evaluators_) {
    if (state.device_instance_id == device_instance_id ||
        (key.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming &&
         key.id == device_instance_id)) {
      state.orphan_retire_after_ns = retire_after_ns;
    }
  }
  for (auto& [key, decision] : capture_retained_plan_decisions_) {
    if (decision.device_instance_id == device_instance_id ||
        (key.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming &&
         key.id == device_instance_id)) {
      decision.orphan_retire_after_ns = retire_after_ns;
    }
  }
}

size_t CoreRuntime::retire_expired_capture_retained_plan_orphans_(
    uint64_t now_ns) {
  assert(core_thread_.is_core_thread());
  size_t retired = 0;
  for (auto it = capture_retained_plan_evaluators_.begin();
       it != capture_retained_plan_evaluators_.end();) {
    if (it->second.orphan_retire_after_ns != 0 &&
        it->second.orphan_retire_after_ns <= now_ns) {
      it = capture_retained_plan_evaluators_.erase(it);
      ++retired;
    } else {
      ++it;
    }
  }
  for (auto it = capture_retained_plan_decisions_.begin();
       it != capture_retained_plan_decisions_.end();) {
    if (it->second.orphan_retire_after_ns != 0 &&
        it->second.orphan_retire_after_ns <= now_ns) {
      it = capture_retained_plan_decisions_.erase(it);
      ++retired;
    } else {
      ++it;
    }
  }
  return retired;
}

void CoreRuntime::next_capture_retained_plan_orphan_retirement_delay_(
    uint64_t now_ns,
    bool& has_next_delay,
    uint64_t& next_delay_ns) const {
  for (const auto& [key, state] : capture_retained_plan_evaluators_) {
    (void)key;
    if (state.orphan_retire_after_ns == 0 ||
        state.orphan_retire_after_ns <= now_ns) {
      continue;
    }
    const uint64_t remaining_ns = state.orphan_retire_after_ns - now_ns;
    if (!has_next_delay || remaining_ns < next_delay_ns) {
      has_next_delay = true;
      next_delay_ns = remaining_ns;
    }
  }
  for (const auto& [key, decision] : capture_retained_plan_decisions_) {
    (void)key;
    if (decision.orphan_retire_after_ns == 0 ||
        decision.orphan_retire_after_ns <= now_ns) {
      continue;
    }
    const uint64_t remaining_ns = decision.orphan_retire_after_ns - now_ns;
    if (!has_next_delay || remaining_ns < next_delay_ns) {
      has_next_delay = true;
      next_delay_ns = remaining_ns;
    }
  }
}

void CoreRuntime::handle_capture_retained_to_image_observation_(
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
    uint8_t deferred_retries_remaining) {
  assert(core_thread_.is_core_thread());
  if (device_instance_id == 0 || posture_id == 0) {
    return;
  }
  bool has_real_session_parent_state = false;
  for (const auto& [key, state] : capture_retained_plan_evaluators_) {
    if (state.device_instance_id != device_instance_id) {
      continue;
    }
    if (key.kind == CaptureRetainedPlanParentKey::Kind::AcquisitionSession ||
        state.acquisition_session_id != 0) {
      has_real_session_parent_state = true;
      break;
    }
  }
  if (!has_real_session_parent_state) {
    for (const auto& [key, decision] : capture_retained_plan_decisions_) {
      if (decision.device_instance_id != device_instance_id) {
        continue;
      }
      if (key.kind == CaptureRetainedPlanParentKey::Kind::AcquisitionSession ||
          decision.acquisition_session_id != 0) {
        has_real_session_parent_state = true;
        break;
      }
    }
  }
  const auto priming_state_it =
      capture_parent_priming_states_.find(device_instance_id);
  const bool keep_priming_parent =
      priming_state_it != capture_parent_priming_states_.end() &&
      priming_state_it->second.provider_hold_active &&
      !has_real_session_parent_state;
  const bool allow_session_parent_observation = !keep_priming_parent;
  uint64_t observed_session_id = 0;
  auto state_it = capture_retained_plan_evaluators_.end();
  if (allow_session_parent_observation && acquisition_session_id_hint != 0) {
    observed_session_id = acquisition_session_id_hint;
    const CaptureRetainedPlanParentKey parent_key{
        CaptureRetainedPlanParentKey::Kind::AcquisitionSession,
        observed_session_id};
    state_it = capture_retained_plan_evaluators_.find(parent_key);
  }
  if (allow_session_parent_observation &&
      capture_id != 0 &&
      state_it == capture_retained_plan_evaluators_.end()) {
    observed_session_id =
        acquisition_sessions_.resolve_session_id_for_capture(
            device_instance_id, capture_id, acquisition_session_id_hint);
    if (observed_session_id != 0) {
      const CaptureRetainedPlanParentKey parent_key{
          CaptureRetainedPlanParentKey::Kind::AcquisitionSession,
          observed_session_id};
      state_it = capture_retained_plan_evaluators_.find(parent_key);
    }
  }
  if (state_it == capture_retained_plan_evaluators_.end()) {
    const ResolvedCaptureRetainedPlanParent parent =
        resolve_capture_retained_plan_parent_(device_instance_id);
    state_it = capture_retained_plan_evaluators_.find(parent.key);
  }
  if (state_it == capture_retained_plan_evaluators_.end()) {
    for (auto it = capture_retained_plan_evaluators_.begin();
         it != capture_retained_plan_evaluators_.end();
         ++it) {
      if (it->second.device_instance_id == device_instance_id) {
        state_it = it;
        break;
      }
    }
  }
  if (state_it != capture_retained_plan_evaluators_.end() &&
      observed_session_id != 0 &&
      !keep_priming_parent &&
      state_it->first.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming &&
      state_it->second.acquisition_session_id == 0) {
    RetainedPlanEvaluatorState migrated_state = state_it->second;
    migrated_state.acquisition_session_id = observed_session_id;
    capture_retained_plan_evaluators_.erase(state_it);
    const CaptureRetainedPlanParentKey session_key{
        CaptureRetainedPlanParentKey::Kind::AcquisitionSession,
        observed_session_id};
    state_it =
        capture_retained_plan_evaluators_.insert_or_assign(
            session_key, migrated_state).first;
  }
  const CoreDeviceRegistry::DeviceRecord* rec = devices_.find(device_instance_id);
  if (state_it == capture_retained_plan_evaluators_.end()) {
    if (deferred_retries_remaining != 0) {
      const uint64_t now_ns = ns_since_epoch_();
      enqueue_pending_capture_observation_(
          device_instance_id,
          capture_id,
          acquisition_session_id_hint,
          posture_id,
          provisional_to_image,
          has_materialization_elapsed_ns,
          materialization_elapsed_ns,
          has_normalized_cost_units,
          normalized_cost_units,
          image_member_index,
          static_cast<uint8_t>(deferred_retries_remaining - 1u),
          now_ns + kCaptureObservationRetryDelayNs);
      capture_latency_trace_printf(
          "capture_plan_observation_deferred capture_id=%llu device_id=%llu posture_id=%llu observed_session_id=%llu image_member_index=%u retries_remaining=%u delay_ns=%llu",
          static_cast<unsigned long long>(capture_id),
          static_cast<unsigned long long>(device_instance_id),
          static_cast<unsigned long long>(posture_id),
          static_cast<unsigned long long>(observed_session_id),
          static_cast<unsigned>(image_member_index),
          static_cast<unsigned>(deferred_retries_remaining),
          static_cast<unsigned long long>(kCaptureObservationRetryDelayNs));
      return;
    }
    size_t matching_state_count = 0;
    CaptureRetainedPlanParentKey first_matching_key{};
    for (const auto& [key, state] : capture_retained_plan_evaluators_) {
      if (state.device_instance_id != device_instance_id) {
        continue;
      }
      if (matching_state_count == 0) {
        first_matching_key = key;
      }
      ++matching_state_count;
    }
    const ResolvedCaptureRetainedPlanParent resolved_parent =
        resolve_capture_retained_plan_parent_(device_instance_id);
    capture_latency_trace_printf(
        "capture_plan_observation_ignored capture_id=%llu device_id=%llu posture_id=%llu observed_session_id=%llu image_member_index=%u reason=no_state_or_requested_plan device_exists=%u resolved_parent_kind=%u resolved_parent_id=%llu matching_state_count=%llu first_matching_parent_kind=%u first_matching_parent_id=%llu",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>(posture_id),
        static_cast<unsigned long long>(observed_session_id),
        static_cast<unsigned>(image_member_index),
        rec != nullptr ? 1u : 0u,
        static_cast<unsigned>(resolved_parent.key.kind),
        static_cast<unsigned long long>(resolved_parent.key.id),
        static_cast<unsigned long long>(matching_state_count),
        static_cast<unsigned>(first_matching_key.kind),
        static_cast<unsigned long long>(first_matching_key.id));
    return;
  }

  RetainedPlanEvaluatorState& state = state_it->second;
  if (!state.active || state.current_candidate_index >= state.candidate_count) {
    capture_latency_trace_printf(
        "capture_plan_observation_ignored capture_id=%llu device_id=%llu posture_id=%llu observed_session_id=%llu image_member_index=%u parent_kind=%u parent_id=%llu reason=inactive_or_oob current_candidate_index=%u candidate_count=%u",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>(posture_id),
        static_cast<unsigned long long>(observed_session_id),
        static_cast<unsigned>(image_member_index),
        static_cast<unsigned>(state_it->first.kind),
        static_cast<unsigned long long>(state_it->first.id),
        static_cast<unsigned>(state.current_candidate_index),
        static_cast<unsigned>(state.candidate_count));
    return;
  }
  const uint64_t now_ns = ns_since_epoch_();
  CoreRetainedProductionPlan effective_requested{};
  if (state.acquisition_session_id != 0) {
    if (const auto* session =
            acquisition_sessions_.find(state.acquisition_session_id);
        session != nullptr &&
        session->requested_retained_plan.valid) {
      effective_requested = session->requested_retained_plan;
    }
  }
  if (!effective_requested.valid && rec != nullptr &&
      rec->requested_retained_plan.valid) {
    effective_requested = rec->requested_retained_plan;
  }
  const CoreRetainedProductionPlan expected_plan =
      state.candidate_sequence[state.current_candidate_index];
  if (!effective_requested.valid && expected_plan.valid) {
    effective_requested = expected_plan;
  }
  if (!effective_requested.valid) {
    capture_latency_trace_printf(
        "capture_plan_observation_ignored capture_id=%llu device_id=%llu posture_id=%llu observed_session_id=%llu image_member_index=%u parent_kind=%u parent_id=%llu reason=no_requested_plan current_candidate_index=%u",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>(posture_id),
        static_cast<unsigned long long>(observed_session_id),
        static_cast<unsigned>(image_member_index),
        static_cast<unsigned>(state_it->first.kind),
        static_cast<unsigned long long>(state_it->first.id),
        static_cast<unsigned>(state.current_candidate_index));
    return;
  }
  if (!same_retained_plan(effective_requested, expected_plan)) {
    capture_latency_trace_printf(
        "capture_plan_observation_ignored capture_id=%llu device_id=%llu posture_id=%llu observed_session_id=%llu image_member_index=%u parent_kind=%u parent_id=%llu reason=requested_mismatch current_candidate_index=%u expected_posture=%d requested_posture=%d",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>(posture_id),
        static_cast<unsigned long long>(observed_session_id),
        static_cast<unsigned>(image_member_index),
        static_cast<unsigned>(state_it->first.kind),
        static_cast<unsigned long long>(state_it->first.id),
        static_cast<unsigned>(state.current_candidate_index),
        static_cast<int>(expected_plan.posture),
        static_cast<int>(effective_requested.posture));
    return;
  }
  if (state.current_candidate_ready_after_ns != 0 &&
      now_ns < state.current_candidate_ready_after_ns) {
    enqueue_pending_capture_observation_(
        device_instance_id,
        capture_id,
        acquisition_session_id_hint,
        posture_id,
        provisional_to_image,
        has_materialization_elapsed_ns,
        materialization_elapsed_ns,
        has_normalized_cost_units,
        normalized_cost_units,
        image_member_index,
        deferred_retries_remaining,
        state.current_candidate_ready_after_ns);
    return;
  }
  const CaptureStillImageBundle& required_bundle =
      state.capture_priming_seed_signature.still_image_bundle;
  if (required_bundle.members.empty()) {
    return;
  }
  const uint32_t required_member_count =
      static_cast<uint32_t>(required_bundle.members.size());
  const SharedCaptureResultData observed_result =
      result_store_.get_capture_result(capture_id, device_instance_id);
  const CoreCaptureResultData::ImageMemberData* observed_member =
      observed_result ? observed_result->image_member_at(image_member_index)
                      : nullptr;
  CoreProductionPostureShape observed_posture{};
  if (!observed_result ||
      observed_result->image_member_count() < required_member_count ||
      observed_member == nullptr ||
      !capture_member_matches_required_bundle_(required_bundle, *observed_member) ||
      observed_result->acquisition_session_id == 0 ||
      observed_result->acquisition_session_id != state.acquisition_session_id ||
      observed_member->access_posture.posture_id != posture_id ||
      !infer_capture_member_posture_shape_(*observed_member, observed_posture) ||
      observed_posture != expected_plan.posture) {
    capture_latency_trace_printf(
        "capture_plan_observation_ignored capture_id=%llu device_id=%llu posture_id=%llu observed_session_id=%llu image_member_index=%u parent_kind=%u parent_id=%llu reason=attribution_mismatch current_candidate_index=%u expected_posture=%d",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>(posture_id),
        static_cast<unsigned long long>(observed_session_id),
        static_cast<unsigned>(image_member_index),
        static_cast<unsigned>(state_it->first.kind),
        static_cast<unsigned long long>(state_it->first.id),
        static_cast<unsigned>(state.current_candidate_index),
        static_cast<int>(expected_plan.posture));
    return;
  }

  MeasuredPlanEvidence& evidence =
      state.evidence[retained_plan_evidence_index(
          expected_plan.posture)];
  if (evidence.observed_to_image &&
      evidence.has_observed_posture) {
    MeasuredPlanEvidence current_observation;
    fill_capture_observation_identity_(
        current_observation, observed_result, *observed_member, observed_posture);
    if (!same_capture_observation_family_(evidence, current_observation)) {
      capture_latency_trace_printf(
          "capture_plan_observation_ignored capture_id=%llu device_id=%llu posture_id=%llu observed_session_id=%llu image_member_index=%u parent_kind=%u parent_id=%llu reason=family_mismatch current_candidate_index=%u expected_posture=%d",
          static_cast<unsigned long long>(capture_id),
          static_cast<unsigned long long>(device_instance_id),
          static_cast<unsigned long long>(posture_id),
          static_cast<unsigned long long>(observed_session_id),
          static_cast<unsigned>(image_member_index),
          static_cast<unsigned>(state_it->first.kind),
          static_cast<unsigned long long>(state_it->first.id),
          static_cast<unsigned>(state.current_candidate_index),
          static_cast<int>(expected_plan.posture));
      return;
    }
  }
  evidence.observed_to_image = true;
  if (!evidence.has_observed_posture) {
    fill_capture_observation_identity_(
        evidence, observed_result, *observed_member, observed_posture);
  }
  if (evidence.capture_member_materialization.size() <
      required_bundle.members.size()) {
    evidence.capture_member_materialization.resize(required_bundle.members.size());
  }
  auto& member_evidence =
      evidence.capture_member_materialization[observed_member->image_member_index];
  member_evidence.observed = true;
  member_evidence.provisional_to_image = provisional_to_image;
  if (has_materialization_elapsed_ns) {
    member_evidence.has_materialization_elapsed_ns = true;
    member_evidence.materialization_elapsed_ns = materialization_elapsed_ns;
  }
  if (has_normalized_cost_units) {
    member_evidence.has_normalized_cost_units = true;
    member_evidence.normalized_cost_units = normalized_cost_units;
  }
  if (state.acquisition_session_id != 0) {
    if (const auto* session =
            acquisition_sessions_.find(state.acquisition_session_id);
        session != nullptr &&
        session->last_capture_id == capture_id &&
        session->last_capture_latency_ns != 0) {
      evidence.has_capture_ready_elapsed_ns = true;
      evidence.capture_ready_elapsed_ns = session->last_capture_latency_ns;
    }
  }
  recompute_capture_materialization_aggregate_(evidence, required_bundle);
  for (uint8_t i = 0; i < state.candidate_count; ++i) {
    const CoreRetainedProductionPlan candidate = state.candidate_sequence[i];
    if (!candidate.valid || candidate.posture == expected_plan.posture) {
      continue;
    }
    const MeasuredPlanEvidence& other =
        state.evidence[retained_plan_evidence_index(candidate.posture)];
    if (other.capture_evidence_accepted &&
        same_capture_observation_family_(other, evidence)) {
      evidence.capture_evidence_accepted = false;
      evidence.capture_evidence_complete = false;
      capture_latency_trace_printf(
          "capture_plan_observation_ignored capture_id=%llu device_id=%llu posture_id=%llu observed_session_id=%llu image_member_index=%u parent_kind=%u parent_id=%llu reason=duplicate_observation current_candidate_index=%u expected_posture=%d",
          static_cast<unsigned long long>(capture_id),
          static_cast<unsigned long long>(device_instance_id),
          static_cast<unsigned long long>(posture_id),
          static_cast<unsigned long long>(observed_session_id),
          static_cast<unsigned>(image_member_index),
          static_cast<unsigned>(state_it->first.kind),
          static_cast<unsigned long long>(state_it->first.id),
          static_cast<unsigned>(state.current_candidate_index),
          static_cast<int>(expected_plan.posture));
      return;
    }
  }
  if (!evidence.capture_evidence_complete &&
      provisional_to_image != ResultCapability::UNSUPPORTED &&
      deferred_retries_remaining != 0) {
    enqueue_pending_capture_observation_(
        device_instance_id,
        capture_id,
        acquisition_session_id_hint,
        posture_id,
        provisional_to_image,
        has_materialization_elapsed_ns,
        materialization_elapsed_ns,
        has_normalized_cost_units,
        normalized_cost_units,
        image_member_index,
        static_cast<uint8_t>(deferred_retries_remaining - 1u),
        now_ns + kCaptureObservationRetryDelayNs);
  }
  capture_latency_trace_printf(
      "capture_plan_observation_applied capture_id=%llu device_id=%llu posture_id=%llu observed_session_id=%llu image_member_index=%u parent_kind=%u parent_id=%llu current_candidate_index=%u requested_posture=%d provisional_to_image=%d has_materialization=%u materialization_elapsed_ns=%llu has_normalized=%u normalized_cost_units=%llu has_capture_ready=%u capture_ready_elapsed_ns=%llu",
      static_cast<unsigned long long>(capture_id),
      static_cast<unsigned long long>(device_instance_id),
      static_cast<unsigned long long>(posture_id),
      static_cast<unsigned long long>(observed_session_id),
      static_cast<unsigned>(evidence.observed_image_member_index),
      static_cast<unsigned>(state_it->first.kind),
      static_cast<unsigned long long>(state_it->first.id),
      static_cast<unsigned>(state.current_candidate_index),
      static_cast<int>(effective_requested.posture),
      static_cast<int>(provisional_to_image),
      evidence.has_materialization_elapsed_ns ? 1u : 0u,
      static_cast<unsigned long long>(evidence.has_materialization_elapsed_ns
                                          ? evidence.materialization_elapsed_ns
                                          : 0),
      evidence.has_normalized_cost_units ? 1u : 0u,
      static_cast<unsigned long long>(evidence.has_normalized_cost_units
                                          ? evidence.normalized_cost_units
                                          : 0),
      evidence.has_capture_ready_elapsed_ns ? 1u : 0u,
      static_cast<unsigned long long>(evidence.has_capture_ready_elapsed_ns
                                          ? evidence.capture_ready_elapsed_ns
                                          : 0));

  if (state.current_candidate_index + 1u < state.candidate_count) {
    if (provisional_to_image == ResultCapability::UNSUPPORTED ||
        evidence.capture_evidence_accepted) {
      ++state.current_candidate_index;
      const CoreRetainedProductionPlan next_plan =
          state.candidate_sequence[state.current_candidate_index];
      state.current_candidate_ready_after_ns =
          now_ns + capture_backing_plan_evaluation_settle_delay_ns();
      capture_latency_trace_printf(
          "capture_plan_observation_advance capture_id=%llu device_id=%llu posture_id=%llu observed_session_id=%llu next_candidate_index=%u next_posture=%d",
          static_cast<unsigned long long>(capture_id),
          static_cast<unsigned long long>(device_instance_id),
          static_cast<unsigned long long>(posture_id),
          static_cast<unsigned long long>(observed_session_id),
          static_cast<unsigned>(state.current_candidate_index),
          static_cast<int>(next_plan.posture));
      if (rec != nullptr) {
        (void)devices_.set_requested_retained_plan(device_instance_id, next_plan, true);
        (void)devices_.clear_steady_retained_plan(device_instance_id);
      }
      if (state.acquisition_session_id != 0) {
        (void)acquisition_sessions_.set_requested_retained_plan(
            state.acquisition_session_id, next_plan, true);
        (void)acquisition_sessions_.clear_steady_retained_plan(
            state.acquisition_session_id);
      }
    }
    return;
  }

  if (provisional_to_image != ResultCapability::UNSUPPORTED &&
      !evidence.capture_evidence_accepted) {
    return;
  }

  CoreRetainedProductionPlan chosen{};
  const MeasuredPlanEvidence* chosen_evidence = nullptr;
  for (uint8_t i = 0; i < state.candidate_count; ++i) {
    const CoreRetainedProductionPlan candidate = state.candidate_sequence[i];
    const MeasuredPlanEvidence& candidate_evidence =
        state.evidence[retained_plan_evidence_index(candidate.posture)];
    if (!plan_is_strictly_better_for_capture_(candidate_evidence, chosen_evidence)) {
      continue;
    }
    chosen = candidate;
    chosen_evidence = &candidate_evidence;
  }
  if (!chosen.valid) {
    return;
  }
  state.completion_reason =
      BackingPlanEvaluationCompletionReason::AllViableCandidatesEvaluated;
  capture_latency_trace_printf(
      "capture_plan_observation_finalize capture_id=%llu device_id=%llu posture_id=%llu observed_session_id=%llu chosen_posture=%d decision_from_evaluation=1",
      static_cast<unsigned long long>(capture_id),
      static_cast<unsigned long long>(device_instance_id),
      static_cast<unsigned long long>(posture_id),
      static_cast<unsigned long long>(observed_session_id),
      static_cast<int>(chosen.posture));

  if (rec != nullptr && !same_retained_plan(effective_requested, chosen)) {
    (void)devices_.set_requested_retained_plan(device_instance_id, chosen, true);
  }
  if (state.acquisition_session_id != 0) {
    (void)acquisition_sessions_.set_requested_retained_plan(
        state.acquisition_session_id, chosen, true);
  }
  if (rec != nullptr) {
    (void)devices_.set_steady_retained_plan(device_instance_id, chosen);
  }
  if (state.acquisition_session_id != 0) {
    (void)acquisition_sessions_.set_steady_retained_plan(
        state.acquisition_session_id, chosen);
  }
  const auto* session =
      state.acquisition_session_id != 0
          ? acquisition_sessions_.find(state.acquisition_session_id)
          : nullptr;
  const bool session_still_live =
      session != nullptr && session->phase == CBLifecyclePhase::LIVE;
  remember_capture_priming_seed_(
      state.capture_priming_seed_signature, chosen);
  RetainedPlanDecisionProvenance provenance =
      build_decision_provenance_(state, chosen);
  if (!session_still_live) {
    provenance.orphan_retire_after_ns =
        now_ns + kCaptureRetainedPlanOrphanRetentionWindowNs;
  } else {
    provenance.orphan_retire_after_ns = 0;
  }
  const CaptureRetainedPlanParentKey decision_key = state_it->first;
  capture_retained_plan_decisions_[decision_key] = provenance;
  capture_retained_plan_evaluators_.erase(state_it);
  release_capture_parent_priming_(
      device_instance_id,
      "capture_observation.finalize");
}



std::vector<CoreBackingPlanEvaluationReport>
CoreRuntime::backing_plan_evaluation_reports() const {
  if (core_thread_.is_core_thread()) {
    return backing_plan_evaluation_reports_on_core_thread_();
  }

  auto completion =
      std::make_shared<std::promise<std::vector<CoreBackingPlanEvaluationReport>>>();
  std::future<std::vector<CoreBackingPlanEvaluationReport>> completed =
      completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  const CoreThread::PostResult pr = self->try_post([this, completion]() {
    completion->set_value(backing_plan_evaluation_reports_on_core_thread_());
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return {};
  }
  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return {};
  }
  return completed.get();
}

std::vector<CoreCaptureLifecycleTimingReport>
CoreRuntime::recent_capture_lifecycle_timing_reports() const {
  if (core_thread_.is_core_thread()) {
    return recent_capture_lifecycle_timing_reports_on_core_thread_();
  }

  auto completion = std::make_shared<
      std::promise<std::vector<CoreCaptureLifecycleTimingReport>>>();
  std::future<std::vector<CoreCaptureLifecycleTimingReport>> completed =
      completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  const CoreThread::PostResult pr = self->try_post([this, completion]() {
    completion->set_value(recent_capture_lifecycle_timing_reports_on_core_thread_());
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return {};
  }
  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return {};
  }
  return completed.get();
}

#if defined(CAMBANG_INTERNAL_SMOKE)
std::optional<CoreRuntime::ImagingSpecRetainedStateForSmoke>
CoreRuntime::imaging_spec_retained_state_for_smoke() const {
  if (core_thread_.is_core_thread()) {
    ImagingSpecRetainedStateForSmoke out{};
    out.imaging_spec_version = spec_state_.imaging_spec_version();
    out.retention_kind = spec_state_.imaging_spec_retention_kind();
    out.payload = spec_state_.imaging_spec_payload_copy();
    return out;
  }

  auto completion =
      std::make_shared<std::promise<std::optional<ImagingSpecRetainedStateForSmoke>>>();
  std::future<std::optional<ImagingSpecRetainedStateForSmoke>> completed =
      completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  const CoreThread::PostResult pr = self->try_post([this, completion]() {
    ImagingSpecRetainedStateForSmoke out{};
    out.imaging_spec_version = spec_state_.imaging_spec_version();
    out.retention_kind = spec_state_.imaging_spec_retention_kind();
    out.payload = spec_state_.imaging_spec_payload_copy();
    completion->set_value(std::move(out));
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return std::nullopt;
  }
  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return std::nullopt;
  }
  return completed.get();
}

std::optional<ExternalCameraDescriptionEntry>
CoreRuntime::active_external_camera_description_for_smoke(const std::string& camera_id) const {
  if (camera_id.empty()) {
    return std::nullopt;
  }
  if (core_thread_.is_core_thread()) {
    const auto* entry = active_external_camera_description_.find_exact(camera_id);
    return entry ? std::optional<ExternalCameraDescriptionEntry>(*entry) : std::nullopt;
  }
  auto completion =
      std::make_shared<std::promise<std::optional<ExternalCameraDescriptionEntry>>>();
  std::future<std::optional<ExternalCameraDescriptionEntry>> completed = completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  const CoreThread::PostResult pr = self->try_post([this, camera_id, completion]() {
    const auto* entry = active_external_camera_description_.find_exact(camera_id);
    completion->set_value(entry ? std::optional<ExternalCameraDescriptionEntry>(*entry)
                                : std::nullopt);
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return std::nullopt;
  }
  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return std::nullopt;
  }
  return completed.get();
}

size_t CoreRuntime::active_external_camera_description_count_for_smoke() const {
  if (core_thread_.is_core_thread()) {
    return active_external_camera_description_.entries().size();
  }
  auto completion = std::make_shared<std::promise<size_t>>();
  std::future<size_t> completed = completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  const CoreThread::PostResult pr = self->try_post([this, completion]() {
    completion->set_value(active_external_camera_description_.entries().size());
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return 0;
  }
  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return 0;
  }
  return completed.get();
}

uint64_t CoreRuntime::active_external_camera_description_version_for_smoke() const {
  if (core_thread_.is_core_thread()) {
    return active_camera_description_version_;
  }
  auto completion = std::make_shared<std::promise<uint64_t>>();
  std::future<uint64_t> completed = completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  const CoreThread::PostResult pr = self->try_post([this, completion]() {
    completion->set_value(active_camera_description_version_);
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return 0;
  }
  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return 0;
  }
  return completed.get();
}
#endif

std::vector<CoreBackingPlanEvaluationReport>
CoreRuntime::backing_plan_evaluation_reports_on_core_thread_() const {
  assert(core_thread_.is_core_thread());
  std::vector<CoreBackingPlanEvaluationReport> out;
  out.reserve(streams_.all().size() + devices_.all().size());

  for (const auto& [stream_id, rec] : streams_.all()) {
    CoreBackingPlanEvaluationReport report{};
    report.parent_kind = CoreBackingPlanEvaluationReport::ParentKind::Stream;
    report.parent_id = stream_id;
    report.stream_id = stream_id;
    report.device_instance_id = rec.device_instance_id;
    report.primary_function =
        BackingPlanEvaluationPrimaryFunction::StreamDisplayView;
    report.target_kind = CoreBackingPlanEvaluationReport::TargetKind::Stream;
    report.target_id = stream_id;
    report.requested = rec.requested_retained_plan;
    report.steady = rec.steady_retained_plan;
    if (const auto it = stream_retained_plan_evaluators_.find(stream_id);
        it != stream_retained_plan_evaluators_.end()) {
      const RetainedPlanEvaluatorState& state = it->second;
      report.primary_function = state.primary_function;
      report.evaluator_active = state.active;
      report.current_candidate_index = state.current_candidate_index;
      report.completion_reason = state.completion_reason;
      report.candidate_sequence.reserve(state.candidate_count);
      report.candidate_evidence.reserve(state.candidate_count);
      for (uint8_t i = 0; i < state.candidate_count; ++i) {
        report.candidate_sequence.push_back(state.candidate_sequence[i]);
        report.candidate_evidence.push_back(
            build_candidate_evidence_report_(
                state.candidate_sequence[i],
                state.evidence[retained_plan_evidence_index(
                    state.candidate_sequence[i].posture)]));
      }
    }
    if (const auto decision_it = stream_retained_plan_decisions_.find(stream_id);
        decision_it != stream_retained_plan_decisions_.end()) {
      const RetainedPlanDecisionProvenance& decision = decision_it->second;
      report.decision_from_evaluation = decision.from_evaluation;
      report.decision_selected = decision.selected;
      report.completion_reason = decision.completion_reason;
      report.decision_candidate_sequence.reserve(decision.candidate_count);
      report.candidate_evidence.clear();
      report.candidate_evidence.reserve(decision.candidate_count);
      for (uint8_t i = 0; i < decision.candidate_count; ++i) {
        report.decision_candidate_sequence.push_back(
            decision.candidate_sequence[i]);
        report.candidate_evidence.push_back(
            build_candidate_evidence_report_(
                decision.candidate_sequence[i],
                decision.evidence[i]));
      }
    }
    out.push_back(std::move(report));
  }

  std::map<CaptureRetainedPlanParentKey, bool> emitted_capture_parents;
  for (const auto& [device_instance_id, rec] : devices_.all()) {
    const ResolvedCaptureRetainedPlanParent parent =
        resolve_capture_retained_plan_parent_(device_instance_id);
    if (emitted_capture_parents.find(parent.key) != emitted_capture_parents.end()) {
      continue;
    }
    emitted_capture_parents[parent.key] = true;

    CoreBackingPlanEvaluationReport report{};
    report.parent_kind = parent.provisional
        ? CoreBackingPlanEvaluationReport::ParentKind::CapturePriming
        : CoreBackingPlanEvaluationReport::ParentKind::AcquisitionSession;
    report.parent_id = parent.key.id;
    report.acquisition_session_id = parent.acquisition_session_id;
    report.device_instance_id = device_instance_id;
    report.provisional_parent = parent.provisional;
    report.primary_function =
        BackingPlanEvaluationPrimaryFunction::CaptureReadyAndMaterialize;
    report.target_kind = CoreBackingPlanEvaluationReport::TargetKind::Capture;
    report.target_id = device_instance_id;
    if (parent.acquisition_session_id != 0) {
      if (const auto* session =
              acquisition_sessions_.find(parent.acquisition_session_id);
          session != nullptr) {
        report.requested = session->requested_retained_plan;
        report.steady = session->steady_retained_plan;
      } else {
        report.requested = rec.requested_retained_plan;
        report.steady = rec.steady_retained_plan;
      }
    } else {
      report.requested = rec.requested_retained_plan;
      report.steady = rec.steady_retained_plan;
    }
    if (const auto it = capture_retained_plan_evaluators_.find(parent.key);
        it != capture_retained_plan_evaluators_.end()) {
      const RetainedPlanEvaluatorState& state = it->second;
      report.acquisition_session_id = state.acquisition_session_id;
      report.primary_function = state.primary_function;
      report.evaluator_active = state.active;
      report.current_candidate_index = state.current_candidate_index;
      report.completion_reason = state.completion_reason;
      report.candidate_sequence.reserve(state.candidate_count);
      report.candidate_evidence.reserve(state.candidate_count);
      for (uint8_t i = 0; i < state.candidate_count; ++i) {
        report.candidate_sequence.push_back(state.candidate_sequence[i]);
        report.candidate_evidence.push_back(
            build_candidate_evidence_report_(
                state.candidate_sequence[i],
                state.evidence[retained_plan_evidence_index(
                    state.candidate_sequence[i].posture)]));
      }
    }
    if (const auto decision_it =
            capture_retained_plan_decisions_.find(parent.key);
        decision_it != capture_retained_plan_decisions_.end()) {
      const RetainedPlanDecisionProvenance& decision = decision_it->second;
      report.decision_from_evaluation = decision.from_evaluation;
      report.decision_selected = decision.selected;
      report.completion_reason = decision.completion_reason;
      report.decision_candidate_sequence.reserve(decision.candidate_count);
      report.candidate_evidence.clear();
      report.candidate_evidence.reserve(decision.candidate_count);
      for (uint8_t i = 0; i < decision.candidate_count; ++i) {
        report.decision_candidate_sequence.push_back(
            decision.candidate_sequence[i]);
        report.candidate_evidence.push_back(
            build_candidate_evidence_report_(
                decision.candidate_sequence[i],
                decision.evidence[i]));
      }
    }
    if (!report.evaluator_active &&
        !report.decision_selected.valid &&
        parent.acquisition_session_id != 0) {
      const CaptureRetainedPlanParentKey priming_key{
          CaptureRetainedPlanParentKey::Kind::CapturePriming,
          device_instance_id};
      if (const auto decision_it =
              capture_retained_plan_decisions_.find(priming_key);
          decision_it != capture_retained_plan_decisions_.end()) {
        const RetainedPlanDecisionProvenance& decision = decision_it->second;
        if (decision.device_instance_id == device_instance_id &&
            decision.valid &&
            decision.acquisition_session_id == 0) {
          report.decision_from_evaluation = decision.from_evaluation;
          report.decision_selected = decision.selected;
          report.completion_reason = decision.completion_reason;
          report.decision_candidate_sequence.clear();
          report.decision_candidate_sequence.reserve(decision.candidate_count);
          report.candidate_evidence.clear();
          report.candidate_evidence.reserve(decision.candidate_count);
          for (uint8_t i = 0; i < decision.candidate_count; ++i) {
            report.decision_candidate_sequence.push_back(
                decision.candidate_sequence[i]);
            report.candidate_evidence.push_back(
                build_candidate_evidence_report_(
                    decision.candidate_sequence[i],
                    decision.evidence[i]));
          }
        }
      }
    }
    out.push_back(std::move(report));
  }

  std::map<uint64_t, bool> emitted_orphan_capture_targets;
  auto emit_orphan_capture_report_from_evaluator =
      [&](const CaptureRetainedPlanParentKey& key,
          const RetainedPlanEvaluatorState& state) {
        if (state.device_instance_id == 0 ||
            devices_.find(state.device_instance_id) != nullptr ||
            emitted_orphan_capture_targets.find(state.device_instance_id) !=
                emitted_orphan_capture_targets.end()) {
          return;
        }
        emitted_orphan_capture_targets[state.device_instance_id] = true;
        CoreBackingPlanEvaluationReport report{};
        report.parent_kind =
            key.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming
                ? CoreBackingPlanEvaluationReport::ParentKind::CapturePriming
                : CoreBackingPlanEvaluationReport::ParentKind::AcquisitionSession;
        report.parent_id = key.id;
        report.acquisition_session_id = state.acquisition_session_id;
        report.device_instance_id = state.device_instance_id;
        report.provisional_parent =
            key.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming;
        report.primary_function = state.primary_function;
        report.target_kind =
            CoreBackingPlanEvaluationReport::TargetKind::Capture;
        report.target_id = state.device_instance_id;
        if (state.current_candidate_index < state.candidate_count) {
          report.requested =
              state.candidate_sequence[state.current_candidate_index];
        }
        report.evaluator_active = state.active;
        report.current_candidate_index = state.current_candidate_index;
        report.completion_reason = state.completion_reason;
        report.candidate_sequence.reserve(state.candidate_count);
        report.candidate_evidence.reserve(state.candidate_count);
        for (uint8_t i = 0; i < state.candidate_count; ++i) {
          report.candidate_sequence.push_back(state.candidate_sequence[i]);
          report.candidate_evidence.push_back(
              build_candidate_evidence_report_(
                  state.candidate_sequence[i],
                  state.evidence[retained_plan_evidence_index(
                      state.candidate_sequence[i].posture)]));
        }
        out.push_back(std::move(report));
      };
  auto emit_orphan_capture_report_from_decision =
      [&](const CaptureRetainedPlanParentKey& key,
          const RetainedPlanDecisionProvenance& decision) {
        if (decision.device_instance_id == 0 ||
            devices_.find(decision.device_instance_id) != nullptr ||
            emitted_orphan_capture_targets.find(decision.device_instance_id) !=
                emitted_orphan_capture_targets.end()) {
          return;
        }
        emitted_orphan_capture_targets[decision.device_instance_id] = true;
        CoreBackingPlanEvaluationReport report{};
        report.parent_kind =
            key.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming
                ? CoreBackingPlanEvaluationReport::ParentKind::CapturePriming
                : CoreBackingPlanEvaluationReport::ParentKind::AcquisitionSession;
        report.parent_id = key.id;
        report.acquisition_session_id = decision.acquisition_session_id;
        report.device_instance_id = decision.device_instance_id;
        report.provisional_parent =
            key.kind == CaptureRetainedPlanParentKey::Kind::CapturePriming;
        report.primary_function = decision.primary_function;
        report.target_kind =
            CoreBackingPlanEvaluationReport::TargetKind::Capture;
        report.target_id = decision.device_instance_id;
        report.requested = decision.selected;
        report.steady = decision.selected;
        report.decision_from_evaluation = decision.from_evaluation;
        report.decision_selected = decision.selected;
        report.completion_reason = decision.completion_reason;
        report.decision_candidate_sequence.reserve(decision.candidate_count);
        report.candidate_evidence.reserve(decision.candidate_count);
        for (uint8_t i = 0; i < decision.candidate_count; ++i) {
          report.decision_candidate_sequence.push_back(
              decision.candidate_sequence[i]);
          report.candidate_evidence.push_back(
              build_candidate_evidence_report_(
                  decision.candidate_sequence[i],
                  decision.evidence[i]));
        }
        out.push_back(std::move(report));
      };
  for (const auto& [key, decision] : capture_retained_plan_decisions_) {
    emit_orphan_capture_report_from_decision(key, decision);
  }
  for (const auto& [key, state] : capture_retained_plan_evaluators_) {
    emit_orphan_capture_report_from_evaluator(key, state);
  }

  return out;
}

std::vector<CoreCaptureLifecycleTimingReport>
CoreRuntime::recent_capture_lifecycle_timing_reports_on_core_thread_() const {
  assert(core_thread_.is_core_thread());
  std::vector<CoreCaptureLifecycleTimingReport> out;
  out.reserve(recent_capture_lifecycle_timing_order_.size());
  for (const auto& key : recent_capture_lifecycle_timing_order_) {
    const auto it = recent_capture_lifecycle_timing_reports_.find(key);
    if (it == recent_capture_lifecycle_timing_reports_.end()) {
      continue;
    }
    out.push_back(it->second);
  }
  return out;
}

void CoreRuntime::note_capture_lifecycle_ingress_(
    const CoreCaptureLifecycleIngressEvent& event) {
  assert(core_thread_.is_core_thread());
  const std::pair<uint64_t, uint64_t> key{
      event.capture_id, event.device_instance_id};
  auto it = recent_capture_lifecycle_timing_reports_.find(key);
  if (it == recent_capture_lifecycle_timing_reports_.end()) {
    CoreCaptureLifecycleTimingReport report{};
    report.capture_id = event.capture_id;
    report.device_instance_id = event.device_instance_id;
    it = recent_capture_lifecycle_timing_reports_
             .emplace(key, std::move(report))
             .first;
    recent_capture_lifecycle_timing_order_.push_back(key);
    constexpr size_t kMaxRecentCaptureLifecycleTimingReports = 256;
    if (recent_capture_lifecycle_timing_order_.size() >
        kMaxRecentCaptureLifecycleTimingReports) {
      const std::pair<uint64_t, uint64_t> oldest =
          recent_capture_lifecycle_timing_order_.front();
      recent_capture_lifecycle_timing_order_.erase(
          recent_capture_lifecycle_timing_order_.begin());
      recent_capture_lifecycle_timing_reports_.erase(oldest);
      it = recent_capture_lifecycle_timing_reports_.find(key);
      if (it == recent_capture_lifecycle_timing_reports_.end()) {
        return;
      }
    }
  }

  CoreCaptureLifecycleTimingReport& report = it->second;
  report.capture_id = event.capture_id;
  report.device_instance_id = event.device_instance_id;
  if (event.acquisition_session_id != 0) {
    report.acquisition_session_id = event.acquisition_session_id;
  }
  switch (event.kind) {
    case CoreCaptureLifecycleIngressEvent::Kind::Started:
      report.has_capture_started_ingested_steady_ns = true;
      report.capture_started_ingested_steady_ns = event.ingest_steady_ns;
      break;
    case CoreCaptureLifecycleIngressEvent::Kind::Completed:
      report.has_capture_completed_ingested_steady_ns = true;
      report.capture_completed_ingested_steady_ns = event.ingest_steady_ns;
      finalize_completed_capture_facts_(event.capture_id, event.device_instance_id);
      break;
    case CoreCaptureLifecycleIngressEvent::Kind::Failed:
      report.has_capture_failed_ingested_steady_ns = true;
      report.capture_failed_ingested_steady_ns = event.ingest_steady_ns;
      break;
  }
}

CoreResolvedCaptureImageFacts CoreRuntime::resolve_capture_image_facts_(
    uint64_t capture_id,
    uint64_t device_instance_id,
    uint32_t image_member_index) const {
  assert(core_thread_.is_core_thread());
  const CoreDeviceRegistry::DeviceRecord* device = devices_.find(device_instance_id);
  const ExternalCameraDescriptionEntry* external =
      device ? active_external_camera_description_.find_exact(device->hardware_id) : nullptr;
  const ProviderCameraFacts* provider_static =
      provider_camera_fact_state_.find_static(device_instance_id);
  const ProviderCameraFactState::CaptureImageKey image_key{
      capture_id, device_instance_id, image_member_index};
  const ProviderCaptureImageFacts* provider_image =
      provider_camera_fact_state_.find_capture_image(image_key);

  CoreResolvedCaptureImageFacts resolved{};
  const CameraStaticFacts* external_facts = external ? &external->facts : nullptr;
  const CameraStaticFacts* static_facts =
      provider_static ? &provider_static->static_facts : nullptr;
  resolved.camera.facing = external_facts && external_facts->facing
      ? external_facts->facing
      : static_facts ? static_facts->facing : std::nullopt;
  resolved.camera.nature = external_facts && external_facts->nature
      ? external_facts->nature
      : static_facts ? static_facts->nature : std::nullopt;
  resolved.camera.sensor_orientation = external_facts && external_facts->sensor_orientation
      ? external_facts->sensor_orientation
      : static_facts ? static_facts->sensor_orientation : std::nullopt;
  resolved.camera.intrinsics = external_facts && external_facts->intrinsics
      ? external_facts->intrinsics
      : provider_image && provider_image->intrinsics ? provider_image->intrinsics
      : static_facts ? static_facts->intrinsics : std::nullopt;
  resolved.camera.distortion = external_facts && external_facts->distortion
      ? external_facts->distortion
      : provider_image && provider_image->distortion ? provider_image->distortion
      : static_facts ? static_facts->distortion : std::nullopt;
  resolved.camera.pose = external_facts && external_facts->pose
      ? external_facts->pose
      : provider_image && provider_image->pose ? provider_image->pose
      : static_facts ? static_facts->pose : std::nullopt;
  if (provider_image) {
    resolved.image.focus_state = provider_image->focus_state;
    resolved.image.realized_image_transform = provider_image->realized_image_transform;
  }
  return resolved;
}

void CoreRuntime::finalize_completed_capture_facts_(
    uint64_t capture_id, uint64_t device_instance_id) {
  assert(core_thread_.is_core_thread());
  const std::optional<CaptureAdmissionContext> context =
      capture_assembly_registry_.admission_context_for(capture_id, device_instance_id);
  (void)result_store_.finalize_capture_facts(
      capture_id,
      device_instance_id,
      context,
      [this, capture_id, device_instance_id](uint32_t image_member_index) {
        return resolve_capture_image_facts_(
            capture_id, device_instance_id, image_member_index);
      });
}

void CoreRuntime::begin_capture_stream_preemption_(uint64_t capture_id, uint64_t device_instance_id) {
  assert(core_thread_.is_core_thread());
  if (capture_id == 0 || device_instance_id == 0) {
    return;
  }

  auto& by_capture = capture_stream_preemptions_by_device_[device_instance_id];
  const bool was_empty = by_capture.empty();
  by_capture[capture_id] = CaptureStreamPreemptionRecord{capture_id, device_instance_id};
  if (was_empty) {
    capture_latency_trace_printf(
        "stream_preempted_for_capture capture_id=%llu device_id=%llu active_captures_for_device=%llu",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>(by_capture.size()));
  }
}

void CoreRuntime::begin_capture_stream_preemption_for_bundle_(const RigAdmittedRequestBundle& bundle) {
  assert(core_thread_.is_core_thread());
  if (!bundle.ok || bundle.capture_id == 0) {
    return;
  }
  for (const RigAdmittedParticipantRequest& participant : bundle.participants) {
    begin_capture_stream_preemption_(bundle.capture_id, participant.request.device_instance_id);
  }
}

void CoreRuntime::release_result_safe_capture_stream_preemptions_() {
  assert(core_thread_.is_core_thread());
  for (auto device_it = capture_stream_preemptions_by_device_.begin();
       device_it != capture_stream_preemptions_by_device_.end();) {
    const uint64_t device_instance_id = device_it->first;
    auto& by_capture = device_it->second;
    for (auto capture_it = by_capture.begin(); capture_it != by_capture.end();) {
      const uint64_t capture_id = capture_it->first;
      if (!capture_assembly_registry_.is_result_safe(capture_id, device_instance_id)) {
        ++capture_it;
        continue;
      }
      capture_latency_trace_printf(
          "stream_preemption_released capture_id=%llu device_id=%llu",
          static_cast<unsigned long long>(capture_id),
          static_cast<unsigned long long>(device_instance_id));
      capture_it = by_capture.erase(capture_it);
    }
    if (by_capture.empty()) {
      device_it = capture_stream_preemptions_by_device_.erase(device_it);
    } else {
      ++device_it;
    }
  }
}

bool CoreRuntime::is_stream_preempted_for_capture_(uint64_t stream_id) const {
  assert(core_thread_.is_core_thread());
  if (stream_id == 0) {
    return false;
  }
  const CoreStreamRegistry::StreamRecord* stream = streams_.find(stream_id);
  if (!stream || stream->device_instance_id == 0) {
    return false;
  }
  const auto it = capture_stream_preemptions_by_device_.find(stream->device_instance_id);
  return it != capture_stream_preemptions_by_device_.end() && !it->second.empty();
}

bool CoreRuntime::suppress_repeating_stream_frame_for_capture_(ProviderToCoreCommand&& cmd) {
  assert(core_thread_.is_core_thread());
  const ProviderFactSummary summary = summarize_provider_fact(cmd, capture_cohort_registry_);
  if (summary.fact_class != ProviderFactClass::RepeatingStreamFrame ||
      !is_stream_preempted_for_capture_(summary.stream_id)) {
    return false;
  }

  auto& frame = std::get<CmdProviderFrame>(cmd.payload).frame;
  const uint64_t integrated_ts_ns = capture_latency_trace_now_ns();
  const bool received_counted = streams_.on_frame_received(frame.stream_id, integrated_ts_ns);
  const bool dropped_counted = streams_.on_frame_dropped(frame.stream_id);
  frame.release_now();
  global_resource_aggregate_telemetry().lease_released(make_framebuffer_lease_scoped_resource_telemetry_key(
      frame.stream_id,
      frame.acquisition_session_id));
  frame.release = nullptr;
  frame.release_user = nullptr;

  if (received_counted || dropped_counted) {
    request_publish_from_core_unchecked();
  }
  capture_latency_trace_printf(
      "stream_frame_suppressed_for_capture stream_id=%llu acquisition_session_id=%llu device_id=%llu received_counted=%u dropped_counted=%u delivered=0 publication_requested=%u",
      static_cast<unsigned long long>(summary.stream_id),
      static_cast<unsigned long long>(summary.acquisition_session_id),
      static_cast<unsigned long long>(summary.device_instance_id),
      received_counted ? 1u : 0u,
      dropped_counted ? 1u : 0u,
      (received_counted || dropped_counted) ? 1u : 0u);
  return true;
}

size_t CoreRuntime::suppress_queued_repeating_stream_frames_for_capture_() {
  assert(core_thread_.is_core_thread());
  size_t suppressed = 0;
  for (auto it = provider_facts_.begin(); it != provider_facts_.end();) {
    const ProviderFactSummary summary = summarize_provider_fact(*it, capture_cohort_registry_);
    if (summary.fact_class != ProviderFactClass::RepeatingStreamFrame ||
        !is_stream_preempted_for_capture_(summary.stream_id)) {
      ++it;
      continue;
    }
    ProviderToCoreCommand cmd = std::move(*it);
    it = provider_facts_.erase(it);
    if (suppress_repeating_stream_frame_for_capture_(std::move(cmd))) {
      ++suppressed;
    }
  }
  return suppressed;
}

std::vector<SharedCaptureResultData> CoreRuntime::get_capture_result_set(uint64_t capture_id) const {
  if (const auto cohort = capture_cohort_registry_.find(capture_id)) {
    if (cohort->state == CoreCaptureCohortRegistry::CohortState::FAILED) {
      return {};
    }
    std::vector<SharedCaptureResultData> cohort_results;
    cohort_results.reserve(cohort->expected_participants.size());
    for (const auto& participant : cohort->expected_participants) {
      const uint64_t device_instance_id = participant.device_instance_id;
      // Skip, don't discard the whole cohort: a still-pending or failed
      // sibling must not hide the genuinely-completed results of the rest.
      if (!capture_assembly_registry_.is_assembly_successful(capture_id, device_instance_id)) {
        continue;
      }
      SharedCaptureResultData result = result_store_.get_capture_result(capture_id, device_instance_id);
      if (!result) {
        continue;
      }
      cohort_results.push_back(std::move(result));
    }
    return curate_capture_result_set_accept_all_assembly_successful_(std::move(cohort_results));
  }

  std::vector<SharedCaptureResultData> candidates = result_store_.get_capture_result_set(capture_id);
  std::vector<SharedCaptureResultData> assembly_successful;
  assembly_successful.reserve(candidates.size());
  for (auto& candidate : candidates) {
    if (!candidate) {
      continue;
    }
    if (!capture_assembly_registry_.is_assembly_successful(capture_id, candidate->device_instance_id)) {
      continue;
    }
    assembly_successful.push_back(std::move(candidate));
  }
  return curate_capture_result_set_accept_all_assembly_successful_(std::move(assembly_successful));
}

bool CoreRuntime::start() {
  // Idempotent start: already running is a success.
  const CoreRuntimeState st0 = state_.load(std::memory_order_acquire);
  if (st0 == CoreRuntimeState::LIVE || st0 == CoreRuntimeState::STARTING) {
    return true;
  }
  if (st0 == CoreRuntimeState::TEARING_DOWN) {
    return false;
  }

  // Attempt to transition CREATED/STOPPED -> STARTING.
  CoreRuntimeState expected = st0;
  while (expected == CoreRuntimeState::CREATED || expected == CoreRuntimeState::STOPPED) {
    if (state_.compare_exchange_weak(expected, CoreRuntimeState::STARTING,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire)) {
      break;
    }
  }
  if (state_.load(std::memory_order_acquire) != CoreRuntimeState::STARTING) {
    // Another thread moved us to LIVE/STARTING concurrently.
    const CoreRuntimeState st1 = state_.load(std::memory_order_acquire);
    return (st1 == CoreRuntimeState::LIVE || st1 == CoreRuntimeState::STARTING);
  }

  publish_pending_.store(false, std::memory_order_relaxed);
  publish_requests_coalesced_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_full_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_closed_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_allocfail_.store(0, std::memory_order_relaxed);
  display_demand_release_async_dropped_full_.store(0, std::memory_order_relaxed);
  display_demand_release_async_dropped_closed_.store(0, std::memory_order_relaxed);
  display_demand_release_async_dropped_allocfail_.store(0, std::memory_order_relaxed);

  // Reset per-generation snapshot bookkeeping (schema v1).
  // gen is monotonic across the app/server lifetime and increments only when a new core loop
  // is successfully started.
  const uint64_t pending_gen = gen_counter_;
  current_gen_ = pending_gen;
  version_ = 0;
  topology_version_ = 0;
  last_topology_sig_ = 0;
  has_topology_sig_ = false;
  provider_banner_printed_ = false;

  // Reset core-thread-only pump state.
  rigs_.clear();
  capture_assembly_registry_.clear();
  capture_cohort_registry_.clear();
  capture_stream_preemptions_by_device_.clear();
  provider_facts_.clear();
  provider_capture_facts_queued_ = 0;
  pending_capture_observations_.clear();
  requests_.clear();
  shutdown_requested_from_stop_.store(false, std::memory_order_release);
  shutdown_requested_ = false;
  shutdown_phase_ = ShutdownPhase::NONE;
  shutdown_phase_code_.store(0, std::memory_order_relaxed);
  shutdown_phase_changes_.store(0, std::memory_order_relaxed);
  shutdown_final_publish_requested_ = false;
  shutdown_wait_ticks_ = 0;

  const bool ok = core_thread_.start(this);
  if (ok) {
    ++gen_counter_;
  } else {
    // Failed to start core thread; revert to a sensible stable state.
    state_.store(CoreRuntimeState::STOPPED, std::memory_order_release);
  }
  return ok;
}

std::vector<SharedCaptureResultData> CoreRuntime::curate_capture_result_set_accept_all_assembly_successful_(
    std::vector<SharedCaptureResultData> candidates) const {
  // Placeholder curation seam: current policy accepts all assembly-successful
  // device captures. Future rig/cohort-aware policy can replace this step.
  return candidates;
}

void CoreRuntime::stop() {
  // Idempotent stop.
  const CoreRuntimeState st0 = state_.exchange(CoreRuntimeState::TEARING_DOWN, std::memory_order_acq_rel);
  if (st0 == CoreRuntimeState::STOPPED || st0 == CoreRuntimeState::CREATED) {
    state_.store(st0, std::memory_order_release);
    return;
  }

  // Deterministic shutdown request:
  // - Stop accepting new external requests immediately (state == TEARING_DOWN).
  // - Keep provider fact ingestion best-effort until the core thread closes.
  // - Core thread executes a deterministic shutdown pump and requests stop only at the end.
  //
  // This signal intentionally avoids the bounded best-effort CoreThread task queue:
  // attached-provider shutdown must not be skipped because the ordinary queue is
  // full or closed to new external work during stop.
  if (core_thread_.is_running()) {
    shutdown_requested_from_stop_.store(true, std::memory_order_release);
    core_thread_.request_timer_tick();
    core_thread_.join();
  }

  state_.store(CoreRuntimeState::STOPPED, std::memory_order_release);
}


void CoreRuntime::on_core_start() {
  // Core thread has started; begin accepting new work.
  capture_latency_trace_diagnostics::reset_trace_group_seen();
  epoch_steady_ns_.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count(),
      std::memory_order_release);
  // Do not carry retained result artifacts across generation boundaries.
  result_store_.clear();
  global_resource_aggregate_telemetry().clear();
  acquisition_sessions_.clear();
  provider_camera_fact_state_.clear();
  {
    const std::lock_guard<std::mutex> lock(configured_capture_geolocation_mutex_);
    active_capture_geolocation_ = configured_capture_geolocation_;
  }
  uint64_t configured_camera_description_version = 0;
  uint64_t configured_imaging_spec_version = 0;
  std::vector<uint8_t> configured_imaging_spec_payload{};
  std::optional<ExternalCameraDescriptionState> configured_external_camera_description{};
  {
    const std::lock_guard<std::mutex> lock(configured_imaging_spec_mutex_);
    configured_camera_description_version = configured_camera_description_version_;
    configured_imaging_spec_version = configured_imaging_spec_version_;
    configured_imaging_spec_payload = configured_imaging_spec_payload_;
    configured_external_camera_description = configured_external_camera_description_;
  }
  spec_state_.reset_for_generation(configured_imaging_spec_version);
  active_external_camera_description_ = configured_external_camera_description.value_or(
      ExternalCameraDescriptionState{});
  active_camera_description_version_ = configured_external_camera_description.has_value()
      ? configured_camera_description_version
      : 0;
  if (configured_external_camera_description.has_value()) {
    spec_state_.set_imaging_spec_concurrency(
        configured_imaging_spec_version,
        configured_external_camera_description->concurrency().value_or(camera_concurrency::Truth{}));
  } else if (!configured_imaging_spec_payload.empty()) {
    const SpecPatchView configured_payload{
        configured_imaging_spec_payload.data(),
        configured_imaging_spec_payload.size()};
    const bool retained = spec_state_.retain_imaging_spec_replace(
        configured_imaging_spec_version,
        configured_payload);
    assert(retained);
    (void)retained;
  }
  stream_retained_plan_evaluators_.clear();
  capture_retained_plan_evaluators_.clear();
  stream_retained_plan_decisions_.clear();
  capture_retained_plan_decisions_.clear();
  capture_priming_seeds_.clear();
  capture_parent_priming_states_.clear();
  pending_capture_observations_.clear();
  state_.store(CoreRuntimeState::LIVE, std::memory_order_release);

  // Start "dirty": publish an initial baseline snapshot (version=0, topology_version=0)
  // via the normal coalesced publish path.
  publish_pending_.store(true, std::memory_order_release);
  core_thread_.request_timer_tick();
}

void CoreRuntime::on_core_timer_tick() {
  assert(core_thread_.is_core_thread());

#if defined(CAMBANG_INTERNAL_SMOKE)
  if (smoke_hold_provider_fact_timer_ticks_.load(std::memory_order_acquire)) {
    core_thread_.request_timer_tick();
    return;
  }
#endif

  const auto now = std::chrono::steady_clock::now();
  const uint64_t now_ns = ns_since_epoch_(now);

  // Banner 2: Core-loop provider attachment (effective runtime attachment).
  // Printed once per CoreRuntime session, the first time Core observes a non-null provider.
  if (!provider_banner_printed_) {
    if (ICameraProvider* prov = provider_.load(std::memory_order_acquire)) {
#if defined(CAMBANG_INTERNAL_SMOKE)
      // Smoke can be stress-loop spammy: print only once per *process*.
      static std::atomic<bool> smoke_printed_process{false};
      if (smoke_printed_process.exchange(true)) {
        provider_banner_printed_ = true;
      } else {
        const ProviderBannerInfo bi = describe_provider_for_banner(prov);
        const int n = std::snprintf(core_banner_line_, sizeof(core_banner_line_),
                                    "[CamBANG][Core] provider attached: %s / %s",
                                    bi.provider_mode, bi.provider_name);
        (void)n;
        std::fprintf(stdout, "%s\n", core_banner_line_);
        std::fflush(stdout);
        core_banner_line_pending_.store(true, std::memory_order_release);
        provider_banner_printed_ = true;
      }
#else
      const ProviderBannerInfo bi = describe_provider_for_banner(prov);
      const int n = std::snprintf(core_banner_line_, sizeof(core_banner_line_),
                                  "[CamBANG][Core] provider attached: %s / %s",
                                  bi.provider_mode, bi.provider_name);
      (void)n;
      std::fprintf(stdout, "%s\n", core_banner_line_);
      std::fflush(stdout);
      core_banner_line_pending_.store(true, std::memory_order_release);
#endif
      provider_banner_printed_ = true;
    }
  }


  // 1) Drain provider facts ("what happened") first, but only for a
  // deterministic fairness slice. Conservative/non-lossy provider facts remain
  // FIFO and non-dropping; when backlog remains, the next requested tick
  // continues from the next queued fact after higher-priority command/capture
  // work has had a service opportunity. Stage B.1 honors capture-over-stream
  // priority inside this integration queue: capture facts may pass only a prefix
  // of lower-priority repeating stream frames, and repeating stream frames yield
  // to already-pending command/request work. Stage C coalesces stale repeating
  // stream frames only when a newer frame for the same stream/session is queued
  // before any non-lossy barrier.
  const bool requests_pending_before_provider_drain = !requests_.empty();
  bool command_or_request_waiting_for_stream_frame = false;
  {
    CoreRuntimeDiagnosticPhaseScope diagnostic_phase_scope(
        core_thread_, CoreThread::DiagnosticPhase::RuntimeProviderFactIntegration);
    const size_t provider_fact_drain_bound = requests_pending_before_provider_drain
        ? kMaxProviderFactsBeforeRequestWhenRequestsPending
        : kMaxProviderFactsPerCoreTurn;
    size_t provider_facts_drained = 0;
    while (!provider_facts_.empty() &&
           provider_facts_drained < provider_fact_drain_bound) {
      if (provider_capture_facts_queued_ != 0) {
        (void)promote_capture_fact_over_repeating_stream_prefix(provider_facts_, capture_cohort_registry_);
      }
      const ProviderFactSummary summary =
          summarize_provider_fact(provider_facts_.front(), capture_cohort_registry_);
      const bool command_or_request_pending = requests_pending_before_provider_drain ||
          core_thread_.has_pending_command_tasks();
      if (summary.fact_class == ProviderFactClass::RepeatingStreamFrame &&
          is_stream_preempted_for_capture_(summary.stream_id)) {
        ProviderToCoreCommand cmd = std::move(provider_facts_.front());
        provider_facts_.pop_front();
        (void)suppress_repeating_stream_frame_for_capture_(std::move(cmd));
        ++provider_facts_drained;
        continue;
      }
      if (summary.fact_class == ProviderFactClass::RepeatingStreamFrame) {
        const StreamFrameCoalesceResult coalesce_result =
            coalesce_front_repeating_stream_frame_if_superseded(
                provider_facts_, summary, capture_cohort_registry_, streams_);
        if (coalesce_result.coalesced) {
          request_publish_from_core_unchecked();
          ++provider_facts_drained;
          if (command_or_request_pending) {
            command_or_request_waiting_for_stream_frame = true;
            break;
          }
          continue;
        }
      }
      if (command_or_request_pending && summary.fact_class == ProviderFactClass::RepeatingStreamFrame) {
        command_or_request_waiting_for_stream_frame = true;
        capture_latency_trace_printf(
            "provider_fact_deferred_for_command class=%s stream_id=%llu acquisition_session_id=%llu provider_fact_depth=%llu requests_pending=%u command_lane_pending=%u",
            provider_fact_class_name(summary.fact_class),
            static_cast<unsigned long long>(summary.stream_id),
            static_cast<unsigned long long>(summary.acquisition_session_id),
            static_cast<unsigned long long>(provider_facts_.size()),
            requests_pending_before_provider_drain ? 1u : 0u,
            core_thread_.has_pending_command_tasks() ? 1u : 0u);
        break;
      }

      ProviderToCoreCommand cmd = std::move(provider_facts_.front());
      provider_facts_.pop_front();
      if (provider_fact_is_capture_critical(summary.fact_class) && provider_capture_facts_queued_ != 0) {
        --provider_capture_facts_queued_;
      }
      uint64_t capture_parent_device_instance_id = 0;
      uint64_t preferred_acquisition_session_id = 0;
      if (summary.type == ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_CREATED) {
        const auto& p = std::get<CmdProviderNativeObjectCreated>(cmd.payload);
        if (p.type ==
            static_cast<uint32_t>(NativeObjectType::AcquisitionSession)) {
          capture_parent_device_instance_id = p.owner_device_instance_id;
          preferred_acquisition_session_id = p.native_id;
        }
      } else if (summary.type ==
                 ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_DESTROYED) {
        const auto& p = std::get<CmdProviderNativeObjectDestroyed>(cmd.payload);
        if (const auto* session = acquisition_sessions_.find(p.native_id);
            session != nullptr) {
          capture_parent_device_instance_id = session->device_instance_id;
        }
      } else if (summary.type ==
                     ProviderToCoreCommandType::PROVIDER_CAPTURE_STARTED ||
                 summary.type ==
                     ProviderToCoreCommandType::PROVIDER_CAPTURE_COMPLETED ||
                 summary.type ==
                     ProviderToCoreCommandType::PROVIDER_CAPTURE_FAILED) {
        capture_parent_device_instance_id = summary.device_instance_id;
      }
      const uint64_t dispatch_begin_ns = capture_latency_trace_now_ns();
      dispatcher_.dispatch(std::move(cmd));
      const uint64_t dispatch_us = (capture_latency_trace_now_ns() - dispatch_begin_ns) / 1000ull;
      emit_provider_fact_dispatch_slow_if_needed(summary, dispatch_us, provider_facts_.size());
      if (capture_parent_device_instance_id != 0 &&
          rehome_capture_retained_plan_parent_state_(
              capture_parent_device_instance_id,
              preferred_acquisition_session_id)) {
        request_publish_from_core_unchecked();
      }
      if (provider_fact_is_capture_critical(summary.fact_class)) {
        release_result_safe_capture_stream_preemptions_();
      }
      ++provider_facts_drained;

      if (!requests_pending_before_provider_drain && core_thread_.has_pending_command_tasks()) {
        command_or_request_waiting_for_stream_frame = true;
        break;
      }
    }
  }
  const bool provider_facts_remain_after_fairness_slice = !provider_facts_.empty();

  // If provider facts mutated relevant state, request a coalesced publish.
  if (dispatcher_.consume_relevant_state_changed()) {
    request_publish_from_core_unchecked();
  }

  // If command-lane work arrived while this timer tick was integrating provider
  // facts, return to CoreThread promptly so the command lane can enqueue into
  // requests_. The pending publish/timer state remains retained and this tick is
  // re-requested for deterministic continuation.
  if (command_or_request_waiting_for_stream_frame && requests_.empty() &&
      !shutdown_requested_ && !shutdown_requested_from_stop_.load(std::memory_order_acquire)) {
    core_thread_.request_timer_tick();
    return;
  }

  // 2) Drain queued requests ("what should we do").
  {
    CoreRuntimeDiagnosticPhaseScope diagnostic_phase_scope(
        core_thread_, CoreThread::DiagnosticPhase::RuntimeRequestDrain);
    while (!requests_.empty()) {
      auto task = std::move(requests_.front());
      requests_.pop_front();
      if (task) {
        task();
      }
    }
  }

  bool has_next_pending_capture_observation_delay = false;
  uint64_t next_pending_capture_observation_delay_ns = 0;
  process_pending_capture_observations_(
      now_ns,
      has_next_pending_capture_observation_delay,
      next_pending_capture_observation_delay_ns);

  if (provider_facts_remain_after_fairness_slice) {
    core_thread_.request_timer_tick();
  }

  // 3) Retention / timers.
  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  {
    CoreRuntimeDiagnosticPhaseScope diagnostic_phase_scope(
        core_thread_, CoreThread::DiagnosticPhase::RuntimeRetentionTimerWork);

    uint64_t next_warm_delay_ns = 0;
    bool has_next_warm_delay = false;
    for (const auto& [device_id, rec] : devices_.all()) {
      (void)device_id;
      if (!rec.open) {
        continue;
      }

      const bool active_use = streams_.has_flowing_stream_for_device(rec.device_instance_id);

      if (active_use) {
        (void)devices_.set_warm_was_in_use(rec.device_instance_id, true);
        if (rec.warm_deadline_active || rec.warm_expired_close_requested) {
          (void)devices_.clear_warm_deadline(rec.device_instance_id);
          request_publish_from_core_unchecked();
        }
        continue;
      }

      const bool became_not_in_use = rec.warm_was_in_use;
      if (became_not_in_use) {
        (void)devices_.set_warm_was_in_use(rec.device_instance_id, false);
      }

      if (rec.warm_hold_ms == 0) {
        if (rec.warm_deadline_active || rec.warm_expired_close_requested) {
          (void)devices_.clear_warm_deadline(rec.device_instance_id);
          request_publish_from_core_unchecked();
        }
        continue;
      }

      if (!rec.warm_deadline_active) {
        const uint64_t hold_ns = warm_delay_ns(rec.warm_hold_ms);
        const uint64_t deadline_ns = (hold_ns > (std::numeric_limits<uint64_t>::max() - now_ns))
            ? std::numeric_limits<uint64_t>::max()
            : (now_ns + hold_ns);
        if (devices_.arm_warm_deadline(rec.device_instance_id, deadline_ns)) {
          request_publish_from_core_unchecked();
        }
        continue;
      }

      if (rec.warm_deadline_active && now_ns >= rec.warm_deadline_ns) {
        if (!rec.warm_expired_close_requested && prov) {
          capture_latency_trace_printf(
              "core_close_device_request source=warm_expiry device_id=%llu warm_hold_ms=%u warm_deadline_ns=%llu now_ns=%llu has_flowing_stream=%u",
              static_cast<unsigned long long>(rec.device_instance_id),
              static_cast<unsigned>(rec.warm_hold_ms),
              static_cast<unsigned long long>(rec.warm_deadline_ns),
              static_cast<unsigned long long>(now_ns),
              streams_.has_flowing_stream_for_device(rec.device_instance_id) ? 1u : 0u);
          (void)devices_.mark_warm_expired_close_requested(rec.device_instance_id, true);
          (void)prov->close_device(rec.device_instance_id);
          request_publish_from_core_unchecked();
        }
        continue;
      }

      const uint64_t remaining_ns = rec.warm_deadline_ns - now_ns;
      if (!has_next_warm_delay || remaining_ns < next_warm_delay_ns) {
        has_next_warm_delay = true;
        next_warm_delay_ns = remaining_ns;
      }
    }

    const size_t retired_count =
        native_objects_.retire_destroyed_older_than(now_ns, kDestroyedNativeObjectRetentionWindowNs);
    const size_t retired_capture_orphan_count =
        retire_expired_capture_retained_plan_orphans_(now_ns);
    global_resource_aggregate_telemetry().reconcile_lifecycle(
        now_ns,
        current_gen_,
        &streams_,
        &acquisition_sessions_,
        &devices_,
        &native_objects_);
    const size_t retired_telemetry_count =
        global_resource_aggregate_telemetry().retire_destroyed_older_than(now_ns, kDestroyedNativeObjectRetentionWindowNs);
    if (retired_count > 0 || retired_capture_orphan_count > 0 ||
        retired_telemetry_count > 0) {
      request_publish_from_core_unchecked();
    }

    uint64_t next_deadline_delay_ns = 0;
    bool has_next_deadline_delay = false;
    if (const auto next_retirement_delay_ns =
            native_objects_.next_retirement_delay_ns(now_ns, kDestroyedNativeObjectRetentionWindowNs);
        next_retirement_delay_ns.has_value()) {
      has_next_deadline_delay = true;
      next_deadline_delay_ns = *next_retirement_delay_ns;
    }
    if (const auto next_telemetry_retirement_delay_ns =
            global_resource_aggregate_telemetry().next_retirement_delay_ns(now_ns, kDestroyedNativeObjectRetentionWindowNs);
        next_telemetry_retirement_delay_ns.has_value()) {
      if (!has_next_deadline_delay || *next_telemetry_retirement_delay_ns < next_deadline_delay_ns) {
        has_next_deadline_delay = true;
        next_deadline_delay_ns = *next_telemetry_retirement_delay_ns;
      }
    }
    if (has_next_warm_delay && (!has_next_deadline_delay || next_warm_delay_ns < next_deadline_delay_ns)) {
      has_next_deadline_delay = true;
      next_deadline_delay_ns = next_warm_delay_ns;
    }
    if (has_next_pending_capture_observation_delay &&
        (!has_next_deadline_delay ||
         next_pending_capture_observation_delay_ns < next_deadline_delay_ns)) {
      has_next_deadline_delay = true;
      next_deadline_delay_ns = next_pending_capture_observation_delay_ns;
    }
    next_capture_retained_plan_orphan_retirement_delay_(
        now_ns, has_next_deadline_delay, next_deadline_delay_ns);

    if (has_next_deadline_delay) {
      core_thread_.set_timer_deadline_ns(next_deadline_delay_ns);
    } else {
      core_thread_.clear_timer_deadline();
    }

  }

  // 4) Snapshot publish (coalesced).
  if (publish_pending_.load(std::memory_order_acquire)) {
    CoreRuntimeDiagnosticPhaseScope diagnostic_phase_scope(core_thread_, CoreThread::DiagnosticPhase::RuntimeSnapshotPublication);
    // Clear pending first so a new request can enqueue even if publish work is heavy.
    publish_pending_.store(false, std::memory_order_release);

    SnapshotBuilder::Inputs in;
    in.rigs = &rigs_;
    in.devices = &devices_;
    in.acquisition_sessions = &acquisition_sessions_;
    in.streams = &streams_;
    in.ingress = &ingress_;
    in.native_objects = &native_objects_;
    in.spec_state = &spec_state_;
    in.scoped_resource_telemetry = &global_resource_aggregate_telemetry();

    const uint64_t topo_sig = snapshot_builder_.compute_topology_signature(in);

    // Publish-side topology signature for boundary diffing (Godot-facing).
    // This is updated on every successful snapshot build/publish.
    published_topology_sig_.store(topo_sig, std::memory_order_release);

    // topology_version is zero-indexed within each gen.
    // The first published snapshot establishes the baseline topology_version=0.
    if (!has_topology_sig_) {
      has_topology_sig_ = true;
      last_topology_sig_ = topo_sig;
    } else if (topo_sig != last_topology_sig_) {
      last_topology_sig_ = topo_sig;
      ++topology_version_;
    }

    const uint64_t gen_out = current_gen_;
    const uint64_t ver_out = version_;
    const uint64_t topo_out = topology_version_;
    const uint64_t timestamp_ns = ns_since_epoch_(now);

    CamBANGStateSnapshot snap = snapshot_builder_.build(in, gen_out, ver_out, topo_out, timestamp_ns);
    auto shared = std::make_shared<CamBANGStateSnapshot>(std::move(snap));

    // Advance per-generation publish counter only after snapshot assembly succeeds.
    ++version_;

    if (IStateSnapshotPublisher* pub = snapshot_publisher_.load(std::memory_order_acquire)) {
      pub->publish(std::move(shared));
    }

    // published_seq_ must not become visible before the corresponding snapshot
    // is visible to boundary consumers.
    published_seq_.fetch_add(1, std::memory_order_acq_rel);
  }

  if (shutdown_requested_from_stop_.exchange(false, std::memory_order_acq_rel)) {
    shutdown_requested_ = true;
  }

  // 5) Shutdown choreography (§10).
  if (shutdown_requested_) {
    CoreRuntimeDiagnosticPhaseScope diagnostic_phase_scope(core_thread_, CoreThread::DiagnosticPhase::RuntimeShutdownChoreography);
    auto set_phase = [this](ShutdownPhase p) {
      if (shutdown_phase_ != p) {
        shutdown_phase_ = p;
        shutdown_phase_code_.store(static_cast<uint8_t>(p), std::memory_order_relaxed);
        shutdown_phase_changes_.fetch_add(1, std::memory_order_relaxed);
      }
    };

    if (shutdown_phase_ == ShutdownPhase::NONE) {
      set_phase(ShutdownPhase::STOP_STREAMS);
      shutdown_wait_ticks_ = 0;
    }

    constexpr uint32_t kMaxShutdownWaitTicks = 200; // deterministic bound; avoids teardown hangs.

    auto any_stream_started = [this]() -> bool {
      for (const auto& kv : streams_.all()) {
        if (kv.second.started) {
          return true;
        }
      }
      return false;
    };

    auto any_streams_exist = [this]() -> bool {
      return !streams_.all().empty();
    };

    auto any_device_open = [this]() -> bool {
      for (const auto& kv : devices_.all()) {
        if (kv.second.open) {
          return true;
        }
      }
      return false;
    };

    switch (shutdown_phase_) {
      case ShutdownPhase::STOP_STREAMS: {
        // Step 3: stop streams (deterministic order).  Public try_destroy_stream()
        // remains strict, but shutdown is host-owned cleanup: when the provider
        // stop call returns OK the provider has synchronously ceased production, so
        // reflect that truth locally as well as accepting the later provider fact.
        if (prov) {
          std::vector<uint64_t> started_stream_ids;
          started_stream_ids.reserve(streams_.all().size());
          for (const auto& kv : streams_.all()) {
            const auto& rec = kv.second;
            if (rec.started) {
              started_stream_ids.push_back(rec.stream_id);
            }
          }
          for (const uint64_t stream_id : started_stream_ids) {
            (void)streams_.mark_stop_requested_by_core(stream_id);
            const ProviderResult sr = prov->stop_stream(stream_id);
            if (sr.ok()) {
              (void)streams_.on_core_stream_stopped(stream_id, /*error_code=*/0);
            }
          }
        }
        set_phase(ShutdownPhase::AWAIT_STREAMS_STOPPED);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::AWAIT_STREAMS_STOPPED: {
        // Best-effort drain: wait for provider facts to reflect stopped streams.
        if (any_stream_started() && shutdown_wait_ticks_++ < kMaxShutdownWaitTicks) {
          core_thread_.request_timer_tick();
          return;
        }
        set_phase(ShutdownPhase::DESTROY_STREAMS);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::DESTROY_STREAMS: {
        // Step 4 (part): tear down stream instances (destroy) before closing devices.
        // Provider destroy_stream() is still strict for ordinary callers; this phase
        // only runs after shutdown stop convergence.  On OK, provider storage has
        // been structurally destroyed, so make registry absence immediately
        // observable instead of depending on a provider-strand round trip.
        if (prov) {
          std::vector<uint64_t> created_stream_ids;
          created_stream_ids.reserve(streams_.all().size());
          for (const auto& kv : streams_.all()) {
            const auto& rec = kv.second;
            if (rec.created) {
              created_stream_ids.push_back(rec.stream_id);
            }
          }
          for (const uint64_t stream_id : created_stream_ids) {
            const ProviderResult dr = prov->destroy_stream(stream_id);
            if (dr.ok()) {
              (void)streams_.on_stream_destroyed(stream_id);
              result_store_.remove_stream_result(stream_id);
              stream_retained_plan_evaluators_.erase(stream_id);
              stream_retained_plan_decisions_.erase(stream_id);
            }
          }
        }
        set_phase(ShutdownPhase::AWAIT_STREAMS_DESTROYED);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::AWAIT_STREAMS_DESTROYED: {
        // Best-effort: wait until provider confirms destruction.
        if (any_streams_exist() && shutdown_wait_ticks_++ < kMaxShutdownWaitTicks) {
          core_thread_.request_timer_tick();
          return;
        }
        set_phase(ShutdownPhase::CLOSE_DEVICES);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::CLOSE_DEVICES: {
        if (prov) {
          for (const auto& kv : devices_.all()) {
            const auto& rec = kv.second;
            if (rec.open) {
              capture_latency_trace_printf(
                  "core_close_device_request source=shutdown_close_devices device_id=%llu warm_hold_ms=%u has_flowing_stream=%u",
                  static_cast<unsigned long long>(rec.device_instance_id),
                  static_cast<unsigned>(rec.warm_hold_ms),
                  streams_.has_flowing_stream_for_device(rec.device_instance_id) ? 1u : 0u);
              (void)prov->close_device(rec.device_instance_id);
            }
          }
        }
        set_phase(ShutdownPhase::AWAIT_DEVICES_CLOSED);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::AWAIT_DEVICES_CLOSED: {
        if (any_device_open() && shutdown_wait_ticks_++ < kMaxShutdownWaitTicks) {
          core_thread_.request_timer_tick();
          return;
        }
        set_phase(ShutdownPhase::PROVIDER_SHUTDOWN);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::PROVIDER_SHUTDOWN: {
        // Step 5: request provider shutdown (idempotent).
        if (prov) {
          (void)prov->shutdown();
        }
        set_phase(ShutdownPhase::FINAL_RETENTION_SWEEP);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::FINAL_RETENTION_SWEEP: {
        (void)native_objects_.retire_destroyed_older_than(now_ns, kDestroyedNativeObjectRetentionWindowNs);
        global_resource_aggregate_telemetry().reconcile_lifecycle(
            now_ns,
            current_gen_,
            &streams_,
            &acquisition_sessions_,
            &devices_,
            &native_objects_);
        (void)global_resource_aggregate_telemetry().retire_destroyed_older_than(now_ns, kDestroyedNativeObjectRetentionWindowNs);
        set_phase(ShutdownPhase::FINAL_PUBLISH);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::FINAL_PUBLISH: {
        // Step 7: final snapshot publication.
        if (!shutdown_final_publish_requested_) {
          request_publish_from_core_unchecked();
          shutdown_final_publish_requested_ = true;
          core_thread_.request_timer_tick();
          return;
        }

        set_phase(ShutdownPhase::CLEAR_DESTROYED_RETAINED_NATIVE_OBJECTS);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::CLEAR_DESTROYED_RETAINED_NATIVE_OBJECTS: {
        // Do not carry retained DESTROYED records across stop/start boundaries.
        // They remain truthfully retained while the generation is live and through
        // final prior-generation publication, then are quarantined before exit.
        (void)native_objects_.clear_destroyed();
        global_resource_aggregate_telemetry().clear();
        set_phase(ShutdownPhase::EXIT);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::EXIT: {
        // Step 8: exit when fully drained and publish completed.
        const bool publish_still_pending = publish_pending_.load(std::memory_order_acquire);
        if ((provider_facts_.empty() && requests_.empty() && !publish_still_pending) ||
            (shutdown_wait_ticks_++ >= kMaxShutdownWaitTicks)) {
          core_thread_.request_stop_from_core();
        } else {
          core_thread_.request_timer_tick();
        }
        return;
      }

      default:
        break;
    }
  }
}

void CoreRuntime::on_core_stop() {
  capture_latency_trace_diagnostics::print_trace_group_seen_summary();
  // Runtime is no longer live; clear retained results so stop/start boundaries
  // cannot expose stale prior-generation result truth.
  result_store_.clear();
  provider_camera_fact_state_.clear();
  global_resource_aggregate_telemetry().clear();
  stream_retained_plan_evaluators_.clear();
  capture_retained_plan_evaluators_.clear();
  stream_retained_plan_decisions_.clear();
  capture_retained_plan_decisions_.clear();
  capture_priming_seeds_.clear();
  capture_parent_priming_states_.clear();
  pending_capture_observations_.clear();
  // Core thread is exiting. Ensure external gating sees STOPPED promptly.
  state_.store(CoreRuntimeState::STOPPED, std::memory_order_release);
}

void CoreRuntime::post(CoreThread::Task task) {
  // Best-effort compatibility shim for disposable work only; retained runtime
  // truth must call an admission-returning path and handle the result.
  (void)try_post(std::move(task));
}

CoreThread::PostResult CoreRuntime::retain_device_identity(uint64_t device_instance_id, const std::string& hardware_id) {
  if (device_instance_id == 0 || hardware_id.empty()) {
    return CoreThread::PostResult::Closed;
  }

  CaptureTemplate capture_tmpl{};
  bool has_capture_template = false;
  if (ICameraProvider* p = provider_.load(std::memory_order_acquire)) {
    capture_tmpl = p->capture_template();
    has_capture_template = true;
  }

  return try_post([this, device_instance_id, hardware_id, capture_tmpl, has_capture_template]() {
    if (!devices_.note_device_identity(device_instance_id, hardware_id)) {
      return;
    }
    devices_.set_camera_spec_version(
        device_instance_id,
        spec_state_.camera_spec_version(hardware_id));
    if (has_capture_template) {
      (void)seed_retained_device_still_profile_from_template(
          devices_, device_instance_id, capture_tmpl);
    }
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::retain_camera_spec_version(const std::string& hardware_id, uint64_t camera_spec_version) {
  if (hardware_id.empty()) {
    return CoreThread::PostResult::Closed;
  }

  return try_post([this, hardware_id, camera_spec_version]() {
    spec_state_.set_camera_spec_version(hardware_id, camera_spec_version);
    for (const auto& [device_instance_id, rec] : devices_.all()) {
      if (rec.hardware_id == hardware_id) {
        (void)devices_.set_camera_spec_version(device_instance_id, camera_spec_version);
      }
    }
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::retain_device_capture_profile(uint64_t device_instance_id,
                                                                  uint32_t width,
                                                                  uint32_t height,
                                                                  uint32_t format,
                                                                  uint64_t capture_profile_version) {
  if (device_instance_id == 0) {
    return CoreThread::PostResult::Closed;
  }

  return try_post([this, device_instance_id, width, height, format, capture_profile_version]() {
    (void)devices_.retain_capture_profile(
        device_instance_id, width, height, format, capture_profile_version);
    (void)refresh_capture_retained_plan_state_(
        device_instance_id,
        /*requested_bump_access_posture_epoch=*/false);
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::retain_rig_capture_profile(uint64_t rig_id,
                                                               uint32_t width,
                                                               uint32_t height,
                                                               uint32_t format,
                                                               uint64_t capture_profile_version) {
  if (rig_id == 0) {
    return CoreThread::PostResult::Closed;
  }

  return try_post([this, rig_id, width, height, format, capture_profile_version]() {
    (void)rigs_.retain_capture_profile(rig_id, width, height, format, capture_profile_version);
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::retain_imaging_spec_version(uint64_t imaging_spec_version) {
  return try_post([this, imaging_spec_version]() {
    spec_state_.set_imaging_spec_version(imaging_spec_version);
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::retain_imaging_spec_replace(
    uint64_t imaging_spec_version,
    SpecPatchView effective_spec) {
  if (!is_valid_spec_patch_view(effective_spec)) {
    return CoreThread::PostResult::Closed;
  }
  std::vector<uint8_t> owned_payload = copy_spec_patch_payload(effective_spec);

  return try_post([this, imaging_spec_version, owned_payload = std::move(owned_payload)]() {
    const SpecPatchView retained_payload{
        owned_payload.empty() ? nullptr : owned_payload.data(),
        owned_payload.size()};
    if (!spec_state_.retain_imaging_spec_replace(imaging_spec_version, retained_payload)) {
      return;
    }
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::retain_imaging_spec_patch(
    uint64_t imaging_spec_version,
    SpecPatchView effective_spec) {
  if (!is_valid_spec_patch_view(effective_spec)) {
    return CoreThread::PostResult::Closed;
  }
  std::vector<uint8_t> owned_payload = copy_spec_patch_payload(effective_spec);

  return try_post([this, imaging_spec_version, owned_payload = std::move(owned_payload)]() {
    const SpecPatchView retained_payload{
        owned_payload.empty() ? nullptr : owned_payload.data(),
        owned_payload.size()};
    if (!spec_state_.retain_imaging_spec_patch(imaging_spec_version, retained_payload)) {
      return;
    }
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::try_post(CoreThread::Task task) {
  const CoreRuntimeState st = state_.load(std::memory_order_acquire);
  if (st != CoreRuntimeState::LIVE) {
    return core_thread_.reject_closed();
  }

  // Marshal Core-owned commands into the command lane. The CoreRuntime pump still
  // enforces facts-before-requests ordering inside on_core_timer_tick(), but the
  // command lane prevents public/request work from sitting behind ordinary
  // provider-frame ingress tasks before it can enter requests_.
  return core_thread_.try_post_command([this, t = std::move(task)]() mutable {
    assert(core_thread_.is_core_thread());
    enqueue_request(std::move(t));
  });
}

TryCreateStreamStatus CoreRuntime::try_create_stream(
    uint64_t stream_id,
    uint64_t device_instance_id,
    StreamIntent intent,
    const CaptureProfile* request_profile,
    const PictureConfig* request_picture,
    uint64_t profile_version) noexcept {
  if (stream_id == 0 || device_instance_id == 0) {
    return TryCreateStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryCreateStreamStatus::Busy;
  }

  // Compute effective config (core owns defaulting).
  const StreamTemplate tmpl = prov->stream_template();
  const bool has_request_profile = (request_profile != nullptr);
  const bool has_request_picture = (request_picture != nullptr);
  const CaptureProfile request_profile_copy = has_request_profile ? *request_profile : CaptureProfile{};
  const PictureConfig request_picture_copy = has_request_picture ? *request_picture : PictureConfig{};

  const CoreThread::PostResult pr = try_post([this,
                                              stream_id,
                                              device_instance_id,
                                              intent,
                                              profile_version,
                                              tmpl,
                                              has_request_profile,
                                              request_profile_copy,
                                              has_request_picture,
                                              request_picture_copy]() {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) return;

    const uint64_t effective_profile_version =
        (profile_version != 0)
            ? profile_version
            : create_stream_profile_version_seq_.fetch_add(1, std::memory_order_relaxed);

    StreamRequest effective{};
    effective.stream_id = stream_id;
    effective.device_instance_id = device_instance_id;
    effective.intent = intent;
    effective.profile_version = effective_profile_version;
    effective.profile = has_request_profile ? request_profile_copy : tmpl.profile;
    effective.picture = has_request_picture ? request_picture_copy : tmpl.picture;
    ProducerBackingCapabilities runtime_caps{};
    ProducerBackingCapabilities parent_context_caps{};
    if (!resolve_stream_backing_capabilities_(
            effective.device_instance_id,
            effective.stream_id,
            effective.intent,
            effective.profile,
            effective.picture,
            runtime_caps,
            parent_context_caps)) {
      return;
    }
    const RetainedPlanResetDecision retained_plan_decision =
        build_retained_plan_reset_decision(
            BackingPlanEvaluationPrimaryFunction::StreamDisplayView,
            parent_context_caps);
    effective.requested_retained_plan = retained_plan_decision.requested;

    // Declare before calling into the provider so any synchronous callbacks
    // can resolve the record deterministically.
    (void)streams_.declare_stream_effective(
        effective, retained_plan_decision.steady);
    (void)streams_.set_backing_capabilities(
        effective.stream_id, runtime_caps, parent_context_caps);
    if (retained_plan_decision.evaluation_active) {
      RetainedPlanEvaluatorState state;
      state.device_instance_id = effective.device_instance_id;
      state.primary_function =
          BackingPlanEvaluationPrimaryFunction::StreamDisplayView;
      state.completion_reason = BackingPlanEvaluationCompletionReason::None;
      state.active = true;
      state.candidate_count = retained_plan_decision.candidate_count;
      for (uint8_t i = 0; i < retained_plan_decision.candidate_count; ++i) {
        state.candidate_sequence[i] = make_retained_plan(
            retained_plan_decision.candidate_sequence[i]);
      }
      stream_retained_plan_evaluators_[stream_id] = state;
      stream_retained_plan_decisions_.erase(stream_id);
    } else {
      stream_retained_plan_evaluators_.erase(stream_id);
      CoreRetainedProductionPlan candidate_sequence[3]{};
      for (uint8_t i = 0; i < retained_plan_decision.candidate_count; ++i) {
        candidate_sequence[i] =
            make_retained_plan(retained_plan_decision.candidate_sequence[i]);
      }
      RetainedPlanDecisionProvenance provenance =
          build_non_evaluated_decision_provenance_(
              BackingPlanEvaluationPrimaryFunction::StreamDisplayView,
              device_instance_id,
              0,
              retained_plan_decision.requested,
              retained_plan_decision.steady,
              retained_plan_decision.candidate_count,
              candidate_sequence);
      stream_retained_plan_decisions_[stream_id] = provenance;
    }

    const ProviderResult r = p->create_stream(effective);
    if (!r.ok()) {
      // Best-effort rollback; create_stream failure must not leave a ghost record.
      (void)streams_.forget_stream(effective.stream_id);
      stream_retained_plan_evaluators_.erase(stream_id);
      stream_retained_plan_decisions_.erase(stream_id);
      request_publish_from_core_unchecked();
      return;
    }
    if (streams_.on_stream_created(effective.stream_id)) {
      request_publish_from_core_unchecked();
    }
  });

  return (pr == CoreThread::PostResult::Enqueued) ? TryCreateStreamStatus::OK
                                                  : TryCreateStreamStatus::Busy;
}

TryStartStreamStatus CoreRuntime::try_start_stream(uint64_t stream_id) noexcept {
  if (stream_id == 0) {
    return TryStartStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryStartStreamStatus::Busy;
  }

  if (core_thread_.is_core_thread()) {
    // This synchronous wrapper posts to the core thread and blocks on the
    // result; called from the core thread itself it would self-deadlock (the
    // posted task could never be popped by this same, now-blocked, thread).
    // No current caller does this; fail fast rather than hang if one ever does.
    return TryStartStreamStatus::Busy;
  }

  auto result_promise = std::make_shared<std::promise<TryStartStreamStatus>>();
  std::future<TryStartStreamStatus> f = result_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, stream_id, result_promise]() mutable {
    ICameraProvider* prov_local = provider_.load(std::memory_order_acquire);
    if (!prov_local) {
      result_promise->set_value(TryStartStreamStatus::Busy);
      return;
    }

    const CoreStreamRegistry::StreamRecord* rec = streams_.find(stream_id);
    if (!rec) {
      result_promise->set_value(TryStartStreamStatus::InvalidArgument);
      return;
    }
    if (rec->started) {
      result_promise->set_value(TryStartStreamStatus::OK);
      return;
    }
    const uint64_t owner_device_instance_id = rec->device_instance_id;
    for (const auto& kv : streams_.all()) {
      const auto& other = kv.second;
      if (other.stream_id == stream_id) {
        continue;
      }
      if (other.device_instance_id == owner_device_instance_id && other.created && other.started) {
        result_promise->set_value(TryStartStreamStatus::Busy);
        return;
      }
    }

    const ProviderResult sr = prov_local->start_stream(stream_id, rec->profile, rec->picture);
    if (!sr.ok()) {
      timeline_teardown_trace_emit("fail StartStream stream_id=%llu reason=provider_rc_%u",
                                   static_cast<unsigned long long>(stream_id),
                                   static_cast<unsigned>(sr.code));
      (void)streams_.on_stream_error(stream_id, static_cast<uint32_t>(sr.code));
      result_promise->set_value(TryStartStreamStatus::ProviderRejected);
      return;
    }
    const bool state_changed = streams_.on_core_stream_started(stream_id);
    (void)refresh_capture_retained_plan_state_(
        owner_device_instance_id,
        /*requested_bump_access_posture_epoch=*/false);
    if (state_changed) {
      request_publish_from_core_unchecked();
    }
    result_promise->set_value(TryStartStreamStatus::OK);
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return TryStartStreamStatus::Busy;
  }
  // Bounded wait: public synchronous wrappers must not block indefinitely
  // (cpp_code_quality_policy.md); mirrors the 2s bound already used by the
  // rig orchestration wrappers in this file.
  if (f.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return TryStartStreamStatus::Busy;
  }
  return f.get();
}

TryStopStreamStatus CoreRuntime::try_stop_stream(uint64_t stream_id) noexcept {
  if (stream_id == 0) {
    return TryStopStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryStopStreamStatus::Busy;
  }

  if (core_thread_.is_core_thread()) {
    // See try_start_stream(): this wrapper would self-deadlock if called from
    // the core thread itself. Fail fast rather than hang.
    return TryStopStreamStatus::Busy;
  }

  auto result_promise = std::make_shared<std::promise<TryStopStreamStatus>>();
  std::future<TryStopStreamStatus> f = result_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, stream_id, result_promise]() mutable {
    ICameraProvider* prov_local = provider_.load(std::memory_order_acquire);
    if (!prov_local) {
      result_promise->set_value(TryStopStreamStatus::Busy);
      return;
    }
    const CoreStreamRegistry::StreamRecord* rec = streams_.find(stream_id);
    if (!rec) {
      result_promise->set_value(TryStopStreamStatus::InvalidArgument);
      return;
    }
    if (!rec->started) {
      result_promise->set_value(TryStopStreamStatus::OK);
      return;
    }
    (void)streams_.mark_stop_requested_by_core(stream_id);
    const ProviderResult sr = prov_local->stop_stream(stream_id);
    if (!sr.ok()) {
      timeline_teardown_trace_emit("fail StopStream stream_id=%llu reason=provider_rc_%u",
                                   static_cast<unsigned long long>(stream_id),
                                   static_cast<unsigned>(sr.code));
      result_promise->set_value(TryStopStreamStatus::ProviderRejected);
      return;
    }
    const bool state_changed =
        streams_.on_core_stream_stopped(stream_id, /*error_code=*/0);
    (void)refresh_capture_retained_plan_state_(
        rec->device_instance_id,
        /*requested_bump_access_posture_epoch=*/false);
    if (state_changed) {
      request_publish_from_core_unchecked();
    }
    result_promise->set_value(TryStopStreamStatus::OK);
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return TryStopStreamStatus::Busy;
  }
  if (f.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return TryStopStreamStatus::Busy;
  }
  return f.get();
}

TryDestroyStreamStatus CoreRuntime::try_destroy_stream(uint64_t stream_id) noexcept {
  if (stream_id == 0) {
    return TryDestroyStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryDestroyStreamStatus::Busy;
  }

  if (core_thread_.is_core_thread()) {
    // See try_start_stream(): this wrapper would self-deadlock if called from
    // the core thread itself. Fail fast rather than hang.
    return TryDestroyStreamStatus::Busy;
  }

  auto result_promise = std::make_shared<std::promise<TryDestroyStreamStatus>>();
  std::future<TryDestroyStreamStatus> f = result_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, stream_id, result_promise]() mutable {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) {
      result_promise->set_value(TryDestroyStreamStatus::Busy);
      return;
    }

    const CoreStreamRegistry::StreamRecord* rec = streams_.find(stream_id);
    if (!rec) {
      result_promise->set_value(TryDestroyStreamStatus::InvalidArgument);
      return;
    }
    if (rec->started) {
      timeline_teardown_trace_emit("fail DestroyStream stream_id=%llu reason=stream_started",
                                   static_cast<unsigned long long>(stream_id));
      result_promise->set_value(TryDestroyStreamStatus::Started);
      return;
    }

    const uint64_t owner_device_instance_id = rec->device_instance_id;
    const ProviderResult dr = p->destroy_stream(stream_id);
    if (!dr.ok()) {
      timeline_teardown_trace_emit("fail DestroyStream stream_id=%llu reason=provider_rc_%u",
                                   static_cast<unsigned long long>(stream_id),
                                   static_cast<unsigned>(dr.code));
      result_promise->set_value(TryDestroyStreamStatus::ProviderRejected);
      return;
    }
    const bool state_changed = streams_.on_stream_destroyed(stream_id);
    if (state_changed) {
      result_store_.remove_stream_result(stream_id);
    }
    stream_retained_plan_evaluators_.erase(stream_id);
    stream_retained_plan_decisions_.erase(stream_id);
    (void)refresh_capture_retained_plan_state_(
        owner_device_instance_id,
        /*requested_bump_access_posture_epoch=*/false);
    if (state_changed) {
      request_publish_from_core_unchecked();
    }
    result_promise->set_value(TryDestroyStreamStatus::OK);
  });

  if (pr != CoreThread::PostResult::Enqueued) {
    return TryDestroyStreamStatus::Busy;
  }
  if (f.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return TryDestroyStreamStatus::Busy;
  }
  return f.get();
}

TryOpenDeviceStatus CoreRuntime::try_open_device(
    const std::string& hardware_id,
    uint64_t device_instance_id,
    uint64_t root_id) noexcept {
  if (hardware_id.empty() || device_instance_id == 0 || root_id == 0) {
    return TryOpenDeviceStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryOpenDeviceStatus::Busy;
  }

  if (core_thread_.is_core_thread()) {
    // See try_start_stream(): this wrapper would self-deadlock if called from
    // the core thread itself. Fail fast rather than hang.
    return TryOpenDeviceStatus::Busy;
  }

  const CaptureTemplate capture_tmpl = prov->capture_template();
  auto result_promise = std::make_shared<std::promise<TryOpenDeviceStatus>>();
  std::future<TryOpenDeviceStatus> f = result_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, hardware_id, device_instance_id, root_id, capture_tmpl, result_promise]() mutable {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) {
      result_promise->set_value(TryOpenDeviceStatus::Busy);
      return;
    }

    const ProviderResult open_result = p->open_device(hardware_id, device_instance_id, root_id);
    if (!open_result.ok()) {
      timeline_teardown_trace_emit("fail OpenDevice device_instance_id=%llu reason=provider_rc_%u",
                                   static_cast<unsigned long long>(device_instance_id),
                                   static_cast<unsigned>(open_result.code));
      result_promise->set_value(TryOpenDeviceStatus::ProviderRejected);
      return;
    }

    // Retain core-owned device identity/profile truth only after provider open
    // submission was accepted. A provider-refused open must not publish a
    // speculative CREATED device record.
    (void)devices_.note_device_identity(device_instance_id, hardware_id);
    (void)seed_retained_device_still_profile_from_template(devices_, device_instance_id, capture_tmpl);
    (void)devices_.set_capture_picture(device_instance_id, capture_tmpl.picture);
    const bool state_changed = devices_.on_device_opened(device_instance_id);
    (void)refresh_capture_retained_plan_state_(
        device_instance_id,
        /*requested_bump_access_posture_epoch=*/false);
    if (state_changed) {
      request_publish_from_core_unchecked();
    }
    result_promise->set_value(TryOpenDeviceStatus::OK);
  });

  if (pr != CoreThread::PostResult::Enqueued) {
    return TryOpenDeviceStatus::Busy;
  }
  if (f.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return TryOpenDeviceStatus::Busy;
  }
  return f.get();
}

TryCloseDeviceStatus CoreRuntime::try_close_device(uint64_t device_instance_id) noexcept {
  if (device_instance_id == 0) {
    return TryCloseDeviceStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryCloseDeviceStatus::Busy;
  }

  if (core_thread_.is_core_thread()) {
    // See try_start_stream(): this wrapper would self-deadlock if called from
    // the core thread itself. Fail fast rather than hang.
    return TryCloseDeviceStatus::Busy;
  }

  auto result_promise = std::make_shared<std::promise<TryCloseDeviceStatus>>();
  std::future<TryCloseDeviceStatus> f = result_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, device_instance_id, result_promise]() mutable {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) {
      result_promise->set_value(TryCloseDeviceStatus::Busy);
      return;
    }
    const CoreDeviceRegistry::DeviceRecord* rec = devices_.find(device_instance_id);
    capture_latency_trace_printf(
        "core_close_device_request source=try_close_device device_id=%llu device_open=%u warm_hold_ms=%u warm_deadline_active=%u warm_deadline_ns=%llu has_flowing_stream=%u",
        static_cast<unsigned long long>(device_instance_id),
        (rec && rec->open) ? 1u : 0u,
        rec ? static_cast<unsigned>(rec->warm_hold_ms) : 0u,
        (rec && rec->warm_deadline_active) ? 1u : 0u,
        rec ? static_cast<unsigned long long>(rec->warm_deadline_ns) : 0ull,
        streams_.has_flowing_stream_for_device(device_instance_id) ? 1u : 0u);
    const ProviderResult cr = p->close_device(device_instance_id);
    if (!cr.ok()) {
      timeline_teardown_trace_emit("fail CloseDevice device_instance_id=%llu reason=provider_rc_%u",
                                   static_cast<unsigned long long>(device_instance_id),
                                   static_cast<unsigned>(cr.code));
      result_promise->set_value(TryCloseDeviceStatus::ProviderRejected);
      return;
    }
    const uint64_t now_ns = ns_since_epoch_();
    bool retain_capture_orphans = false;
    for (const auto& [key, state] : capture_retained_plan_evaluators_) {
      if (state.device_instance_id == device_instance_id ||
          (key.kind ==
               CaptureRetainedPlanParentKey::Kind::CapturePriming &&
           key.id == device_instance_id)) {
        retain_capture_orphans = true;
        break;
      }
    }
    if (!retain_capture_orphans) {
      for (const PendingCaptureObservation& pending :
           pending_capture_observations_) {
        if (pending.device_instance_id == device_instance_id) {
          retain_capture_orphans = true;
          break;
        }
      }
    }
    const bool state_changed = devices_.on_device_closed(device_instance_id);
    capture_parent_priming_states_.erase(device_instance_id);
    if (retain_capture_orphans) {
      mark_capture_retained_plan_state_orphaned_for_device_(
          device_instance_id,
          now_ns + kCaptureRetainedPlanOrphanRetentionWindowNs);
    } else {
      for (auto it = capture_retained_plan_evaluators_.begin();
           it != capture_retained_plan_evaluators_.end();) {
        if (it->second.device_instance_id == device_instance_id) {
          it = capture_retained_plan_evaluators_.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = capture_retained_plan_decisions_.begin();
           it != capture_retained_plan_decisions_.end();) {
        const bool same_device =
            it->second.device_instance_id == device_instance_id ||
            (it->first.kind ==
                 CaptureRetainedPlanParentKey::Kind::CapturePriming &&
             it->first.id == device_instance_id);
        if (same_device) {
          it = capture_retained_plan_decisions_.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (state_changed) {
      request_publish_from_core_unchecked();
    }
    result_promise->set_value(TryCloseDeviceStatus::OK);
  });

  if (pr != CoreThread::PostResult::Enqueued) {
    return TryCloseDeviceStatus::Busy;
  }
  if (f.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return TryCloseDeviceStatus::Busy;
  }
  return f.get();
}

TrySetStreamPictureStatus CoreRuntime::try_set_stream_picture_config(
    uint64_t stream_id,
    const PictureConfig& picture) noexcept {
  if (stream_id == 0) {
    return TrySetStreamPictureStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TrySetStreamPictureStatus::Busy;
  }
  if (!prov->supports_stream_picture_updates()) {
    return TrySetStreamPictureStatus::NotSupported;
  }

  const CoreThread::PostResult pr = try_post([this, stream_id, picture]() {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) return;
    const ProviderResult sr = p->set_stream_picture_config(stream_id, picture);
    if (!sr.ok()) {
      return;
    }
    if (streams_.set_picture(stream_id, picture)) {
      (void)refresh_stream_retained_plan_state_(
          stream_id,
          /*apply_to_provider=*/true,
          /*requested_bump_access_posture_epoch=*/false);
      // Picture config is snapshot-visible stream state. Route updates through
      // the normal core coalesced publication path.
      request_publish_from_core_unchecked();
    }
  });

  return (pr == CoreThread::PostResult::Enqueued) ? TrySetStreamPictureStatus::OK
                                                  : TrySetStreamPictureStatus::Busy;
}

TrySetCapturePictureStatus CoreRuntime::try_set_capture_picture_config(
    uint64_t device_instance_id,
    const PictureConfig& picture) noexcept {
  if (device_instance_id == 0) {
    return TrySetCapturePictureStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TrySetCapturePictureStatus::Busy;
  }
  if (!prov->supports_capture_picture_updates()) {
    return TrySetCapturePictureStatus::NotSupported;
  }

  const CoreThread::PostResult pr = try_post([this, device_instance_id, picture]() {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) return;
    const ProviderResult sr = p->set_capture_picture_config(device_instance_id, picture);
    if (!sr.ok()) {
      return;
    }
    if (devices_.set_capture_picture(device_instance_id, picture)) {
      (void)refresh_capture_retained_plan_state_(
          device_instance_id,
          /*requested_bump_access_posture_epoch=*/false);
      request_publish_from_core_unchecked();
    }
  });

  return (pr == CoreThread::PostResult::Enqueued) ? TrySetCapturePictureStatus::OK
                                                  : TrySetCapturePictureStatus::Busy;
}

TrySetStillCaptureProfileStatus CoreRuntime::try_set_device_still_capture_profile(
    uint64_t device_instance_id,
    const CaptureProfile& profile,
    const CaptureStillImageBundle& still_image_bundle) noexcept {
  if (device_instance_id == 0 || profile.width == 0 || profile.height == 0 || profile.format_fourcc == 0) {
    return TrySetStillCaptureProfileStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TrySetStillCaptureProfileStatus::Busy;
  }
  const bool supports_multi_image = prov->supports_multi_image_still_sequence();
  if (!is_valid_capture_still_image_bundle(still_image_bundle, supports_multi_image)) {
    return supports_multi_image
        ? TrySetStillCaptureProfileStatus::InvalidArgument
        : TrySetStillCaptureProfileStatus::NotSupported;
  }

  const CoreThread::PostResult pr = try_post([this, device_instance_id, profile, still_image_bundle]() {
    uint64_t next_version = 1;
    if (const auto* rec = devices_.find(device_instance_id)) {
      bool same_sequence = (rec->capture_still_image_bundle.members.size() == still_image_bundle.members.size());
      if (same_sequence) {
        for (size_t i = 0; i < rec->capture_still_image_bundle.members.size(); ++i) {
          const auto& a = rec->capture_still_image_bundle.members[i];
          const auto& b = still_image_bundle.members[i];
          if (a.image_member_index != b.image_member_index ||
              a.role != b.role ||
              a.intended_exposure_compensation_milli_ev != b.intended_exposure_compensation_milli_ev) {
            same_sequence = false;
            break;
          }
        }
      }
      const bool unchanged =
          rec->capture_width == profile.width &&
          rec->capture_height == profile.height &&
          rec->capture_format == profile.format_fourcc &&
          same_sequence;
      if (unchanged) {
        return;
      }
      next_version = rec->capture_profile_version + 1;
      if (next_version == 0) next_version = 1;
    }
    (void)devices_.retain_capture_profile(
        device_instance_id,
        profile.width,
        profile.height,
        profile.format_fourcc,
        next_version);
    (void)devices_.set_capture_still_image_bundle(device_instance_id, still_image_bundle, next_version);
    (void)refresh_capture_retained_plan_state_(
        device_instance_id,
        /*requested_bump_access_posture_epoch=*/false);
    request_publish_from_core_unchecked();
  });

  return (pr == CoreThread::PostResult::Enqueued)
      ? TrySetStillCaptureProfileStatus::OK
      : TrySetStillCaptureProfileStatus::Busy;
}

TrySetWarmHoldStatus CoreRuntime::try_set_device_warm_hold_ms(
    uint64_t device_instance_id,
    uint32_t warm_hold_ms) noexcept {
  if (device_instance_id == 0) {
    return TrySetWarmHoldStatus::InvalidArgument;
  }
  if (core_thread_.is_core_thread()) {
    // See try_start_stream(): this wrapper would self-deadlock if called from
    // the core thread itself. Fail fast rather than hang.
    return TrySetWarmHoldStatus::Busy;
  }
  auto status_promise = std::make_shared<std::promise<TrySetWarmHoldStatus>>();
  std::future<TrySetWarmHoldStatus> status_future = status_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, device_instance_id, warm_hold_ms, status_promise]() {
    const CoreDeviceRegistry::DeviceRecord* rec = devices_.find(device_instance_id);
    if (rec == nullptr || !rec->open) {
      status_promise->set_value(TrySetWarmHoldStatus::Busy);
      return;
    }
    (void)devices_.set_warm_hold_ms(device_instance_id, warm_hold_ms);
    request_publish_from_core_unchecked();
    status_promise->set_value(TrySetWarmHoldStatus::OK);
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return TrySetWarmHoldStatus::Busy;
  }
  if (status_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return TrySetWarmHoldStatus::Busy;
  }
  return status_future.get();
}

bool CoreRuntime::materialize_capture_request_for_server(uint64_t device_instance_id, CaptureRequest& out) const {
  out = CaptureRequest{};
  if (device_instance_id == 0) {
    return false;
  }

  if (core_thread_.is_core_thread()) {
    return materialize_capture_request_(device_instance_id, out);
  }

  auto completion = std::make_shared<std::promise<std::pair<bool, CaptureRequest>>>();
  std::future<std::pair<bool, CaptureRequest>> completed = completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  const CoreThread::PostResult pr = self->try_post([this, device_instance_id, completion]() {
    CaptureRequest request{};
    const bool ok = materialize_capture_request_(device_instance_id, request);
    completion->set_value(std::make_pair(ok, request));
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return false;
  }

  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return false;
  }
  const auto result = completed.get();
  if (!result.first) {
    return false;
  }
  out = result.second;
  return true;
}

bool CoreRuntime::materialize_capture_request(uint64_t device_instance_id, CaptureRequest& out) const {
  return materialize_capture_request_for_server(device_instance_id, out);
}

bool CoreRuntime::materialize_capture_request_(uint64_t device_instance_id, CaptureRequest& out) const {
  assert(core_thread_.is_core_thread());

  if (!build_effective_capture_request_without_retained_plan_(
          device_instance_id, out)) {
    return false;
  }
  const ResolvedCaptureRetainedPlanParent parent =
      resolve_capture_retained_plan_parent_(device_instance_id);
  if (parent.acquisition_session_id != 0) {
    if (const auto* session =
            acquisition_sessions_.find(parent.acquisition_session_id);
        session && session->requested_retained_plan.valid) {
      out.requested_retained_plan = session->requested_retained_plan;
      return true;
    }
  }
  if (const auto* rec = devices_.find(device_instance_id);
      rec && rec->requested_retained_plan.valid) {
    out.requested_retained_plan = rec->requested_retained_plan;
    return true;
  }
  for (const auto& [key, state] : capture_retained_plan_evaluators_) {
    (void)key;
    if (state.device_instance_id != device_instance_id ||
        !state.active ||
        state.current_candidate_index >= state.candidate_count) {
      continue;
    }
    const CoreRetainedProductionPlan requested =
        state.candidate_sequence[state.current_candidate_index];
    if (!requested.valid) {
      continue;
    }
    out.requested_retained_plan = requested;
    return true;
  }
  for (const auto& [key, decision] : capture_retained_plan_decisions_) {
    (void)key;
    if (decision.device_instance_id != device_instance_id ||
        !decision.valid ||
        !decision.selected.valid) {
      continue;
    }
    out.requested_retained_plan = decision.selected;
    return true;
  }
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  if (!self->refresh_capture_retained_plan_state_(
          device_instance_id,
          /*requested_bump_access_posture_epoch=*/false)) {
    return false;
  }
  const ResolvedCaptureRetainedPlanParent refreshed_parent =
      resolve_capture_retained_plan_parent_(device_instance_id);
  if (refreshed_parent.acquisition_session_id != 0) {
    if (const auto* session =
            acquisition_sessions_.find(refreshed_parent.acquisition_session_id);
        session && session->requested_retained_plan.valid) {
      out.requested_retained_plan = session->requested_retained_plan;
      return true;
    }
  }
  if (const auto* rec = devices_.find(device_instance_id);
      rec && rec->requested_retained_plan.valid) {
    out.requested_retained_plan = rec->requested_retained_plan;
    return true;
  }
  return false;
}

TryTriggerDeviceCaptureStatus CoreRuntime::trigger_device_capture_with_capture_id_(
    uint64_t device_instance_id,
    uint64_t capture_id) {
  assert(core_thread_.is_core_thread());

  if (device_instance_id == 0 || capture_id == 0) {
    return TryTriggerDeviceCaptureStatus::InvalidArgument;
  }

  (void)integrate_pending_provider_facts_before_capture_request_();

  CaptureRequest req{};
  if (!materialize_capture_request_(device_instance_id, req)) {
    CaptureRequest effective{};
    ProducerBackingCapabilities runtime_caps{};
    ProducerBackingCapabilities parent_context_caps{};
    (void)runtime_caps;
    if (build_effective_capture_request_without_retained_plan_(
            device_instance_id, effective) &&
        resolve_capture_backing_capabilities_(
            device_instance_id,
            effective,
            runtime_caps,
            parent_context_caps)) {
      CoreProductionPostureShape viable_postures[3]{};
      if (build_viable_candidate_order(
              BackingPlanEvaluationPrimaryFunction::CaptureReadyAndMaterialize,
              parent_context_caps,
              viable_postures,
              3u) == 0u) {
        capture_latency_trace_printf(
            "capture_admission_unavailable capture_id=%llu device_id=%llu reason=no_viable_parent_context_postures primary_function=%s runtime_caps_cpu=%u runtime_caps_gpu=%u runtime_caps_gpu_cpu_sidecar=%u parent_context_caps_cpu=%u parent_context_caps_gpu=%u parent_context_caps_gpu_cpu_sidecar=%u",
            static_cast<unsigned long long>(capture_id),
            static_cast<unsigned long long>(device_instance_id),
            backing_plan_primary_function_name(
                BackingPlanEvaluationPrimaryFunction::CaptureReadyAndMaterialize),
            runtime_caps.cpu_backed_available ? 1u : 0u,
            runtime_caps.gpu_backed_available ? 1u : 0u,
            runtime_caps.gpu_with_cpu_sidecar_available ? 1u : 0u,
            parent_context_caps.cpu_backed_available ? 1u : 0u,
            parent_context_caps.gpu_backed_available ? 1u : 0u,
            parent_context_caps.gpu_with_cpu_sidecar_available ? 1u : 0u);
        return TryTriggerDeviceCaptureStatus::Unavailable;
      }
    }
    return TryTriggerDeviceCaptureStatus::Busy;
  }
  req.capture_id = capture_id;
  if (!req.requested_retained_plan.valid) {
    capture_latency_trace_printf(
        "capture_admission_unavailable capture_id=%llu device_id=%llu reason=no_requested_backing_plan_after_refresh",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id));
    return TryTriggerDeviceCaptureStatus::Unavailable;
  }
  (void)devices_.set_requested_retained_plan(
      device_instance_id,
      req.requested_retained_plan,
      /*bump_capture_access_posture_epoch=*/false);

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryTriggerDeviceCaptureStatus::Busy;
  }
  req.admission_context = make_capture_admission_context_();
  req.has_admission_context = true;
  const ProviderResult pr = prov->trigger_capture(req);
  if (!pr.ok()) {
    return TryTriggerDeviceCaptureStatus::ProviderRejected;
  }
  capture_assembly_registry_.record_admission_context(
      capture_id, device_instance_id, req.admission_context, req.still_image_bundle);

  begin_capture_stream_preemption_(capture_id, device_instance_id);
  (void)suppress_queued_repeating_stream_frames_for_capture_();
  return TryTriggerDeviceCaptureStatus::OK;
}

TryTriggerDeviceCaptureStatus CoreRuntime::try_trigger_device_capture_with_capture_id_for_server(
    uint64_t device_instance_id,
    uint64_t capture_id) {
  if (device_instance_id == 0 || capture_id == 0) {
    return TryTriggerDeviceCaptureStatus::InvalidArgument;
  }

  if (core_thread_.is_core_thread()) {
    const uint64_t direct_begin_ns = capture_latency_trace_now_ns();
    const TryTriggerDeviceCaptureStatus status =
        trigger_device_capture_with_capture_id_(device_instance_id, capture_id);
    const uint64_t direct_end_ns = capture_latency_trace_now_ns();
    capture_latency_trace_printf(
        "core_device_admission capture_id=%llu device_id=%llu post_to_core_us=0 core_queue_wait_us=0 core_execution_us=%llu status=%u path=direct",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>((direct_end_ns - direct_begin_ns) / 1000ull),
        static_cast<unsigned>(status));
    capture_latency_trace_printf(
        "core_device_future capture_id=%llu device_id=%llu post_to_core_us=0 future_wait_us=0 total_us=%llu status=%u path=direct",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>((direct_end_ns - direct_begin_ns) / 1000ull),
        static_cast<unsigned>(status));
    return status;
  }

  auto completion = std::make_shared<std::promise<TryTriggerDeviceCaptureStatus>>();
  std::future<TryTriggerDeviceCaptureStatus> completed = completion->get_future();
  const uint64_t post_begin_ns = capture_latency_trace_now_ns();
  const CoreThread::DiagnosticSnapshot post_snapshot = core_thread_.diagnostic_snapshot();
  const CoreThread::PostResult pr = try_post([this,
                                              device_instance_id,
                                              capture_id,
                                              completion,
                                              post_begin_ns,
                                              post_snapshot]() {
    const uint64_t core_start_ns = capture_latency_trace_now_ns();
    const CoreThread::DiagnosticSnapshot start_snapshot = core_thread_.diagnostic_snapshot();
    const size_t runtime_request_depth_at_start = requests_.size();
    const size_t runtime_provider_fact_depth_at_start = provider_facts_.size();
    const TryTriggerDeviceCaptureStatus status =
        trigger_device_capture_with_capture_id_(device_instance_id, capture_id);
    const uint64_t core_end_ns = capture_latency_trace_now_ns();
    const uint64_t core_queue_wait_us = (core_start_ns - post_begin_ns) / 1000ull;
    capture_latency_trace_emit_core_command_wait_context(
        "device_capture",
        capture_id,
        device_instance_id,
        "device_id",
        core_queue_wait_us,
        post_snapshot,
        start_snapshot,
        runtime_request_depth_at_start,
        runtime_provider_fact_depth_at_start);
    capture_latency_trace_printf(
        "core_device_admission capture_id=%llu device_id=%llu post_to_core_us=0 core_queue_wait_us=%llu core_execution_us=%llu status=%u path=queued",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>(core_queue_wait_us),
        static_cast<unsigned long long>((core_end_ns - core_start_ns) / 1000ull),
        static_cast<unsigned>(status));
    completion->set_value(status);
  });
  const uint64_t post_end_ns = capture_latency_trace_now_ns();
  if (pr != CoreThread::PostResult::Enqueued) {
    capture_latency_trace_printf(
        "core_device_admission_post_failed capture_id=%llu device_id=%llu post_us=%llu post_result=%u",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>((post_end_ns - post_begin_ns) / 1000ull),
        static_cast<unsigned>(pr));
    return TryTriggerDeviceCaptureStatus::Busy;
  }

  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return TryTriggerDeviceCaptureStatus::Busy;
  }
  const TryTriggerDeviceCaptureStatus status = completed.get();
  const uint64_t wait_end_ns = capture_latency_trace_now_ns();
  capture_latency_trace_printf(
      "core_device_future capture_id=%llu device_id=%llu post_to_core_us=%llu future_wait_us=%llu total_us=%llu status=%u path=queued",
      static_cast<unsigned long long>(capture_id),
      static_cast<unsigned long long>(device_instance_id),
      static_cast<unsigned long long>((post_end_ns - post_begin_ns) / 1000ull),
      static_cast<unsigned long long>((wait_end_ns - post_end_ns) / 1000ull),
      static_cast<unsigned long long>((wait_end_ns - post_begin_ns) / 1000ull),
      static_cast<unsigned>(status));
  return status;
}

CoreRuntime::RigPreflightResult CoreRuntime::preflight_rig_participants_materialize_(uint64_t rig_id) const {
  assert(core_thread_.is_core_thread());

  if (rig_id == 0) {
    return make_rig_preflight_failure(rig_id, RigPreflightFailure::RigNotFound);
  }

  const auto* rig = rigs_.find(rig_id);
  if (!rig) {
    return make_rig_preflight_failure(rig_id, RigPreflightFailure::RigNotFound);
  }
  if (rig->member_hardware_ids.empty()) {
    return make_rig_preflight_failure(rig_id, RigPreflightFailure::EmptyMembership);
  }

  std::vector<RigPreflightParticipant> participants;
  participants.reserve(rig->member_hardware_ids.size());
  std::map<uint64_t, bool> seen_devices;

  for (size_t i = 0; i < rig->member_hardware_ids.size(); ++i) {
    const std::string& hardware_id = rig->member_hardware_ids[i];
    uint64_t resolved_device_id = 0;
    size_t matches = 0;
    for (const auto& [device_instance_id, rec] : devices_.all()) {
      if (rec.hardware_id == hardware_id && rec.open) {
        ++matches;
        resolved_device_id = device_instance_id;
      }
    }

    if (matches == 0) {
      return make_rig_preflight_failure(
          rig_id, RigPreflightFailure::HardwareIdUnresolved, i, hardware_id);
    }
    if (matches > 1) {
      return make_rig_preflight_failure(
          rig_id, RigPreflightFailure::HardwareIdAmbiguous, i, hardware_id);
    }
    if (seen_devices.find(resolved_device_id) != seen_devices.end()) {
      return make_rig_preflight_failure(
          rig_id,
          RigPreflightFailure::DuplicateResolvedDevice,
          i,
          hardware_id,
          resolved_device_id);
    }
    seen_devices.emplace(resolved_device_id, true);

    CaptureRequest req{};
    if (!materialize_capture_request_(resolved_device_id, req)) {
      return make_rig_preflight_failure(
          rig_id,
          RigPreflightFailure::MaterializeFailed,
          i,
          hardware_id,
          resolved_device_id);
    }

    RigPreflightParticipant participant{};
    participant.hardware_id = hardware_id;
    participant.device_instance_id = resolved_device_id;
    participant.request = req;
    participants.push_back(std::move(participant));
  }

  return make_rig_preflight_success(rig_id, std::move(participants));
}

CoreRuntime::RigAdmittedRequestBundle CoreRuntime::admit_rig_cohort_from_preflight_(
    uint64_t rig_id,
    uint64_t capture_id,
    const RigPreflightResult& preflight) {
  if (capture_id == 0 || rig_id == 0) {
    return make_rig_admitted_failure(
        rig_id, capture_id, RigCohortAdmissionFailure::InvalidCaptureId);
  }
  if (!preflight.ok || preflight.failure != RigPreflightFailure::None || preflight.rig_id != rig_id) {
    return make_rig_admitted_failure(
        rig_id, capture_id, RigCohortAdmissionFailure::PreflightFailed);
  }
  if (preflight.participants.empty()) {
    return make_rig_admitted_failure(
        rig_id, capture_id, RigCohortAdmissionFailure::EmptyParticipants);
  }
  const RigCohortAdmissionFailure imaging_spec_failure =
      grouped_rig_imaging_spec_admission_failure_(preflight);
  if (imaging_spec_failure != RigCohortAdmissionFailure::None) {
    return make_rig_admitted_failure(
        rig_id, capture_id, imaging_spec_failure);
  }

  CoreCaptureCohortRegistry::CohortRecord cohort{};
  cohort.capture_id = capture_id;
  cohort.rig_id = rig_id;
  cohort.expected_participants.reserve(preflight.participants.size());
  for (const auto& p : preflight.participants) {
    if (p.device_instance_id == 0) {
      return make_rig_admitted_failure(
          rig_id, capture_id, RigCohortAdmissionFailure::PreflightFailed);
    }
    cohort.expected_participants.push_back({p.device_instance_id, p.hardware_id});
  }

  if (!capture_cohort_registry_.insert(std::move(cohort))) {
    return make_rig_admitted_failure(
        rig_id, capture_id, RigCohortAdmissionFailure::DuplicateCaptureId);
  }

  const CaptureAdmissionContext context = make_capture_admission_context_();
  const bool context_set = capture_cohort_registry_.set_admission_context(capture_id, context);
  assert(context_set);
  (void)context_set;

  std::vector<RigAdmittedParticipantRequest> participants;
  participants.reserve(preflight.participants.size());
  for (const auto& p : preflight.participants) {
    RigAdmittedParticipantRequest ap{};
    ap.hardware_id = p.hardware_id;
    ap.request = p.request;
    ap.request.capture_id = capture_id;
    ap.request.rig_id = rig_id;
    ap.request.device_instance_id = p.device_instance_id;
    ap.request.admission_context = context;
    ap.request.has_admission_context = true;
    participants.push_back(std::move(ap));
  }

  for (const auto& participant : participants) {
    capture_assembly_registry_.record_admission_context(
        capture_id, participant.request.device_instance_id,
        participant.request.admission_context, participant.request.still_image_bundle);
  }

  return make_rig_admitted_success(
      rig_id, capture_id, std::move(participants));
}

CoreRuntime::RigCohortAdmissionFailure
CoreRuntime::grouped_rig_imaging_spec_admission_failure_(
    const RigPreflightResult& preflight) const noexcept {
  if (preflight.participants.size() <= 1) {
    return RigCohortAdmissionFailure::None;
  }

  const CoreSpecState::ImagingSpecInterpretation imaging_spec =
      spec_state_.interpret_imaging_spec();
  switch (imaging_spec.camera_concurrency.kind) {
    case camera_concurrency::TruthKind::Unavailable:
      return RigCohortAdmissionFailure::ImagingSpecUnavailable;
    case camera_concurrency::TruthKind::Unsupported:
      return RigCohortAdmissionFailure::ImagingSpecRejected;
    case camera_concurrency::TruthKind::Supported:
      break;
  }

  std::vector<std::string> requested_camera_ids;
  requested_camera_ids.reserve(preflight.participants.size());
  for (const auto& participant : preflight.participants) {
    requested_camera_ids.push_back(participant.hardware_id);
  }
  if (camera_concurrency::requested_camera_id_set_is_allowed(
          imaging_spec.camera_concurrency,
          requested_camera_ids)) {
    return RigCohortAdmissionFailure::None;
  }
  return RigCohortAdmissionFailure::ImagingSpecRejected;
}

CoreRuntime::RigSubmissionResult CoreRuntime::submit_admitted_rig_bundle_(
    const RigAdmittedRequestBundle& bundle) {
  if (!bundle.ok || bundle.capture_id == 0 || bundle.rig_id == 0 || bundle.participants.empty()) {
    return make_rig_submission_failure(
        bundle.rig_id, bundle.capture_id, RigSubmissionFailure::InvalidBundle);
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    for (const auto& participant : bundle.participants) {
      capture_assembly_registry_.mark_capture_failed(bundle.capture_id,
                                                     participant.request.device_instance_id,
                                                     static_cast<uint32_t>(ProviderError::ERR_BAD_STATE));
    }
    (void)capture_cohort_registry_.mark_failed(bundle.capture_id,
                                               0,
                                               static_cast<uint32_t>(ProviderError::ERR_BAD_STATE),
                                               CoreCaptureCohortRegistry::CohortFailurePhase::SUBMISSION);
    return make_rig_submission_provider_unavailable(
        bundle.rig_id,
        bundle.capture_id,
        static_cast<uint32_t>(ProviderError::ERR_BAD_STATE));
  }

  CaptureSubmission submission{};
  submission.capture_id = bundle.capture_id;
  submission.origin = CaptureSubmissionOrigin::RIG_CAPTURE;
  submission.rig_id = bundle.rig_id;
  submission.device_requests.reserve(bundle.participants.size());

  for (size_t i = 0; i < bundle.participants.size(); ++i) {
    const auto& participant = bundle.participants[i];
    if (!is_valid_capture_still_image_bundle(
            participant.request.still_image_bundle,
            prov->supports_multi_image_still_sequence())) {
      // Reject the whole submission (policy unchanged), but record a terminal
      // fact for every bundle participant, not just the one that failed
      // validation: earlier siblings already validated in this loop must not
      // be left with no recorded assembly state.
      for (const auto& all_participants : bundle.participants) {
        capture_assembly_registry_.mark_capture_failed(bundle.capture_id,
                                                       all_participants.request.device_instance_id,
                                                       static_cast<uint32_t>(ProviderError::ERR_INVALID_ARGUMENT));
      }
      (void)capture_cohort_registry_.mark_failed(bundle.capture_id,
                                                 participant.request.device_instance_id,
                                                 static_cast<uint32_t>(ProviderError::ERR_INVALID_ARGUMENT),
                                                 CoreCaptureCohortRegistry::CohortFailurePhase::SUBMISSION);
      return make_rig_submission_trigger_failed(
          bundle.rig_id,
          bundle.capture_id,
          i,
          participant.request.device_instance_id,
          static_cast<uint32_t>(ProviderError::ERR_INVALID_ARGUMENT));
    }
    submission.device_requests.push_back(participant.request);
  }

  const ProviderResult pr = prov->trigger_capture_submission(submission);
  if (!pr.ok()) {
    const uint64_t failed_device_instance_id =
        submission.device_requests.empty() ? 0 : submission.device_requests.front().device_instance_id;
    for (const auto& participant : bundle.participants) {
      capture_assembly_registry_.mark_capture_failed(bundle.capture_id,
                                                     participant.request.device_instance_id,
                                                     static_cast<uint32_t>(pr.code));
    }
    (void)capture_cohort_registry_.mark_failed(bundle.capture_id,
                                               failed_device_instance_id,
                                               static_cast<uint32_t>(pr.code),
                                               CoreCaptureCohortRegistry::CohortFailurePhase::SUBMISSION);
    return make_rig_submission_trigger_failed(
        bundle.rig_id,
        bundle.capture_id,
        0,
        failed_device_instance_id,
        static_cast<uint32_t>(pr.code));
  }

  begin_capture_stream_preemption_for_bundle_(bundle);
  (void)suppress_queued_repeating_stream_frames_for_capture_();

  return make_rig_submission_success(
      bundle.rig_id, bundle.capture_id, bundle.participants.size());
}

CoreRuntime::RigTriggerOrchestrationResult CoreRuntime::orchestrate_rig_capture_from_preflight_(
    uint64_t rig_id,
    uint64_t capture_id,
    const RigPreflightResult& preflight) {
  assert(core_thread_.is_core_thread());

  if (!preflight.ok) {
    return make_rig_orchestration_preflight_failure(
        rig_id, capture_id, preflight.failure);
  }

  if (capture_id == 0) {
    return make_rig_orchestration_invalid_capture_id(rig_id, capture_id);
  }

  const RigAdmittedRequestBundle admitted = admit_rig_cohort_from_preflight_(rig_id, capture_id, preflight);
  if (!admitted.ok) {
    return make_rig_orchestration_admission_failure(
        rig_id, capture_id, admitted.failure);
  }

  const RigSubmissionResult submitted = submit_admitted_rig_bundle_(admitted);
  if (!submitted.ok) {
    return make_rig_orchestration_submission_failure(submitted);
  }

  return make_rig_orchestration_success(
      rig_id, capture_id, submitted.submitted_count);
}

CoreRuntime::RigTriggerOrchestrationResult CoreRuntime::orchestrate_rig_capture_with_capture_id_(
    uint64_t rig_id,
    uint64_t capture_id) {
  assert(core_thread_.is_core_thread());

  const RigPreflightResult preflight = preflight_rig_participants_materialize_(rig_id);
  return orchestrate_rig_capture_from_preflight_(rig_id, capture_id, preflight);
}

#if defined(CAMBANG_INTERNAL_SMOKE)
CoreRuntime::RigPreflightResult CoreRuntime::preflight_rig_participants_materialize(uint64_t rig_id) const {
  if (core_thread_.is_core_thread()) {
    return preflight_rig_participants_materialize_(rig_id);
  }

  auto completion = std::make_shared<std::promise<RigPreflightResult>>();
  std::future<RigPreflightResult> completed = completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  const CoreThread::PostResult pr = self->try_post([this, rig_id, completion]() {
    completion->set_value(preflight_rig_participants_materialize_(rig_id));
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return make_rig_preflight_failure(rig_id, RigPreflightFailure::RigNotFound);
  }
  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return make_rig_preflight_failure(rig_id, RigPreflightFailure::RigNotFound);
  }
  return completed.get();
}

bool CoreRuntime::smoke_set_rig_member_hardware_ids(uint64_t rig_id, std::vector<std::string> member_hardware_ids) {
  if (rig_id == 0) {
    return false;
  }

  if (core_thread_.is_core_thread()) {
    const bool retained = rigs_.retain_member_hardware_ids(rig_id, std::move(member_hardware_ids));
    request_publish_from_core_unchecked();
    return retained;
  }

  auto completion = std::make_shared<std::promise<bool>>();
  std::future<bool> completed = completion->get_future();
  const CoreThread::PostResult pr = try_post([this, rig_id, member_hardware_ids = std::move(member_hardware_ids), completion]() mutable {
    const bool retained = rigs_.retain_member_hardware_ids(rig_id, std::move(member_hardware_ids));
    request_publish_from_core_unchecked();
    completion->set_value(retained);
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return false;
  }

  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return false;
  }
  return completed.get();
}

CoreRuntime::RigAdmittedRequestBundle CoreRuntime::smoke_admit_rig_cohort_from_preflight(
    uint64_t rig_id,
    uint64_t capture_id,
    const RigPreflightResult& preflight) {
  if (core_thread_.is_core_thread()) {
    return admit_rig_cohort_from_preflight_(rig_id, capture_id, preflight);
  }

  auto completion = std::make_shared<std::promise<RigAdmittedRequestBundle>>();
  std::future<RigAdmittedRequestBundle> completed = completion->get_future();
  const CoreThread::PostResult pr = try_post([this, rig_id, capture_id, preflight, completion]() {
    completion->set_value(admit_rig_cohort_from_preflight_(rig_id, capture_id, preflight));
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return make_rig_admitted_failure(
        rig_id, capture_id, RigCohortAdmissionFailure::PreflightFailed);
  }

  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return make_rig_admitted_failure(
        rig_id, capture_id, RigCohortAdmissionFailure::PreflightFailed);
  }
  return completed.get();
}

CoreRuntime::RigTriggerOrchestrationResult CoreRuntime::smoke_orchestrate_rig_capture_from_preflight(
    uint64_t rig_id,
    uint64_t capture_id,
    const RigPreflightResult& preflight) {
  if (core_thread_.is_core_thread()) {
    return orchestrate_rig_capture_from_preflight_(rig_id, capture_id, preflight);
  }

  auto completion = std::make_shared<std::promise<RigTriggerOrchestrationResult>>();
  std::future<RigTriggerOrchestrationResult> completed = completion->get_future();
  const CoreThread::PostResult pr = try_post([this, rig_id, capture_id, preflight, completion]() {
    completion->set_value(orchestrate_rig_capture_from_preflight_(rig_id, capture_id, preflight));
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return make_rig_orchestration_preflight_failure(
        rig_id, capture_id, RigPreflightFailure::RigNotFound);
  }

  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return make_rig_orchestration_preflight_failure(
        rig_id, capture_id, RigPreflightFailure::RigNotFound);
  }
  return completed.get();
}

CoreRuntime::RigSubmissionResult CoreRuntime::smoke_submit_admitted_rig_bundle(
    const RigAdmittedRequestBundle& bundle) {
  if (core_thread_.is_core_thread()) {
    return submit_admitted_rig_bundle_(bundle);
  }

  auto completion = std::make_shared<std::promise<RigSubmissionResult>>();
  std::future<RigSubmissionResult> completed = completion->get_future();
  const CoreThread::PostResult pr = try_post([this, bundle, completion]() {
    completion->set_value(submit_admitted_rig_bundle_(bundle));
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return make_rig_submission_provider_unavailable(
        bundle.rig_id,
        bundle.capture_id,
        static_cast<uint32_t>(ProviderError::ERR_BAD_STATE));
  }

  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return make_rig_submission_provider_unavailable(
        bundle.rig_id,
        bundle.capture_id,
        static_cast<uint32_t>(ProviderError::ERR_BAD_STATE));
  }
  return completed.get();
}

CoreRuntime::RigTriggerOrchestrationResult CoreRuntime::smoke_orchestrate_rig_capture_with_capture_id(
    uint64_t rig_id,
    uint64_t capture_id) {
  return orchestrate_rig_capture_with_capture_id_for_server(rig_id, capture_id);
}

#endif

CoreRuntime::RigTriggerOrchestrationResult CoreRuntime::orchestrate_rig_capture_with_capture_id_for_server(
    uint64_t rig_id,
    uint64_t capture_id) {
  if (core_thread_.is_core_thread()) {
    const uint64_t direct_begin_ns = capture_latency_trace_now_ns();
    RigTriggerOrchestrationResult result = orchestrate_rig_capture_with_capture_id_(rig_id, capture_id);
    const uint64_t direct_end_ns = capture_latency_trace_now_ns();
    capture_latency_trace_printf(
        "core_rig_admission capture_id=%llu rig_id=%llu post_to_core_us=0 core_queue_wait_us=0 core_execution_us=%llu ok=%u submitted=%llu path=direct",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(rig_id),
        static_cast<unsigned long long>((direct_end_ns - direct_begin_ns) / 1000ull),
        result.ok ? 1u : 0u,
        static_cast<unsigned long long>(result.submitted_count));
    capture_latency_trace_printf(
        "core_rig_future capture_id=%llu rig_id=%llu post_to_core_us=0 future_wait_us=0 total_us=%llu ok=%u submitted=%llu path=direct",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(rig_id),
        static_cast<unsigned long long>((direct_end_ns - direct_begin_ns) / 1000ull),
        result.ok ? 1u : 0u,
        static_cast<unsigned long long>(result.submitted_count));
    return result;
  }

  auto completion = std::make_shared<std::promise<RigTriggerOrchestrationResult>>();
  std::future<RigTriggerOrchestrationResult> completed = completion->get_future();
  const uint64_t post_begin_ns = capture_latency_trace_now_ns();
  const CoreThread::DiagnosticSnapshot post_snapshot = core_thread_.diagnostic_snapshot();
  const CoreThread::PostResult pr = try_post([this, rig_id, capture_id, completion, post_begin_ns, post_snapshot]() {
    const uint64_t core_start_ns = capture_latency_trace_now_ns();
    const CoreThread::DiagnosticSnapshot start_snapshot = core_thread_.diagnostic_snapshot();
    const size_t runtime_request_depth_at_start = requests_.size();
    const size_t runtime_provider_fact_depth_at_start = provider_facts_.size();
    RigTriggerOrchestrationResult result = orchestrate_rig_capture_with_capture_id_(rig_id, capture_id);
    const uint64_t core_end_ns = capture_latency_trace_now_ns();
    const uint64_t core_queue_wait_us = (core_start_ns - post_begin_ns) / 1000ull;
    capture_latency_trace_emit_core_command_wait_context(
        "rig_capture",
        capture_id,
        rig_id,
        "rig_id",
        core_queue_wait_us,
        post_snapshot,
        start_snapshot,
        runtime_request_depth_at_start,
        runtime_provider_fact_depth_at_start);
    capture_latency_trace_printf(
        "core_rig_admission capture_id=%llu rig_id=%llu post_to_core_us=0 core_queue_wait_us=%llu core_execution_us=%llu ok=%u submitted=%llu path=queued",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(rig_id),
        static_cast<unsigned long long>(core_queue_wait_us),
        static_cast<unsigned long long>((core_end_ns - core_start_ns) / 1000ull),
        result.ok ? 1u : 0u,
        static_cast<unsigned long long>(result.submitted_count));
    completion->set_value(std::move(result));
  });
  const uint64_t post_end_ns = capture_latency_trace_now_ns();
  if (pr != CoreThread::PostResult::Enqueued) {
    capture_latency_trace_printf(
        "core_rig_admission_post_failed capture_id=%llu rig_id=%llu post_us=%llu post_result=%u",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(rig_id),
        static_cast<unsigned long long>((post_end_ns - post_begin_ns) / 1000ull),
        static_cast<unsigned>(pr));
    return make_rig_orchestration_submission_failure(
        make_rig_submission_provider_unavailable(
            rig_id,
            capture_id,
            static_cast<uint32_t>(ProviderError::ERR_BAD_STATE)));
  }

  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return make_rig_orchestration_submission_failure(
        make_rig_submission_provider_unavailable(
            rig_id,
            capture_id,
            static_cast<uint32_t>(ProviderError::ERR_BAD_STATE)));
  }
  RigTriggerOrchestrationResult result = completed.get();
  const uint64_t wait_end_ns = capture_latency_trace_now_ns();
  capture_latency_trace_printf(
      "core_rig_future capture_id=%llu rig_id=%llu post_to_core_us=%llu future_wait_us=%llu total_us=%llu ok=%u submitted=%llu path=queued",
      static_cast<unsigned long long>(capture_id),
      static_cast<unsigned long long>(rig_id),
      static_cast<unsigned long long>((post_end_ns - post_begin_ns) / 1000ull),
      static_cast<unsigned long long>((wait_end_ns - post_end_ns) / 1000ull),
      static_cast<unsigned long long>((wait_end_ns - post_begin_ns) / 1000ull),
      result.ok ? 1u : 0u,
      static_cast<unsigned long long>(result.submitted_count));
  return result;
}

bool CoreRuntime::retain_rig_member_hardware_ids(
    uint64_t rig_id,
    const std::vector<std::string>& member_hardware_ids) {
  if (rig_id == 0) {
    return false;
  }
  auto pr = try_post([this, rig_id, member_hardware_ids]() {
    (void)rigs_.retain_member_hardware_ids(rig_id, member_hardware_ids);
    request_publish_from_core_unchecked();
  });
  return pr == CoreThread::PostResult::Enqueued;
}

CoreRuntime::IngestCameraConcurrencyResult
CoreRuntime::ingest_camera_concurrency_json_for_server(
    const std::string& json_text) {
  const CoreRuntimeState state = state_.load(std::memory_order_acquire);
  if (state == CoreRuntimeState::LIVE ||
      state == CoreRuntimeState::TEARING_DOWN) {
    IngestCameraConcurrencyResult out{};
    out.status = IngestCameraConcurrencyStatus::Busy;
    return out;
  }

  const camera_concurrency::LoadResult load =
      camera_concurrency::load_truth_from_adc_json_text(json_text);
  if (!load.ok) {
    IngestCameraConcurrencyResult out{};
    out.status = load.error_kind == camera_concurrency::LoadErrorKind::Parse
                     ? IngestCameraConcurrencyStatus::ParseError
                     : IngestCameraConcurrencyStatus::Invalid;
    out.error_message = load.error_message;
    return out;
  }

  IngestCameraConcurrencyResult out{};
  out.status = IngestCameraConcurrencyStatus::Ok;
  {
    const std::lock_guard<std::mutex> lock(configured_imaging_spec_mutex_);
    configured_imaging_spec_version_ = next_configured_imaging_spec_version_++;
    configured_external_camera_description_.reset();
    configured_camera_description_version_ = 0;
    configured_imaging_spec_payload_.assign(
        reinterpret_cast<const uint8_t*>(json_text.data()),
        reinterpret_cast<const uint8_t*>(json_text.data()) + json_text.size());
    out.imaging_spec_version = configured_imaging_spec_version_;
  }
  return out;
}

CoreRuntime::ReplaceExternalCameraDescriptionResult
CoreRuntime::replace_external_camera_description_json_for_internal(
    const std::string& json_text) {
  const CoreRuntimeState state = state_.load(std::memory_order_acquire);
  if (state == CoreRuntimeState::LIVE || state == CoreRuntimeState::TEARING_DOWN) {
    return ReplaceExternalCameraDescriptionResult{
        ReplaceExternalCameraDescriptionStatus::Busy, {}, 0, 0};
  }
  const adc_camera_description::LoadResult load =
      adc_camera_description::load_replacement_from_json_text(json_text);
  if (!load.ok) {
    return ReplaceExternalCameraDescriptionResult{
        load.error_kind == adc_camera_description::LoadErrorKind::Parse
            ? ReplaceExternalCameraDescriptionStatus::ParseError
            : ReplaceExternalCameraDescriptionStatus::Invalid,
        load.error_message,
        0,
        0};
  }
  ReplaceExternalCameraDescriptionResult out{};
  {
    const std::lock_guard<std::mutex> lock(configured_imaging_spec_mutex_);
    configured_camera_description_version_ = next_configured_camera_description_version_++;
    configured_imaging_spec_version_ = load.state.concurrency().has_value()
        ? next_configured_imaging_spec_version_++
        : 0;
    configured_imaging_spec_payload_.clear();
    configured_external_camera_description_ = load.state;
    out.camera_description_version = configured_camera_description_version_;
    out.imaging_spec_version = configured_imaging_spec_version_;
  }
  return out;
}


void CoreRuntime::release_stream_display_demand_async(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  const CoreThread::PostResult pr = core_thread_.try_post_essential([this, stream_id]() {
    result_store_.release_stream_display_demand(stream_id);
  });
  if (pr == CoreThread::PostResult::Enqueued) {
    return;
  }
  account_display_demand_release_async_post_failure_(pr);
}

void CoreRuntime::account_display_demand_release_async_post_failure_(CoreThread::PostResult result) noexcept {
  switch (result) {
    case CoreThread::PostResult::QueueFull:
      display_demand_release_async_dropped_full_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Closed:
      display_demand_release_async_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::AllocFail:
      display_demand_release_async_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Enqueued:
      break;
  }
}

CoreRuntime::Stats CoreRuntime::stats_copy() const noexcept {
  Stats s;
  s.publish_requests_coalesced = publish_requests_coalesced_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_full = publish_requests_dropped_full_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_closed = publish_requests_dropped_closed_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_allocfail = publish_requests_dropped_allocfail_.load(std::memory_order_relaxed);
  s.display_demand_release_async_dropped_full =
      display_demand_release_async_dropped_full_.load(std::memory_order_relaxed);
  s.display_demand_release_async_dropped_closed =
      display_demand_release_async_dropped_closed_.load(std::memory_order_relaxed);
  s.display_demand_release_async_dropped_allocfail =
      display_demand_release_async_dropped_allocfail_.load(std::memory_order_relaxed);
  return s;
}

CoreRuntime::ShutdownDiag CoreRuntime::shutdown_diag_copy() const noexcept {
  ShutdownDiag d;
  d.phase_code = shutdown_phase_code_.load(std::memory_order_relaxed);
  d.phase_changes = shutdown_phase_changes_.load(std::memory_order_relaxed);
  return d;
}

void CoreRuntime::request_publish() {
  // Lifecycle gating: only LIVE accepts publish work.
  if (state_.load(std::memory_order_acquire) != CoreRuntimeState::LIVE) {
    publish_requests_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
    publish_pending_.store(false, std::memory_order_release);
    return;
  }

  // Coalesce publish requests to avoid spamming the provider_to_core_commands queue.
  const bool was_pending = publish_pending_.exchange(true, std::memory_order_acq_rel);
  if (was_pending) {
    publish_requests_coalesced_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Schedule a pump tick to perform publish after draining facts/requests.
  // We post directly to CoreThread (not via CoreRuntime::try_post) to avoid
  // routing this through the requests_ queue.
  const CoreThread::PostResult r = core_thread_.try_post([this]() {
    assert(core_thread_.is_core_thread());
    core_thread_.request_timer_tick();
  });

  if (r == CoreThread::PostResult::Enqueued) {
    return;
  }

  // Failed to enqueue: clear pending so callers can retry later.
  publish_pending_.store(false, std::memory_order_release);

  switch (r) {
    case CoreThread::PostResult::QueueFull:
      publish_requests_dropped_full_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Closed:
      publish_requests_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::AllocFail:
      publish_requests_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Enqueued:
      break;
  }
}

void CoreRuntime::enqueue_provider_fact(ProviderToCoreCommand&& cmd) {
  assert(core_thread_.is_core_thread());
  if (suppress_repeating_stream_frame_for_capture_(std::move(cmd))) {
    core_thread_.request_timer_tick();
    return;
  }
  if (provider_fact_has_capture_id_for_priority(cmd)) {
    ++provider_capture_facts_queued_;
  }
  provider_facts_.push_back(std::move(cmd));
  // Facts wake the core pump so descendant realization can be observed through
  // dispatcher state changes and the normal coalesced publish path. Scenario
  // start must not force host-side publication ahead of provider truth.
  core_thread_.request_timer_tick();
}

void CoreRuntime::enqueue_request(CoreThread::Task task) {
  assert(core_thread_.is_core_thread());
  requests_.push_back(std::move(task));
  // Requests also wake the core pump; any snapshot publication remains a
  // consequence of provider facts or core-owned state mutation, not a broad
  // publish request from the host scenario-start path.
  core_thread_.request_timer_tick();
}

void CoreRuntime::request_publish_from_core_unchecked() {
  assert(core_thread_.is_core_thread());
  // Coalesce naturally: if already pending, keep it pending.
  publish_pending_.store(true, std::memory_order_release);
}

} // namespace cambang
