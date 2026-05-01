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

#if defined(CAMBANG_GDE_BUILD) && CAMBANG_GDE_BUILD
std::shared_ptr<void> synthetic_gpu_backing_retain_primary_gpu_backing_rgba8(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  const SyntheticGpuBackingRuntimeOps* ops = g_ops.load(std::memory_order_acquire);
  if (!ops || !ops->retain_primary_gpu_backing_rgba8) {
    trace_line("retain_gpu_backing success=false reason=ops_unset_or_missing_retain_fn");
    return {};
  }
  std::shared_ptr<void> backing = ops->retain_primary_gpu_backing_rgba8(src, width, height, stride_bytes);
  trace_line(backing ? "retain_gpu_backing success=true" : "retain_gpu_backing success=false");
  return backing;
}

std::shared_ptr<void> synthetic_gpu_backing_create_stream_live_gpu_backing_rgba8(
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  const SyntheticGpuBackingRuntimeOps* ops = g_ops.load(std::memory_order_acquire);
  if (!ops || !ops->create_stream_live_gpu_backing_rgba8) {
    trace_line("create_stream_live_gpu_backing success=false reason=ops_unset_or_missing_create_fn");
    return {};
  }
  std::shared_ptr<void> backing = ops->create_stream_live_gpu_backing_rgba8(width, height, stride_bytes);
  trace_line(backing ? "create_stream_live_gpu_backing success=true"
                     : "create_stream_live_gpu_backing success=false");
  return backing;
}

bool synthetic_gpu_backing_update_stream_live_gpu_backing_rgba8(
    const std::shared_ptr<void>& backing,
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  const SyntheticGpuBackingRuntimeOps* ops = g_ops.load(std::memory_order_acquire);
  if (!ops || !ops->update_stream_live_gpu_backing_rgba8) {
    trace_line("update_stream_live_gpu_backing success=false reason=ops_unset_or_missing_update_fn");
    return false;
  }
  const bool ok = ops->update_stream_live_gpu_backing_rgba8(backing, src, width, height, stride_bytes);
  trace_line(ok ? "update_stream_live_gpu_backing success=true" : "update_stream_live_gpu_backing success=false");
  return ok;
}

void synthetic_gpu_backing_release_stream_live_gpu_backing(std::shared_ptr<void>& backing) noexcept {
  const SyntheticGpuBackingRuntimeOps* ops = g_ops.load(std::memory_order_acquire);
  if (!ops || !ops->release_stream_live_gpu_backing) {
    trace_line("release_stream_live_gpu_backing skipped reason=ops_unset_or_missing_release_fn");
    backing.reset();
    return;
  }
  ops->release_stream_live_gpu_backing(backing);
  trace_line("release_stream_live_gpu_backing done");
}

bool synthetic_gpu_backing_take_update_timing_stats(
    uint64_t& upload_copy_calls,
    uint64_t& upload_copy_total_ns,
    uint64_t& upload_copy_max_ns,
    uint64_t& texture_update_calls,
    uint64_t& texture_update_total_ns,
    uint64_t& texture_update_max_ns,
    uint64_t& texture_update_skipped) noexcept {
  const SyntheticGpuBackingRuntimeOps* ops = g_ops.load(std::memory_order_acquire);
  if (!ops || !ops->take_update_timing_stats) {
    return false;
  }
  return ops->take_update_timing_stats(
      upload_copy_calls,
      upload_copy_total_ns,
      upload_copy_max_ns,
      texture_update_calls,
      texture_update_total_ns,
      texture_update_max_ns,
      texture_update_skipped);
}
#else
std::shared_ptr<void> synthetic_gpu_backing_create_stream_live_gpu_backing_rgba8(
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  (void)width;
  (void)height;
  (void)stride_bytes;
  return {};
}
bool synthetic_gpu_backing_update_stream_live_gpu_backing_rgba8(
    const std::shared_ptr<void>& backing,
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  (void)backing;
  (void)src;
  (void)width;
  (void)height;
  (void)stride_bytes;
  return false;
}
void synthetic_gpu_backing_release_stream_live_gpu_backing(std::shared_ptr<void>& backing) noexcept {
  backing.reset();
}
bool synthetic_gpu_backing_take_update_timing_stats(
    uint64_t& upload_copy_calls,
    uint64_t& upload_copy_total_ns,
    uint64_t& upload_copy_max_ns,
    uint64_t& texture_update_calls,
    uint64_t& texture_update_total_ns,
    uint64_t& texture_update_max_ns,
    uint64_t& texture_update_skipped) noexcept {
  (void)upload_copy_calls;
  (void)upload_copy_total_ns;
  (void)upload_copy_max_ns;
  (void)texture_update_calls;
  (void)texture_update_total_ns;
  (void)texture_update_max_ns;
  (void)texture_update_skipped;
  return false;
}
#endif

} // namespace cambang
