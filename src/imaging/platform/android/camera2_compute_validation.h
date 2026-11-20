#pragma once

#include "imaging/api/capture_gpu_backing.h"
#include <vulkan/vulkan.h>

namespace cambang::camera2_detail {

bool validate_capture_compute(const CaptureGpuComputeRequest& request,
                              const VkPhysicalDeviceLimits& limits);

} // namespace cambang::camera2_detail
