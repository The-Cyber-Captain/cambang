#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

#include "godot/display_demand_dispatcher.h"

namespace {

struct Probe final {
  std::atomic<uint32_t> retain_calls{0};
  std::atomic<uint32_t> release_calls{0};
  std::atomic<uint32_t> boundary_state_reads{0};
  std::atomic<uint64_t> last_release_token{0};
  std::atomic<uint32_t>* destruction_count = nullptr;
  std::mutex mutex;
  std::condition_variable entered_cv;
  std::condition_variable continue_cv;
  std::condition_variable uninstall_started_cv;
  bool block_release = false;
  bool release_entered = false;
  bool release_may_finish = false;
  bool uninstall_started = false;

  ~Probe() {
    if (destruction_count) {
      destruction_count->fetch_add(1, std::memory_order_relaxed);
    }
  }
};

bool expect(bool value, const char* label) {
  if (value) {
    return true;
  }
  std::fprintf(stderr, "FAIL display_demand_dispatcher_smoke %s\n", label);
  return false;
}

uint64_t retain_target(void* target, uint64_t stream_id) noexcept {
  static_cast<Probe*>(target)->retain_calls.fetch_add(1, std::memory_order_relaxed);
  return stream_id + 1000u;
}

void release_target(void* target, uint64_t lease_token) noexcept {
  Probe& probe = *static_cast<Probe*>(target);
  probe.release_calls.fetch_add(1, std::memory_order_relaxed);
  probe.last_release_token.store(lease_token, std::memory_order_relaxed);
  std::unique_lock<std::mutex> lock(probe.mutex);
  probe.release_entered = true;
  probe.entered_cv.notify_all();
  probe.continue_cv.wait(lock, [&probe] { return !probe.block_release || probe.release_may_finish; });
}

bool test_worker_release_is_target_only_and_balanced() {
  cambang::DisplayDemandDispatcher dispatcher;
  Probe probe;
  dispatcher.install(&probe, retain_target, release_target);
  const uint64_t token = dispatcher.retain(77);
  if (!expect(token == 1077, "retain_returned_exact_token")) return false;
  std::thread worker([&dispatcher, token] { dispatcher.release(token); });
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    probe.release_may_finish = true;
  }
  probe.continue_cv.notify_all();
  worker.join();
  return expect(probe.retain_calls.load(std::memory_order_relaxed) == 1 &&
                    probe.release_calls.load(std::memory_order_relaxed) == 1 &&
                    probe.last_release_token.load(std::memory_order_relaxed) == token &&
                    probe.boundary_state_reads.load(std::memory_order_relaxed) == 0,
                "worker_release_is_callback_only_and_balanced");
}

bool test_uninstall_waits_for_inflight_release_and_then_noops() {
  cambang::DisplayDemandDispatcher dispatcher;
  std::atomic<uint32_t> destruction_count{0};
  std::unique_ptr<Probe> owner = std::make_unique<Probe>();
  Probe& probe = *owner;
  probe.destruction_count = &destruction_count;
  probe.block_release = true;
  dispatcher.install(&probe, retain_target, release_target);

  const uint64_t token = dispatcher.retain(88);
  if (!expect(token == 1088, "inflight_retain_returned_token")) return false;
  std::thread worker([&dispatcher, token] { dispatcher.release(token); });
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    probe.entered_cv.wait(lock, [&probe] { return probe.release_entered; });
  }

  std::atomic<bool> uninstall_returned{false};
  std::thread uninstaller([&] {
    {
      std::lock_guard<std::mutex> lock(probe.mutex);
      probe.uninstall_started = true;
    }
    probe.uninstall_started_cv.notify_all();
    dispatcher.uninstall(&probe);
    uninstall_returned.store(true, std::memory_order_release);
  });
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    probe.uninstall_started_cv.wait(lock, [&probe] { return probe.uninstall_started; });
  }
  if (!expect(!uninstall_returned.load(std::memory_order_acquire), "uninstall_waits_for_release")) {
    {
      std::lock_guard<std::mutex> lock(probe.mutex);
      probe.release_may_finish = true;
    }
    probe.continue_cv.notify_all();
    worker.join();
    uninstaller.join();
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    probe.release_may_finish = true;
  }
  probe.continue_cv.notify_all();
  worker.join();
  uninstaller.join();
  if (!expect(uninstall_returned.load(std::memory_order_acquire), "uninstall_after_release")) return false;

  const uint32_t release_calls_before_delete = probe.release_calls.load(std::memory_order_relaxed);
  owner.reset();
  if (!expect(destruction_count.load(std::memory_order_relaxed) == 1,
              "target_destroyed_after_uninstall")) return false;
  dispatcher.release(token);
  return expect(dispatcher.retain(88) == 0 && release_calls_before_delete == 1,
                "post_uninstall_release_is_noop");
}

bool test_repeated_install_uninstall() {
  cambang::DisplayDemandDispatcher dispatcher;
  Probe first;
  Probe second;
  dispatcher.install(&first, retain_target, release_target);
  dispatcher.uninstall(&first);
  dispatcher.release(1099);
  dispatcher.install(&second, retain_target, release_target);
  const uint64_t token = dispatcher.retain(99);
  dispatcher.release(token);
  dispatcher.uninstall(&second);
  return expect(first.release_calls.load(std::memory_order_relaxed) == 0 &&
                    second.retain_calls.load(std::memory_order_relaxed) == 1 &&
                    second.release_calls.load(std::memory_order_relaxed) == 1 &&
                    second.last_release_token.load(std::memory_order_relaxed) == 1099,
                "repeated_install_uninstall");
}

} // namespace

int main() {
  uint32_t run = 0;
  uint32_t ok = 0;
  const auto run_test = [&run, &ok](bool (*test)()) {
    ++run;
    if (test()) {
      ++ok;
    }
  };
  run_test(test_worker_release_is_target_only_and_balanced);
  run_test(test_uninstall_waits_for_inflight_release_and_then_noops);
  run_test(test_repeated_install_uninstall);
  std::printf("%s display_demand_dispatcher_smoke run=%u ok=%u\n",
              run == ok ? "PASS" : "FAIL", run, ok);
  return run == ok ? 0 : 1;
}
