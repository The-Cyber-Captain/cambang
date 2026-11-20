#pragma once

#include "imaging/api/capture_gpu_backing.h"
#include <vulkan/vulkan.h>

namespace cambang::camera2_detail {

// Executed on the provider's queue thread. On success only the requested
// result buffer is copied to host memory.
VkResult run_capture_compute(VkDevice device, VkQueue queue, uint32_t family,
                             const VkPhysicalDeviceMemoryProperties& memory,
                             const std::array<VkImageView, 2>& planes,
                             const CaptureGpuComputeRequest& request,
                             std::vector<uint8_t>& output);

} // namespace cambang::camera2_detail
