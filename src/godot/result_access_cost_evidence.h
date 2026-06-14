#pragma once

#include <cstdint>

#include <godot_cpp/variant/dictionary.hpp>

#include "core/core_result_store.h"

namespace cambang::result_access_cost_evidence {

constexpr const char* kRouteStreamToImageCpuPacked = "stream_to_image.cpu_packed";
constexpr const char* kRouteStreamToImageGpuPrimaryCpuSidecar = "stream_to_image.gpu_primary_cpu_sidecar";
constexpr const char* kRouteStreamToImageGpuSyntheticBackingMaterializer = "stream_to_image.gpu_synthetic_backing_materializer";
constexpr const char* kRouteStreamDisplayViewRetainedGpuBacking = "stream_display_view.retained_gpu_backing";
constexpr const char* kRouteStreamDisplayViewCpuLiveDisplayView = "stream_display_view.cpu_live_display_view";
constexpr const char* kRouteStreamAccessUnsupported = "stream_access.unsupported";

void record_stream_access(
    const char* route,
    const SharedStreamResultData& data,
    uint64_t elapsed_ns,
    bool success,
    ResultCapability reported_capability) noexcept;

godot::Dictionary snapshot();
void clear() noexcept;

} // namespace cambang::result_access_cost_evidence
