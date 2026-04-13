#include "imaging/synthetic/gpu_backing_runtime.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>

namespace cambang {
namespace {

std::atomic<const SyntheticGpuBackingRuntimeOps*> g_ops{nullptr};

bool gpu_trace_enabled() noexcept {
  const char* value = std::getenv("CAMBANG_DEV_SYNTH_GPU_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}

void trace_line(const char* message) {
  if (!gpu_trace_enabled()) {
    return;
  }
  std::fprintf(stdout, "[CamBANG][SyntheticGPU] %s\n", message);
  std::fflush(stdout);
}

} // namespace

void set_synthetic_gpu_backing_runtime_ops(const SyntheticGpuBackingRuntimeOps* ops) noexcept {
  g_ops.store(ops, std::memory_order_release);
  trace_line(ops ? "runtime_ops_set=true" : "runtime_ops_set=false");
}

void clear_synthetic_gpu_backing_runtime_ops() noexcept {
  g_ops.store(nullptr, std::memory_order_release);
  trace_line("runtime_ops_set=false");
}

bool synthetic_gpu_backing_runtime_available() noexcept {
  const SyntheticGpuBackingRuntimeOps* ops = g_ops.load(std::memory_order_acquire);
  if (!ops || !ops->is_available) {
    trace_line("runtime_available result=false reason=ops_unset_or_missing_is_available");
    return false;
  }
  const bool available = ops->is_available();
  trace_line(available ? "runtime_available result=true" : "runtime_available result=false");
  return available;
}

bool synthetic_gpu_backing_realize_rgba8_via_global_gpu(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes,
    std::vector<uint8_t>& out) noexcept {
  const SyntheticGpuBackingRuntimeOps* ops = g_ops.load(std::memory_order_acquire);
  if (!ops || !ops->realize_rgba8_global_gpu_roundtrip) {
    trace_line("roundtrip_attempt success=false reason=ops_unset_or_missing_roundtrip_fn");
    return false;
  }
  const bool ok = ops->realize_rgba8_global_gpu_roundtrip(src, width, height, stride_bytes, out);
  trace_line(ok ? "roundtrip_attempt success=true" : "roundtrip_attempt success=false");
  return ok;
}

} // namespace cambang
