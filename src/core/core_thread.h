// src/core/core_thread.h
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace cambang {

// CoreThread implements CamBANG's dedicated core thread and event loop (Model A).
//
// Threading model invariants (non-negotiable):
// - Exactly one dedicated CamBANG core thread owns all mutable core state.
// - Core logic MUST execute only on the core thread.
// - All external threads (providers, Godot, etc.) must marshal work into the core
//   via post()/try_post() (and never touch core state directly).
// - All tasks are executed serially (no concurrent core execution).
//
// Determinism invariants:
// - Tasks are executed in FIFO order.
// - There is no implicit background work; all work is explicit via post() or hook ticks.
// - Hooks are invoked only on the core thread.
//
// Blocking + timed-wait model:
// - The core loop blocks on a condition_variable.
// - A timed wake mechanism exists to support warm/retention scheduling and periodic sweeps.
// - Scheduling semantics are owned by core; this class only provides wake primitives.
//
// Mailbox hardening (Build slice C):
// - The posted-task queue is bounded.
// - try_post() is best-effort; it returns false if the queue is full.
// - post() is best-effort and drops on overflow (accounted).
class CoreThread final {
public:
  // NOTE: std::function may allocate; acceptable for scaffolding.
  // If/when this becomes a hot path, replace with a fixed-capacity mailbox payload.
  using Task = std::function<void()>;

  enum class PostResult : uint8_t {
    Enqueued,
    QueueFull,
    Closed,
    AllocFail,
  };

  struct Stats {
    uint64_t tasks_enqueued = 0;
    uint64_t tasks_dropped_full = 0;
    uint64_t tasks_dropped_closed = 0;
    uint64_t tasks_dropped_allocfail = 0;
  };

  // Optional core-thread hooks for periodic work (timer tick) and lifecycle.
  // All hook methods are invoked ONLY on the core thread.
  class IHooks {
  public:
    virtual ~IHooks() = default;

    // Called once on the core thread after the thread starts, before first loop iteration.
    virtual void on_core_start() {}

    // Called on the core thread when a timer tick is requested or when a timed deadline elapses.
    // Hook work must be bounded and return promptly.
    virtual void on_core_timer_tick() {}

    // Called once on the core thread just before exiting the thread main.
    virtual void on_core_stop() {}
  };

  CoreThread() = default;
  ~CoreThread();

  CoreThread(const CoreThread&) = delete;
  CoreThread& operator=(const CoreThread&) = delete;

  // Start the dedicated core thread.
  // - hooks must remain valid until stop() completes.
  // - returns false if already started or hooks is null.
  bool start(IHooks* hooks);

  // Stop and join the core thread.
  // - safe to call multiple times
  // - guarantees the core thread has exited before returning
  void stop();

  bool is_running() const { return running_.load(std::memory_order_acquire); }

  // Debug helper: returns true if called from the core thread.
  bool is_core_thread() const noexcept { return std::this_thread::get_id() == core_tid_; }

  // Bounded mailbox capacity (number of pending tasks).
  static constexpr size_t kMaxPendingTasks = 1024;

  // Post a unit of work to be executed on the core thread.
  // - thread-safe
  // - best-effort: drops on overflow (accounted)
  void post(Task task);

  // Best-effort post; returns a reason on failure.
  // - thread-safe
  // - does not block
  PostResult try_post(Task task);

  Stats stats_copy() const noexcept;

  // Request an immediate timer tick (wakes the core thread promptly).
  // - thread-safe
  // - results in hooks_->on_core_timer_tick() being called on the core thread
  void request_timer_tick();

  // Set/clear a timed wake deadline.
  // - thread-safe
  //
  // NOTE (scaffolding): deadline_ns_ is currently treated as a relative delay hint.
  // Precise scheduling semantics are implemented by core warm/retention logic; this class
  // only provides a timed-wake primitive.
  void set_timer_deadline_ns(uint64_t deadline_ns);
  void clear_timer_deadline();

private:
  void thread_main();

  // Drain tasks into a local queue; mu_ must be held.
  void drain_tasks_locked(std::deque<Task>& local);

  // Synchronization
  mutable std::mutex mu_;
  std::condition_variable cv_;

  std::thread thread_;
  std::thread::id core_tid_{};
  IHooks* hooks_ = nullptr; // non-owning; must outlive stop()

  // Work queue (protected by mu_).
  std::deque<Task> tasks_;

  // Post accounting
  std::atomic<uint64_t> tasks_enqueued_{0};
  std::atomic<uint64_t> tasks_dropped_full_{0};
  std::atomic<uint64_t> tasks_dropped_closed_{0};
  std::atomic<uint64_t> tasks_dropped_allocfail_{0};

  // Stop / running flags
  std::atomic<bool> running_{false};
  bool stop_requested_ = false;

  // Timer tick control (protected by mu_).
  bool timer_tick_requested_ = false;
  bool has_deadline_ = false;
  uint64_t deadline_ns_ = 0;
};

} // namespace cambang
