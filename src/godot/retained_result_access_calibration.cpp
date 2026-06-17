#include "godot/retained_result_access_calibration.h"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/core_runtime.h"
#include "godot/cambang_capture_result.h"
#include "godot/cambang_stream_result.h"
#include "godot/result_access_cost_evidence.h"

namespace cambang::retained_result_access_calibration {
namespace {

struct CalibrationIdentity final {
  std::string surface;
  uint64_t posture_id = 0;
  uint64_t stream_id = 0;
  uint32_t image_member_index = 0;

  bool operator<(const CalibrationIdentity& other) const noexcept {
    if (surface != other.surface) return surface < other.surface;
    if (posture_id != other.posture_id) return posture_id < other.posture_id;
    if (stream_id != other.stream_id) return stream_id < other.stream_id;
    return image_member_index < other.image_member_index;
  }
};

std::mutex g_mutex;
std::set<CalibrationIdentity> g_completed;

bool mark_needed(const CalibrationIdentity& identity) {
  if (identity.posture_id == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_completed.insert(identity).second;
}

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
      costs.push_back(candidate.measurement.normalized_cost_ns_per_byte());
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
          result_access_cost_evidence::kRouteStreamToImageGpuSyntheticBackingMaterializer,
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
          result_access_cost_evidence::kRouteCaptureToImageGpuSyntheticBackingMaterializer,
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
    measurement = result_access_cost_evidence::latest_stream_measurement(
        result_access_cost_evidence::kRouteStreamToImageGpuPrimaryCpuSidecar,
        data->access_posture.posture_id);
  } else if (data->access_posture.has_retained_gpu_backing &&
             data->access_posture.gpu_materialization_available) {
    measurement = result_access_cost_evidence::latest_stream_measurement(
        result_access_cost_evidence::kRouteStreamToImageGpuSyntheticBackingMaterializer,
        data->access_posture.posture_id);
  }
  runtime->report_stream_retained_to_image_observation(
      data->stream_id,
      data->access_posture.posture_id,
      data->retained_access_truth.to_image,
      measurement.has_value() && measurement->success,
      measurement.has_value() && measurement->success
          ? measurement->normalized_cost_ns_per_byte()
          : 0);
}

void report_capture_to_image_observation(
    const SharedCaptureResultData& data,
    const CoreCaptureResultData::ImageMemberData& member,
    CoreRuntime* runtime) {
  if (!runtime || !data || data->device_instance_id == 0 ||
      member.access_posture.posture_id == 0 ||
      member.image_member_index != 0 ||
      member.role != CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED) {
    return;
  }
  std::optional<result_access_cost_evidence::RecordedAccessMeasurement> measurement;
  if (member.payload_kind == ResultPayloadKind::CPU_PACKED) {
    measurement = result_access_cost_evidence::latest_capture_measurement(
        result_access_cost_evidence::kRouteCaptureToImageCpuPacked,
        member.access_posture.posture_id,
        member.image_member_index);
  } else if (member.access_posture.has_retained_gpu_backing &&
             member.access_posture.gpu_materialization_available) {
    measurement = result_access_cost_evidence::latest_capture_measurement(
        result_access_cost_evidence::kRouteCaptureToImageGpuSyntheticBackingMaterializer,
        member.access_posture.posture_id,
        member.image_member_index);
  }
  runtime->report_capture_retained_to_image_observation(
      data->device_instance_id,
      member.access_posture.posture_id,
      member.retained_access_truth.to_image,
      measurement.has_value() && measurement->success,
      measurement.has_value() && measurement->success
          ? measurement->normalized_cost_ns_per_byte()
          : 0);
}

} // namespace

void calibrate_stream_result(const SharedStreamResultData& data,
                             CoreRuntime* runtime) {
  if (!data) {
    return;
  }
  const CalibrationIdentity display_identity{
      "stream_display_view", data->access_posture.posture_id, data->stream_id, 0};
  if (data->retained_access_truth.display_view != ResultCapability::UNSUPPORTED &&
      mark_needed(display_identity)) {
    (void)CamBANGStreamResult::calibrate_display_view_for_retained_access(data);
  }

  const CalibrationIdentity image_identity{
      "stream_to_image", data->access_posture.posture_id, data->stream_id, 0};
  if (data->retained_access_truth.to_image != ResultCapability::UNSUPPORTED &&
      mark_needed(image_identity)) {
    if (data->access_posture.has_retained_cpu_payload) {
      (void)CamBANGStreamResult::calibrate_to_image_cpu_payload_for_retained_access(data);
    }
    if (data->access_posture.has_retained_gpu_backing &&
        data->access_posture.gpu_materialization_available) {
      (void)CamBANGStreamResult::calibrate_to_image_gpu_materializer_for_retained_access(data);
    }
    refine_stream_to_image_classification(data);
    report_stream_to_image_observation(data, runtime);
  } else if (data->retained_access_truth.to_image == ResultCapability::UNSUPPORTED &&
             mark_needed(image_identity)) {
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
    const CalibrationIdentity identity{
        "capture_to_image", member->access_posture.posture_id, member->access_posture.stream_id,
        member->image_member_index};
    if (mark_needed(identity)) {
      if (member->retained_access_truth.to_image != ResultCapability::UNSUPPORTED &&
          member->access_posture.has_retained_cpu_payload) {
        (void)CamBANGCaptureResult::calibrate_to_image_member_cpu_payload_for_retained_access(
            data, member->image_member_index);
      }
      if (member->retained_access_truth.to_image != ResultCapability::UNSUPPORTED &&
          member->access_posture.has_retained_gpu_backing &&
          member->access_posture.gpu_materialization_available) {
        (void)CamBANGCaptureResult::calibrate_to_image_member_gpu_materializer_for_retained_access(
            data, member->image_member_index);
      }
      if (member->retained_access_truth.to_image != ResultCapability::UNSUPPORTED) {
        refine_capture_to_image_classification(*member);
      }
      report_capture_to_image_observation(data, *member, runtime);
    }
  }
}

void clear() noexcept {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_completed.clear();
}

} // namespace cambang::retained_result_access_calibration
