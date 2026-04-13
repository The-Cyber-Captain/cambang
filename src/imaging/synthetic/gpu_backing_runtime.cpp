#include "imaging/synthetic/gpu_backing_runtime.h"

#include <atomic>

namespace cambang {
namespace {

std::atomic<const SyntheticGpuBackingRuntimeOps*> g_ops{nullptr};

} // namespace

void set_synthetic_gpu_backing_runtime_ops(const SyntheticGpuBackingRuntimeOps* ops) noexcept {
  g_ops.store(ops, std::memory_order_release);
}

void clear_synthetic_gpu_backing_runtime_ops() noexcept {
  g_ops.store(nullptr, std::memory_order_release);
}

bool synthetic_gpu_backing_runtime_available() noexcept {
  const SyntheticGpuBackingRuntimeOps* ops = g_ops.load(std::memory_order_acquire);
  if (!ops || !ops->is_available) {
    return false;
  }
  return ops->is_available();
}

bool synthetic_gpu_backing_realize_rgba8_via_global_gpu(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes,
    std::vector<uint8_t>& out) noexcept {
  const SyntheticGpuBackingRuntimeOps* ops = g_ops.load(std::memory_order_acquire);
  if (!ops || !ops->realize_rgba8_global_gpu_roundtrip) {
    return false;
  }
  return ops->realize_rgba8_global_gpu_roundtrip(src, width, height, stride_bytes, out);
}

} // namespace cambang

