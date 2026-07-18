#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

#include "imaging/synthetic/gpu_backing_runtime.h"

namespace {

using namespace std::chrono_literals;
using cambang::SyntheticGpuBackingRuntimeOps;

struct BlockingCallGate final {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered = false;
  bool release = false;
  std::atomic<uint64_t> call_count{0};
};

BlockingCallGate g_gate;

bool blocking_roundtrip(
    const uint8_t*,
    uint32_t,
    uint32_t,
    uint32_t,
    std::vector<uint8_t>&) noexcept {
  g_gate.call_count.fetch_add(1, std::memory_order_relaxed);
  std::unique_lock<std::mutex> lock(g_gate.mutex);
  g_gate.entered = true;
  g_gate.changed.notify_all();
  g_gate.changed.wait(lock, [] { return g_gate.release; });
  return true;
}

bool available_true() noexcept {
  return true;
}

const SyntheticGpuBackingRuntimeOps kBlockingOps{
    &available_true,
    &blocking_roundtrip,
};

const SyntheticGpuBackingRuntimeOps kReplacementOps{
    &available_true,
};

bool wait_for_gate_entry() {
  std::unique_lock<std::mutex> lock(g_gate.mutex);
  return g_gate.changed.wait_for(lock, 2s, [] { return g_gate.entered; });
}

void release_gate() {
  {
    std::lock_guard<std::mutex> lock(g_gate.mutex);
    g_gate.release = true;
  }
  g_gate.changed.notify_all();
}

bool wait_until_admission_closed() {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!cambang::synthetic_gpu_backing_runtime_available()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

bool fail(const char* reason) {
  std::fprintf(stdout, "FAIL synthetic_gpu_backing_runtime_verify reason=%s\n", reason);
  std::fflush(stdout);
  return false;
}

bool run_clear_drain_check() {
  cambang::clear_synthetic_gpu_backing_runtime_ops();
  cambang::set_synthetic_gpu_backing_runtime_ops(&kBlockingOps);

  std::atomic<bool> call_result{false};
  std::thread caller([&] {
    std::vector<uint8_t> output;
    call_result.store(
        cambang::synthetic_gpu_backing_realize_rgba8_via_global_gpu(
            nullptr, 1, 1, 4, output),
        std::memory_order_release);
  });

  if (!wait_for_gate_entry()) {
    release_gate();
    caller.join();
    cambang::clear_synthetic_gpu_backing_runtime_ops();
    return fail("admitted_call_did_not_enter");
  }

  std::atomic<bool> clear_returned{false};
  std::thread clearer([&] {
    cambang::clear_synthetic_gpu_backing_runtime_ops();
    clear_returned.store(true, std::memory_order_release);
  });

  const bool admission_closed = wait_until_admission_closed();
  const bool returned_while_call_active = clear_returned.load(std::memory_order_acquire);
  std::vector<uint8_t> rejected_output;
  const bool late_call_admitted = cambang::synthetic_gpu_backing_realize_rgba8_via_global_gpu(
      nullptr, 1, 1, 4, rejected_output);
  const uint64_t calls_before_release = g_gate.call_count.load(std::memory_order_relaxed);

  release_gate();
  caller.join();
  clearer.join();

  if (!admission_closed) {
    return fail("clear_did_not_close_admission");
  }
  if (returned_while_call_active) {
    return fail("clear_returned_before_admitted_call");
  }
  if (late_call_admitted || calls_before_release != 1) {
    return fail("call_admitted_while_draining");
  }
  if (!call_result.load(std::memory_order_acquire)) {
    return fail("admitted_call_did_not_complete");
  }
  if (!clear_returned.load(std::memory_order_acquire)) {
    return fail("clear_did_not_return_after_quiescence");
  }
  if (cambang::synthetic_gpu_backing_runtime_available()) {
    return fail("runtime_remained_available_after_clear");
  }
  return true;
}

bool run_replacement_drain_check() {
  {
    std::lock_guard<std::mutex> lock(g_gate.mutex);
    g_gate.entered = false;
    g_gate.release = false;
  }
  g_gate.call_count.store(0, std::memory_order_relaxed);
  cambang::set_synthetic_gpu_backing_runtime_ops(&kBlockingOps);

  std::thread caller([] {
    std::vector<uint8_t> output;
    (void)cambang::synthetic_gpu_backing_realize_rgba8_via_global_gpu(
        nullptr, 1, 1, 4, output);
  });
  if (!wait_for_gate_entry()) {
    release_gate();
    caller.join();
    cambang::clear_synthetic_gpu_backing_runtime_ops();
    return fail("replacement_call_did_not_enter");
  }

  std::atomic<bool> install_returned{false};
  std::thread installer([&] {
    cambang::set_synthetic_gpu_backing_runtime_ops(&kReplacementOps);
    install_returned.store(true, std::memory_order_release);
  });

  const bool admission_closed = wait_until_admission_closed();
  const bool returned_while_call_active = install_returned.load(std::memory_order_acquire);
  release_gate();
  caller.join();
  installer.join();

  if (!admission_closed) {
    cambang::clear_synthetic_gpu_backing_runtime_ops();
    return fail("replacement_did_not_close_old_generation");
  }
  if (returned_while_call_active) {
    cambang::clear_synthetic_gpu_backing_runtime_ops();
    return fail("replacement_published_before_old_generation_drained");
  }
  if (!install_returned.load(std::memory_order_acquire) ||
      !cambang::synthetic_gpu_backing_runtime_available()) {
    cambang::clear_synthetic_gpu_backing_runtime_ops();
    return fail("replacement_generation_not_published");
  }

  cambang::clear_synthetic_gpu_backing_runtime_ops();
  cambang::clear_synthetic_gpu_backing_runtime_ops();
  return true;
}

} // namespace

int main() {
  if (!run_clear_drain_check() || !run_replacement_drain_check()) {
    return 1;
  }
  std::fprintf(stdout, "PASS synthetic_gpu_backing_runtime_verify\n");
  std::fflush(stdout);
  return 0;
}
