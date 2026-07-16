#include <atomic>
#include <cstdio>
#include <mutex>
#include <optional>
#include <thread>

#include "godot/bounded_render_resource_release_queue.h"

namespace {

struct ReleaseProbe final {
  std::atomic<uint32_t>* destroyed_on_drain = nullptr;
  std::atomic<uint32_t>* destroyed_off_drain = nullptr;
  std::thread::id drain_thread;

  ReleaseProbe() = default;
  ReleaseProbe(
      std::atomic<uint32_t>* on_drain,
      std::atomic<uint32_t>* off_drain,
      std::thread::id drain)
      : destroyed_on_drain(on_drain), destroyed_off_drain(off_drain), drain_thread(drain) {}
  ReleaseProbe(ReleaseProbe&& other) noexcept
      : destroyed_on_drain(other.destroyed_on_drain),
        destroyed_off_drain(other.destroyed_off_drain),
        drain_thread(other.drain_thread) {
    other.destroyed_on_drain = nullptr;
  }
  ReleaseProbe& operator=(ReleaseProbe&& other) noexcept {
    if (this != &other) {
      destroyed_on_drain = other.destroyed_on_drain;
      destroyed_off_drain = other.destroyed_off_drain;
      drain_thread = other.drain_thread;
      other.destroyed_on_drain = nullptr;
    }
    return *this;
  }
  ReleaseProbe(const ReleaseProbe&) = delete;
  ReleaseProbe& operator=(const ReleaseProbe&) = delete;
  ~ReleaseProbe() {
    if (!destroyed_on_drain) {
      return;
    }
    if (std::this_thread::get_id() == drain_thread) {
      destroyed_on_drain->fetch_add(1, std::memory_order_relaxed);
    } else {
      destroyed_off_drain->fetch_add(1, std::memory_order_relaxed);
    }
  }
};

bool expect(bool value, const char* label) {
  if (value) {
    return true;
  }
  std::fprintf(stderr, "FAIL render_resource_release_queue_smoke %s\n", label);
  return false;
}

bool test_worker_owner_handoff_to_drain() {
  using Queue = cambang::BoundedRenderResourceReleaseQueue<ReleaseProbe, 1>;
  std::atomic<uint32_t> on_drain{0};
  std::atomic<uint32_t> off_drain{0};
  const std::thread::id drain_thread = std::this_thread::get_id();
  Queue queue;
  queue.open();
  Queue::AdmissionResult result = Queue::AdmissionResult::Closed;
  std::thread worker([&] {
    result = queue.admit(ReleaseProbe(&on_drain, &off_drain, drain_thread));
  });
  worker.join();
  if (!expect(result == Queue::AdmissionResult::Accepted, "worker_admitted")) return false;
  std::optional<ReleaseProbe> drained = queue.take_one();
  if (!expect(drained.has_value() && queue.size() == 0, "extracted_after_unlock")) return false;
  drained.reset();
  return expect(on_drain.load() == 1 && off_drain.load() == 0, "released_on_drain");
}

bool test_worker_full_uses_bounded_recovery_owner() {
  using Queue = cambang::BoundedRenderResourceReleaseQueue<ReleaseProbe, 1>;
  std::atomic<uint32_t> on_drain{0};
  std::atomic<uint32_t> off_drain{0};
  const std::thread::id drain_thread = std::this_thread::get_id();
  Queue queue;
  queue.open();
  if (!expect(queue.admit(ReleaseProbe(&on_drain, &off_drain, drain_thread)) ==
                  Queue::AdmissionResult::Accepted,
              "full_seed")) return false;
  std::optional<ReleaseProbe> emergency;
  Queue::AdmissionResult result = Queue::AdmissionResult::Closed;
  std::thread worker([&] {
    ReleaseProbe rejected(&on_drain, &off_drain, drain_thread);
    result = queue.admit(std::move(rejected));
    if (result == Queue::AdmissionResult::Full) {
      emergency.emplace(std::move(rejected));
    }
  });
  worker.join();
  if (!expect(result == Queue::AdmissionResult::Full && emergency.has_value(),
              "full_retained_in_bounded_emergency")) return false;
  if (!expect(off_drain.load() == 0, "full_not_destroyed_on_worker")) return false;
  std::optional<ReleaseProbe> accepted = queue.take_one();
  accepted.reset();
  emergency.reset();
  return expect(on_drain.load() == 2 && off_drain.load() == 0, "full_recovered_on_drain");
}

bool test_worker_allocation_failure_uses_bounded_recovery_owner() {
  using Queue = cambang::BoundedRenderResourceReleaseQueue<ReleaseProbe, 1>;
  std::atomic<uint32_t> on_drain{0};
  std::atomic<uint32_t> off_drain{0};
  const std::thread::id drain_thread = std::this_thread::get_id();
  Queue queue;
  queue.open();
  queue.force_next_allocation_failure_for_smoke();
  std::optional<ReleaseProbe> emergency;
  Queue::AdmissionResult result = Queue::AdmissionResult::Closed;
  std::thread worker([&] {
    ReleaseProbe rejected(&on_drain, &off_drain, drain_thread);
    result = queue.admit(std::move(rejected));
    if (result == Queue::AdmissionResult::AllocationFailure) {
      emergency.emplace(std::move(rejected));
    }
  });
  worker.join();
  if (!expect(result == Queue::AdmissionResult::AllocationFailure && emergency.has_value(),
              "allocation_failure_retained")) return false;
  if (!expect(off_drain.load() == 0, "allocation_failure_not_destroyed_on_worker")) return false;
  emergency.reset();
  return expect(on_drain.load() == 1 && off_drain.load() == 0,
                "allocation_failure_recovered_on_drain");
}

bool test_terminal_close_is_separate() {
  using Queue = cambang::BoundedRenderResourceReleaseQueue<ReleaseProbe, 1>;
  std::atomic<uint32_t> on_drain{0};
  std::atomic<uint32_t> off_drain{0};
  const std::thread::id drain_thread = std::this_thread::get_id();
  Queue queue;
  queue.open();
  if (!expect(queue.admit(ReleaseProbe(&on_drain, &off_drain, drain_thread)) ==
                  Queue::AdmissionResult::Accepted,
              "terminal_seed")) return false;
  queue.close_terminal();
  std::optional<ReleaseProbe> terminal = queue.take_one();
  if (!expect(terminal.has_value(), "terminal_extract")) return false;
  terminal.reset();
  return expect(on_drain.load() == 1 && off_drain.load() == 0, "terminal_release_context");
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
  run_test(test_worker_owner_handoff_to_drain);
  run_test(test_worker_full_uses_bounded_recovery_owner);
  run_test(test_worker_allocation_failure_uses_bounded_recovery_owner);
  run_test(test_terminal_close_is_separate);
  std::printf("%s render_resource_release_queue_smoke run=%u ok=%u\n",
              run == ok ? "PASS" : "FAIL", run, ok);
  return run == ok ? 0 : 1;
}
