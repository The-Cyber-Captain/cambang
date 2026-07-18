#include "imaging/synthetic/gpu_backing_runtime.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <mutex>

namespace cambang {
namespace {

enum class RuntimeOpsPhase {
  Closed,
  Active,
  Draining,
};

class RuntimeOpsRegistry final {
public:
  class CallLease final {
  public:
    CallLease() = default;
    CallLease(const CallLease&) = delete;
    CallLease& operator=(const CallLease&) = delete;
    CallLease(CallLease&&) = delete;
    CallLease& operator=(CallLease&&) = delete;

    ~CallLease() {
      if (ops_) {
        registry_->release_call_();
      }
    }

    explicit operator bool() const noexcept { return ops_ != nullptr; }
    const SyntheticGpuBackingRuntimeOps* ops() const noexcept { return ops_; }

  private:
    friend class RuntimeOpsRegistry;

    CallLease(RuntimeOpsRegistry& registry, const SyntheticGpuBackingRuntimeOps* ops) noexcept
        : registry_(&registry), ops_(ops) {}

    RuntimeOpsRegistry* registry_ = nullptr;
    const SyntheticGpuBackingRuntimeOps* ops_ = nullptr;
  };

  CallLease acquire_call() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (phase_ != RuntimeOpsPhase::Active || !ops_) {
      return {};
    }
    ++in_flight_calls_;
    return CallLease(*this, ops_);
  }

  void install(const SyntheticGpuBackingRuntimeOps* ops) noexcept {
    if (!ops) {
      clear();
      return;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    wait_for_other_drain_(lock);
    if (phase_ == RuntimeOpsPhase::Active && ops_ == ops) {
      return;
    }
    if (phase_ == RuntimeOpsPhase::Active) {
      begin_drain_();
      wait_for_in_flight_calls_(lock);
      finish_drain_();
    }

    ops_ = ops;
    phase_ = RuntimeOpsPhase::Active;
    state_changed_.notify_all();
  }

  void clear() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    wait_for_other_drain_(lock);
    if (phase_ == RuntimeOpsPhase::Closed) {
      return;
    }

    begin_drain_();
    wait_for_in_flight_calls_(lock);
    finish_drain_();
  }

private:
  void begin_drain_() noexcept {
    phase_ = RuntimeOpsPhase::Draining;
    ops_ = nullptr;
    state_changed_.notify_all();
  }

  void finish_drain_() noexcept {
    phase_ = RuntimeOpsPhase::Closed;
    state_changed_.notify_all();
  }

  void wait_for_other_drain_(std::unique_lock<std::mutex>& lock) noexcept {
    state_changed_.wait(lock, [this] { return phase_ != RuntimeOpsPhase::Draining; });
  }

  void wait_for_in_flight_calls_(std::unique_lock<std::mutex>& lock) noexcept {
    state_changed_.wait(lock, [this] { return in_flight_calls_ == 0; });
  }

