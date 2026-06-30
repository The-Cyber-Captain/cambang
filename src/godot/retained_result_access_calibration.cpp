#include "godot/retained_result_access_calibration.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/core_runtime.h"
#include "godot/cambang_capture_result.h"
#include "godot/cambang_stream_result.h"
#include "godot/result_access_cost_evidence.h"

namespace cambang::retained_result_access_calibration {
namespace {

struct CandidateMeasurement final {
  result_access_cost_evidence::RecordedAccessMeasurement measurement{};
};

ResultCapability classify_supported_non_ready_from_measurements(
    ResultCapability provisional,
    const std::vector<CandidateMeasurement>& candidates) noexcept {
  std::vector<uint64_t> costs;
  costs.reserve(candidates.size());
  for (const CandidateMeasurement& candidate : candidates) {
    if (candidate.measurement.success) {
      costs.push_back(candidate.measurement.normalized_cost_units());
    }
  }
  return classify_supported_non_ready_result_access_from_normalized_costs(
      provisional,
      costs.empty() ? nullptr : costs.data(),
      costs.size());
}

void refine_stream_to_image_classification(const SharedStreamResultData& data) {
  if (!data || data->retained_access_truth.to_image == ResultCapability::UNSUPPORTED ||
      data->retained_access_truth.to_image == ResultCapability::READY) {
    return;
  }
  std::vector<CandidateMeasurement> candidates;
  if (auto measurement = result_access_cost_evidence::latest_stream_measurement(
          result_access_cost_evidence::kRouteStreamToImageCpuPacked,
          data->access_posture.posture_id);
      measurement && measurement->success) {
    candidates.push_back(CandidateMeasurement{*measurement});
  }
  if (auto measurement = result_access_cost_evidence::latest_stream_measurement(
          result_access_cost_evidence::kRouteStreamToImageGpuPrimaryCpuSidecar,
          data->access_posture.posture_id);
      measurement && measurement->success) {
    candidates.push_back(CandidateMeasurement{*measurement});
  }
  if (auto measurement = result_access_cost_evidence::latest_stream_measurement(
          result_access_cost_evidence::kRouteStreamToImageGpuPrimaryNoCpuSidecarMaterializer,
          data->access_posture.posture_id);
      measurement && measurement->success) {
    candidates.push_back(CandidateMeasurement{*measurement});
  }
  refine_result_access_classification(
      data->access_classification,
      CoreResultAccessOperation::TO_IMAGE,
      classify_supported_non_ready_from_measurements(data->retained_access_truth.to_image, candidates));
}

void refine_capture_to_image_classification(
    const CoreCaptureResultData::ImageMemberData& member) {
  if (member.retained_access_truth.to_image == ResultCapability::UNSUPPORTED ||
      member.retained_access_truth.to_image == ResultCapability::READY) {
    return;
  }
  std::vector<CandidateMeasurement> candidates;
  if (auto measurement = result_access_cost_evidence::latest_capture_measurement(
          result_access_cost_evidence::kRouteCaptureToImageCpuPacked,
          member.access_posture.posture_id,
          member.image_member_index);
      measurement && measurement->success) {
    candidates.push_back(CandidateMeasurement{*measurement});
  }
  if (auto measurement = result_access_cost_evidence::latest_capture_measurement(
          result_access_cost_evidence::kRouteCaptureToImageGpuPrimaryCpuSidecar,
          member.access_posture.posture_id,
          member.image_member_index);
      measurement && measurement->success) {
    candidates.push_back(CandidateMeasurement{*measurement});
  }
  if (auto measurement = result_access_cost_evidence::latest_capture_measurement(
          result_access_cost_evidence::kRouteCaptureToImageGpuPrimaryNoCpuSidecarMaterializer,
          member.access_posture.posture_id,
          member.image_member_index);
      measurement && measurement->success) {
    candidates.push_back(CandidateMeasurement{*measurement});
  }
  refine_result_access_classification(
      member.access_classification,
      CoreResultAccessOperation::TO_IMAGE,
      classify_supported_non_ready_from_measurements(member.retained_access_truth.to_image, candidates));
}

void report_stream_to_image_observation(
    const SharedStreamResultData& data,
    CoreRuntime* runtime) {
  if (!runtime || !data || data->stream_id == 0 || data->access_posture.posture_id == 0) {
    return;
  }
  std::optional<result_access_cost_evidence::RecordedAccessMeasurement> measurement;
  if (data->payload_kind == ResultPayloadKind::CPU_PACKED) {
    measurement = result_access_cost_evidence::latest_stream_measurement(
        result_access_cost_evidence::kRouteStreamToImageCpuPacked,
        data->access_posture.posture_id);
  } else if (data->access_posture.has_retained_cpu_payload) {
    // Chooser/report consumption follows the public to_image() path for the
    // realized posture. A sidecar posture may also have an auxiliary GPU
    // materializer measurement, but that remains a separate maintainer-only
    // route rather than replacing the decision-facing public route.
    measurement = result_access_cost_evidence::latest_stream_measurement(
        result_access_cost_evidence::kRouteStreamToImageGpuPrimaryCpuSidecar,
        data->access_posture.posture_id);
  } else if (data->access_posture.has_retained_gpu_backing &&
             data->access_posture.gpu_materialization_available) {
    measurement = result_access_cost_evidence::latest_stream_measurement(
        result_access_cost_evidence::kRouteStreamToImageGpuPrimaryNoCpuSidecarMaterializer,
        data->access_posture.posture_id);
  }
  runtime->report_stream_retained_to_image_observation(
      data->stream_id,
      data->access_posture.posture_id,
      data->retained_access_truth.to_image,
      measurement.has_value() && measurement->success,
      measurement.has_value() && measurement->success
          ? measurement->normalized_cost_units()
          : 0);
}

void report_stream_display_view_observation(
    const SharedStreamResultData& data,
    CoreRuntime* runtime) {
  if (!runtime || !data || data->stream_id == 0 || data->access_posture.posture_id == 0) {
    return;
  }
  std::optional<result_access_cost_evidence::RecordedAccessMeasurement> measurement;
  if (data->payload_kind == ResultPayloadKind::GPU_SURFACE &&
      data->retained_gpu_backing) {
    measurement = result_access_cost_evidence::latest_stream_measurement(
        result_access_cost_evidence::kRouteStreamDisplayViewRetainedGpuBacking,
        data->access_posture.posture_id);
  } else {
    measurement = result_access_cost_evidence::latest_stream_measurement(
        result_access_cost_evidence::kRouteStreamDisplayViewCpuLiveDisplayView,
        data->access_posture.posture_id);
  }
  runtime->report_stream_retained_display_view_observation(
      data->stream_id,
      data->access_posture.posture_id,
      data->retained_access_truth.display_view,
      measurement.has_value() && measurement->success,
      measurement.has_value() && measurement->success
          ? measurement->elapsed_ns
          : 0);
}

void report_capture_to_image_observation(
    const SharedCaptureResultData& data,
    const CoreCaptureResultData::ImageMemberData& member,
    CoreRuntime* runtime) {
  if (!runtime || !data || data->device_instance_id == 0 ||
      member.access_posture.posture_id == 0) {
    return;
  }
  std::optional<result_access_cost_evidence::RecordedAccessMeasurement> measurement;
  if (member.payload_kind == ResultPayloadKind::CPU_PACKED) {
    measurement = result_access_cost_evidence::latest_capture_measurement(
        result_access_cost_evidence::kRouteCaptureToImageCpuPacked,
        member.access_posture.posture_id,
        member.image_member_index);
  } else if (member.access_posture.has_retained_cpu_payload) {
    // Capture follows the same split as stream: decision/report input tracks
    // the realized public to_image() route, while any sidecar-present GPU
    // materializer timing remains an auxiliary maintainer-only route.
    measurement = result_access_cost_evidence::latest_capture_measurement(
        result_access_cost_evidence::kRouteCaptureToImageGpuPrimaryCpuSidecar,
        member.access_posture.posture_id,
        member.image_member_index);
  } else if (member.access_posture.has_retained_gpu_backing &&
             member.access_posture.gpu_materialization_available) {
    measurement = result_access_cost_evidence::latest_capture_measurement(
        result_access_cost_evidence::kRouteCaptureToImageGpuPrimaryNoCpuSidecarMaterializer,
        member.access_posture.posture_id,
        member.image_member_index);
  }
  runtime->report_capture_retained_to_image_observation(
      data->device_instance_id,
      data->capture_id,
      data->acquisition_session_id,
      member.access_posture.posture_id,
      member.retained_access_truth.to_image,
      measurement.has_value() && measurement->success,
      measurement.has_value() && measurement->success
          ? measurement->elapsed_ns
          : 0,
      measurement.has_value() && measurement->success,
      measurement.has_value() && measurement->success
          ? measurement->normalized_cost_units()
          : 0,
      member.image_member_index);
}

} // namespace

void calibrate_stream_result(const SharedStreamResultData& data,
                             CoreRuntime* runtime) {
  if (!data) {
    return;
  }
  if (data->retained_access_truth.display_view != ResultCapability::UNSUPPORTED) {
    (void)CamBANGStreamResult::calibrate_display_view_for_retained_access(data);
    report_stream_display_view_observation(data, runtime);
  } else {
    report_stream_display_view_observation(data, runtime);
  }

  if (data->retained_access_truth.to_image != ResultCapability::UNSUPPORTED) {
    if (data->access_posture.has_retained_cpu_payload) {
      (void)CamBANGStreamResult::calibrate_to_image_cpu_payload_for_retained_access(data);
    }
    if (data->access_posture.has_retained_gpu_backing &&
        data->access_posture.gpu_materialization_available) {
      (void)CamBANGStreamResult::calibrate_to_image_gpu_materializer_for_retained_access(data);
    }
    refine_stream_to_image_classification(data);
    report_stream_to_image_observation(data, runtime);
  } else {
    report_stream_to_image_observation(data, runtime);
  }
}

void calibrate_capture_result(const SharedCaptureResultData& data,
                              CoreRuntime* runtime) {
  if (!data) {
    return;
  }
  for (uint32_t i = 0; i < data->image_member_count(); ++i) {
    const auto* member = data->image_member_at(i);
    if (!member) {
      continue;
    }
    if (member->retained_access_truth.to_image == ResultCapability::UNSUPPORTED) {
      report_capture_to_image_observation(data, *member, runtime);
      continue;
    }
    if (member->access_posture.has_retained_cpu_payload) {
      (void)CamBANGCaptureResult::calibrate_to_image_member_cpu_payload_for_retained_access(
          data, member->image_member_index);
    }
    if (member->access_posture.has_retained_gpu_backing &&
        member->access_posture.gpu_materialization_available) {
      (void)CamBANGCaptureResult::calibrate_to_image_member_gpu_materializer_for_retained_access(
          data, member->image_member_index);
    }
    refine_capture_to_image_classification(*member);
    report_capture_to_image_observation(data, *member, runtime);
  }
}

void report_capture_result_member_observation(
    const SharedCaptureResultData& data,
    uint32_t image_member_index,
    CoreRuntime* runtime) {
  if (!data || !runtime) {
    return;
  }
  const auto* member = data->image_member_at(image_member_index);
  if (!member) {
    return;
  }
  report_capture_to_image_observation(data, *member, runtime);
}

void report_capture_result_observation(const SharedCaptureResultData& data,
                                       CoreRuntime* runtime) {
  if (!data || !runtime) {
    return;
  }
  for (uint32_t i = 0; i < data->image_member_count(); ++i) {
    const auto* member = data->image_member_at(i);
    if (!member) {
      continue;
    }
    report_capture_to_image_observation(data, *member, runtime);
  }
}

void clear() noexcept {
}

} // namespace cambang::retained_result_access_calibration
