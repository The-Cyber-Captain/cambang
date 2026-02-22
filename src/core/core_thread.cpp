#include "core/core_thread.h"

#include <chrono>
#include <utility>

namespace cambang {

CoreThread::~CoreThread() {
  // Destructor enforces deterministic shutdown.
  // Core thread must not outlive its owner.
  stop();
}

bool CoreThread::start(IHooks* hooks) {
  // start() establishes the single core-thread ownership model.
  // Must only succeed once.
  if (!hooks) {
    return false;
  }

  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return false; // already running
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    hooks_ = hooks;
    stop_requested_ = false;
    timer_tick_requested_ = false;
    has_deadline_ = false;
    deadline_ns_ = 0;
    tasks_.clear();
  }

  // Reset accounting
  tasks_enqueued_.store(0, std::memory_order_relaxed);
  tasks_dropped_full_.store(0, std::memory_order_relaxed);
  tasks_dropped_closed_.store(0, std::memory_order_relaxed);
  tasks_dropped_allocfail_.store(0, std::memory_order_relaxed);

  thread_ = std::thread(&CoreThread::thread_main, this);
  return true;
}

void CoreThread::stop() {
  // stop() signals the core loop to exit and joins the thread.
  // Guarantees:
  // - on_core_stop() runs on the core thread before join completes.
  // - No tasks execute after stop() returns.
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_requested_ = true;
  }

  cv_.notify_one();

  if (thread_.joinable()) {
    thread_.join();
  }

  core_tid_ = std::thread::id{};

  running_.store(false, std::memory_order_release);
}

void CoreThread::post(Task task) {
  (void)try_post(std::move(task));
}

CoreThread::PostResult CoreThread::try_post(Task task) {
  // External ingress point.
  // All non-core threads must schedule work through this method.
  if (!task) {
    return PostResult::Enqueued; // nothing to do; treat as success
  }

  // Refuse new work if the core thread is not accepting tasks (teardown-in-flight).
  if (!running_.load(std::memory_order_acquire)) {
    tasks_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
    return PostResult::Closed;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (stop_requested_) {
      tasks_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
      return PostResult::Closed;
    }

    if (tasks_.size() >= kMaxPendingTasks) {
      tasks_dropped_full_.fetch_add(1, std::memory_order_relaxed);
      return PostResult::QueueFull;
    }

    try {
      tasks_.push_back(std::move(task));
    } catch (...) {
      // Allocation failure or unexpected exception while enqueueing.
      tasks_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      return PostResult::AllocFail;
    }
  }

  tasks_enqueued_.fetch_add(1, std::memory_order_relaxed);
  cv_.notify_one();
  return PostResult::Enqueued;
}

CoreThread::Stats CoreThread::stats_copy() const noexcept {
  Stats s;
  s.tasks_enqueued = tasks_enqueued_.load(std::memory_order_relaxed);
  s.tasks_dropped_full = tasks_dropped_full_.load(std::memory_order_relaxed);
  s.tasks_dropped_closed = tasks_dropped_closed_.load(std::memory_order_relaxed);
  s.tasks_dropped_allocfail = tasks_dropped_allocfail_.load(std::memory_order_relaxed);
  return s;
}

void CoreThread::request_timer_tick() {
  // Forces an immediate wake and hook tick on the core thread.
  {
    std::lock_guard<std::mutex> lock(mu_);
    timer_tick_requested_ = true;
  }

  cv_.notify_one();
}

void CoreThread::set_timer_deadline_ns(uint64_t deadline_ns) {
  // Scaffolding behavior:
  // - deadline_ns is currently interpreted as a relative delay hint.
  // - Core warm/retention scheduling will refine this later.
  {
    std::lock_guard<std::mutex> lock(mu_);
    has_deadline_ = true;
    deadline_ns_ = deadline_ns;
  }

  cv_.notify_one();
}

void CoreThread::clear_timer_deadline() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    has_deadline_ = false;
    deadline_ns_ = 0;
  }

  cv_.notify_one();
}

void CoreThread::drain_tasks_locked(std::deque<Task>& local) {
  // Moves all pending tasks into a local queue.
  // Guarantees:
  // - Tasks execute outside the mutex.
  // - FIFO order is preserved.
  local.clear();
  local.swap(tasks_);
}

void CoreThread::thread_main() {
  core_tid_ = std::this_thread::get_id();
  // From this point onward, execution is exclusively on the core thread.
  // No other thread may mutate core state.

  if (hooks_) {
    hooks_->on_core_start();
  }

  std::deque<Task> local;

  for (;;) {
    bool do_timer_tick = false;
    bool stopping = false;

    {
      std::unique_lock<std::mutex> lock(mu_);

      const bool has_deadline = has_deadline_;
      const uint64_t deadline_ns = deadline_ns_;

      auto predicate = [&]() {
        return stop_requested_ || !tasks_.empty() || timer_tick_requested_;
      };

      if (!has_deadline) {
        // Pure blocking mode: wait until work, timer request, or stop.
        cv_.wait(lock, predicate);
      } else {
        // Timed wait mode (scaffolding).
        // Interpret deadline_ns as relative nanoseconds from now.
        const auto now = std::chrono::steady_clock::now();
        const auto wait_dur = std::chrono::nanoseconds(deadline_ns);
        const auto wake_time = now + wait_dur;

        cv_.wait_until(lock, wake_time, predicate);

        // Conservative deadline detection.
        if (std::chrono::steady_clock::now() >= wake_time) {
          do_timer_tick = true;
        }
      }

      stopping = stop_requested_;

      if (timer_tick_requested_) {
        do_timer_tick = true;
        timer_tick_requested_ = false;
      }

      // Drain tasks while holding mutex, execute outside.
      drain_tasks_locked(local);
    }

    // Execute all tasks serially.
    // Determinism guarantee:
    // - No two tasks execute concurrently.
    // - Execution order matches FIFO post() order.
    for (auto& task : local) {
      task();
    }
    local.clear();

    if (do_timer_tick && hooks_) {
      // Timer tick runs strictly on the core thread.
      hooks_->on_core_timer_tick();
    }

    if (stopping) {
      break;
    }
  }

  if (hooks_) {
    hooks_->on_core_stop();
  }
}

} // namespace cambang