  void release_call_() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    --in_flight_calls_;
    if (in_flight_calls_ == 0) {
      state_changed_.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable state_changed_;
  const SyntheticGpuBackingRuntimeOps* ops_ = nullptr;
  std::size_t in_flight_calls_ = 0;
  RuntimeOpsPhase phase_ = RuntimeOpsPhase::Closed;
};

RuntimeOpsRegistry g_registry;

bool gpu_trace_enabled() noexcept {
  const char* value = std::getenv("CAMBANG_DEV_SYNTH_GPU_TRACE");
  return value && value[0] != '\0' && value[0] != '0';
}

void trace_line(const char* message) {
  if (!gpu_trace_enabled()) {
    return;
  }
  std::fprintf(stdout, "[CamBANG][SyntheticGpu] %s\n", message);
  std::fflush(stdout);
}

} // namespace

void set_synthetic_gpu_backing_runtime_ops(const SyntheticGpuBackingRuntimeOps* ops) noexcept {
  g_registry.install(ops);
  trace_line(ops ? "runtime_ops_set=true" : "runtime_ops_set=false");
}

void clear_synthetic_gpu_backing_runtime_ops() noexcept {
  g_registry.clear();
  trace_line("runtime_ops_set=false");
}

bool synthetic_gpu_backing_runtime_available() noexcept {
  const RuntimeOpsRegistry::CallLease lease = g_registry.acquire_call();
  if (!lease || !lease.ops()->is_available) {
    trace_line("runtime_available result=false reason=ops_unset_or_missing_is_available");
    return false;
  }
  const bool available = lease.ops()->is_available();
  trace_line(available ? "runtime_available result=true" : "runtime_available result=false");
  return available;
}

bool synthetic_gpu_backing_realize_rgba8_via_global_gpu(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes,
    std::vector<uint8_t>& out) noexcept {
  const RuntimeOpsRegistry::CallLease lease = g_registry.acquire_call();
  if (!lease || !lease.ops()->realize_rgba8_global_gpu_roundtrip) {
    trace_line("roundtrip_attempt success=false reason=ops_unset_or_missing_roundtrip_fn");
    return false;
  }
  const bool ok = lease.ops()->realize_rgba8_global_gpu_roundtrip(src, width, height, stride_bytes, out);
  trace_line(ok ? "roundtrip_attempt success=true" : "roundtrip_attempt success=false");
  return ok;
}

#if (defined(CAMBANG_GDE_BUILD) && CAMBANG_GDE_BUILD) || \
    (defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE)
std::shared_ptr<void> synthetic_gpu_backing_retain_primary_gpu_backing_rgba8(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  const RuntimeOpsRegistry::CallLease lease = g_registry.acquire_call();
  if (!lease || !lease.ops()->retain_primary_gpu_backing_rgba8) {
    trace_line("retain_gpu_backing success=false reason=ops_unset_or_missing_retain_fn");
    return {};
  }
  std::shared_ptr<void> backing =
      lease.ops()->retain_primary_gpu_backing_rgba8(src, width, height, stride_bytes);
  trace_line(backing ? "retain_gpu_backing success=true" : "retain_gpu_backing success=false");
  return backing;
}

std::shared_ptr<void> synthetic_gpu_backing_create_stream_live_gpu_backing_rgba8(
    uint64_t stream_id,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  const RuntimeOpsRegistry::CallLease lease = g_registry.acquire_call();
  if (!lease || !lease.ops()->create_stream_live_gpu_backing_rgba8) {
    trace_line("create_stream_live_gpu_backing success=false reason=ops_unset_or_missing_create_fn");
    return {};
  }
  std::shared_ptr<void> backing =
      lease.ops()->create_stream_live_gpu_backing_rgba8(stream_id, width, height, stride_bytes);
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
  const RuntimeOpsRegistry::CallLease lease = g_registry.acquire_call();
  if (!lease || !lease.ops()->update_stream_live_gpu_backing_rgba8) {
    trace_line("update_stream_live_gpu_backing success=false reason=ops_unset_or_missing_update_fn");
    return false;
  }
  const bool ok =
      lease.ops()->update_stream_live_gpu_backing_rgba8(backing, src, width, height, stride_bytes);
  trace_line(ok ? "update_stream_live_gpu_backing success=true" : "update_stream_live_gpu_backing success=false");
  return ok;
}

void synthetic_gpu_backing_release_stream_live_gpu_backing(std::shared_ptr<void>& backing) noexcept {
  const RuntimeOpsRegistry::CallLease lease = g_registry.acquire_call();
  if (!lease || !lease.ops()->release_stream_live_gpu_backing) {
    trace_line("release_stream_live_gpu_backing skipped reason=ops_unset_or_missing_release_fn");
    backing.reset();
    return;
  }
  lease.ops()->release_stream_live_gpu_backing(backing);
  trace_line("release_stream_live_gpu_backing done");
}

bool synthetic_gpu_backing_can_materialize_to_image(const std::shared_ptr<void>& backing) noexcept {
  const RuntimeOpsRegistry::CallLease lease = g_registry.acquire_call();
  if (!lease || !lease.ops()->can_materialize_to_image) {
    trace_line("can_materialize_to_image result=false reason=ops_unset_or_missing_materialize_fn");
    return false;
  }
  const bool ok = lease.ops()->can_materialize_to_image(backing);
  trace_line(ok ? "can_materialize_to_image result=true" : "can_materialize_to_image result=false");
  return ok;
}

bool synthetic_gpu_backing_take_update_timing_stats(
    uint64_t& upload_copy_calls,
    uint64_t& upload_copy_total_ns,
    uint64_t& upload_copy_max_ns,
    uint64_t& texture_update_calls,
    uint64_t& texture_update_total_ns,
    uint64_t& texture_update_max_ns,
    uint64_t& texture_update_skipped) noexcept {
  const RuntimeOpsRegistry::CallLease lease = g_registry.acquire_call();
  if (!lease || !lease.ops()->take_update_timing_stats) {
    return false;
  }
  return lease.ops()->take_update_timing_stats(
      upload_copy_calls,
      upload_copy_total_ns,
      upload_copy_max_ns,
      texture_update_calls,
      texture_update_total_ns,
      texture_update_max_ns,
      texture_update_skipped);
}
bool synthetic_gpu_backing_peek_update_timing_stats(
    uint64_t& upload_copy_calls,
    uint64_t& upload_copy_total_ns,
    uint64_t& upload_copy_max_ns,
    uint64_t& texture_update_calls,
    uint64_t& texture_update_total_ns,
    uint64_t& texture_update_max_ns,
    uint64_t& texture_update_skipped) noexcept {
  const RuntimeOpsRegistry::CallLease lease = g_registry.acquire_call();
  if (!lease || !lease.ops()->peek_update_timing_stats) {
    return false;
  }
  return lease.ops()->peek_update_timing_stats(
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
    uint64_t stream_id,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept {
  (void)stream_id;
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
bool synthetic_gpu_backing_can_materialize_to_image(const std::shared_ptr<void>& backing) noexcept {
  (void)backing;
  return false;
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
bool synthetic_gpu_backing_peek_update_timing_stats(
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
