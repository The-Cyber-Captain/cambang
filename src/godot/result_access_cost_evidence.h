#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <godot_cpp/variant/dictionary.hpp>

#include "core/core_result_store.h"

namespace cambang::result_access_cost_evidence {

constexpr uint64_t kCheapWithinBestMultiplier =
    kResultAccessCheapWithinBestMultiplier;

constexpr const char* kRouteStreamToImageCpuPacked = "stream_to_image.cpu_packed";
constexpr const char* kRouteStreamToImageGpuPrimaryCpuSidecar = "stream_to_image.gpu_primary_cpu_sidecar";
constexpr const char* kRouteStreamToImageGpuPrimaryCpuSidecarMaterializer = "stream_to_image.gpu_primary_cpu_sidecar_materializer";
constexpr const char* kRouteStreamToImageGpuPrimaryNoCpuSidecarMaterializer = "stream_to_image.gpu_primary_no_cpu_sidecar_materializer";
constexpr const char* kRouteStreamDisplayViewRetainedGpuBacking = "stream_display_view.retained_gpu_backing";
constexpr const char* kRouteStreamDisplayViewCpuLiveDisplayView = "stream_display_view.cpu_live_display_view";
constexpr const char* kRouteStreamAccessUnsupported = "stream_access.unsupported";
constexpr const char* kRouteCaptureToImageCpuPacked = "capture_to_image.cpu_packed";
constexpr const char* kRouteCaptureToImageGpuPrimaryCpuSidecar = "capture_to_image.gpu_primary_cpu_sidecar";
constexpr const char* kRouteCaptureToImageGpuPrimaryCpuSidecarMaterializer = "capture_to_image.gpu_primary_cpu_sidecar_materializer";
constexpr const char* kRouteCaptureToImageGpuPrimaryNoCpuSidecarMaterializer = "capture_to_image.gpu_primary_no_cpu_sidecar_materializer";
constexpr const char* kRouteCaptureAccessUnsupported = "capture_access.unsupported";

struct RecordedAccessMeasurement {
  std::string route;
  uint64_t posture_id = 0;
  uint64_t elapsed_ns = 0;
  uint64_t estimated_result_bytes = 0;
  bool success = false;
  ResultCapability reported_capability = ResultCapability::UNSUPPORTED;

  uint64_t normalized_cost_ns_per_byte() const noexcept {
    return elapsed_ns / (estimated_result_bytes == 0 ? 1ull : estimated_result_bytes);
  }
};

void record_stream_access(
    const char* route,
    const SharedStreamResultData& data,
    uint64_t elapsed_ns,
    bool success,
    ResultCapability reported_capability) noexcept;

void record_capture_member_access(
    const char* route,
    const SharedCaptureResultData& data,
    const CoreCaptureResultData::ImageMemberData* member,
    uint64_t elapsed_ns,
    bool success,
    ResultCapability reported_capability) noexcept;

godot::Dictionary snapshot();
std::optional<RecordedAccessMeasurement> latest_stream_measurement(
    const char* route,
    uint64_t posture_id);
std::optional<RecordedAccessMeasurement> latest_capture_measurement(
    const char* route,
    uint64_t posture_id,
    uint32_t image_member_index);
void clear() noexcept;

} // namespace cambang::result_access_cost_evidence
