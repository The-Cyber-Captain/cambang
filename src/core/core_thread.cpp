#include "core/core_thread.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace cambang {

namespace {

// Stage A.3 fairness bound: ordinary provider/frame ingress remains FIFO and
// non-dropping, but CoreThread takes it in single-task slices so command-lane
// work posted while ordinary work is flowing is observed before another
// ordinary task runs.
constexpr size_t kMaxOrdinaryTasksPerCoreThreadTurn = 1;

bool bounded_core_thread_work_full(size_t command_size, size_t ordinary_size) noexcept {
  return command_size + ordinary_size >= CoreThread::kMaxPendingTasks;
}

} // namespace


// BEGIN TEMPORARY CAPTURE LATENCY DIAGNOSTICS
uint64_t CoreThread::diagnostic_now_ns_() noexcept {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

const char* CoreThread::diagnostic_phase_name(DiagnosticPhase phase) noexcept {
  switch (phase) {
    case DiagnosticPhase::Unknown: return "unknown";
    case DiagnosticPhase::IdleWaiting: return "idle_waiting";
    case DiagnosticPhase::EssentialLane: return "essential_lane";
    case DiagnosticPhase::CommandLane: return "command_lane";
    case DiagnosticPhase::OrdinaryLane: return "ordinary_lane";
    case DiagnosticPhase::TimerHook: return "timer_hook";
    case DiagnosticPhase::RuntimeProviderFactIntegration: return "runtime_provider_fact_integration";
    case DiagnosticPhase::RuntimeRequestDrain: return "runtime_request_drain";
    case DiagnosticPhase::RuntimeRetentionTimerWork: return "runtime_retention_timer_work";
    case DiagnosticPhase::RuntimeSnapshotPublication: return "runtime_snapshot_publication";
    case DiagnosticPhase::RuntimeShutdownChoreography: return "runtime_shutdown_choreography";
    case DiagnosticPhase::ShutdownStopChoreography: return "shutdown_stop_choreography";
  }
  return "unknown";
}

void CoreThread::diagnostic_update_depths_(size_t essential_depth,
                                           size_t command_depth,
                                           size_t ordinary_depth) noexcept {
  diagnostic_essential_queue_depth_.store(essential_depth, std::memory_order_relaxed);
  diagnostic_command_queue_depth_.store(command_depth, std::memory_order_relaxed);
  diagnostic_ordinary_queue_depth_.store(ordinary_depth, std::memory_order_relaxed);
}

void CoreThread::diagnostic_set_phase_from_core(DiagnosticPhase phase) noexcept {
  const uint8_t next = static_cast<uint8_t>(phase);
  const uint8_t previous = diagnostic_phase_.load(std::memory_order_relaxed);
  const uint64_t now_ns = diagnostic_now_ns_();
  if (previous != next) {
    diagnostic_previous_phase_.store(previous, std::memory_order_relaxed);
    diagnostic_previous_phase_end_ns_.store(now_ns, std::memory_order_relaxed);
    diagnostic_phase_start_ns_.store(now_ns, std::memory_order_release);
    diagnostic_phase_.store(next, std::memory_order_release);
  }
}

CoreThread::DiagnosticSnapshot CoreThread::diagnostic_snapshot() const noexcept {
  DiagnosticSnapshot out;
  const uint64_t now_ns = diagnostic_now_ns_();
  out.phase = static_cast<DiagnosticPhase>(diagnostic_phase_.load(std::memory_order_acquire));
  out.phase_start_ns = diagnostic_phase_start_ns_.load(std::memory_order_acquire);
  out.phase_age_us = (out.phase_start_ns != 0 && now_ns >= out.phase_start_ns)
      ? ((now_ns - out.phase_start_ns) / 1000ull)
      : 0;
  out.previous_phase = static_cast<DiagnosticPhase>(diagnostic_previous_phase_.load(std::memory_order_acquire));
  out.previous_phase_end_ns = diagnostic_previous_phase_end_ns_.load(std::memory_order_acquire);
  out.previous_phase_ended_before_us = (out.previous_phase_end_ns != 0 && now_ns >= out.previous_phase_end_ns)
      ? ((now_ns - out.previous_phase_end_ns) / 1000ull)
      : 0;
  out.essential_queue_depth = diagnostic_essential_queue_depth_.load(std::memory_order_relaxed);
  out.command_queue_depth = diagnostic_command_queue_depth_.load(std::memory_order_relaxed);
  out.ordinary_queue_depth = diagnostic_ordinary_queue_depth_.load(std::memory_order_relaxed);
  out.timer_requested = diagnostic_timer_requested_.load(std::memory_order_relaxed);
  out.timer_running = diagnostic_timer_running_.load(std::memory_order_relaxed);
  return out;
}
// END TEMPORARY CAPTURE LATENCY DIAGNOSTICS

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
    stop_when_idle_ = false;
    timer_tick_requested_ = false;
    has_deadline_ = false;
    deadline_ns_ = 0;
    essential_tasks_.clear();
    command_tasks_.clear();
    tasks_.clear();
    diagnostic_update_depths_(0, 0, 0);
    diagnostic_timer_requested_.store(false, std::memory_order_relaxed);
    diagnostic_timer_running_.store(false, std::memory_order_relaxed);
    diagnostic_phase_.store(static_cast<uint8_t>(DiagnosticPhase::Unknown), std::memory_order_relaxed);
    diagnostic_phase_start_ns_.store(diagnostic_now_ns_(), std::memory_order_relaxed);
    diagnostic_previous_phase_.store(static_cast<uint8_t>(DiagnosticPhase::Unknown), std::memory_order_relaxed);
    diagnostic_previous_phase_end_ns_.store(0, std::memory_order_relaxed);
  }

  // Reset accounting
  tasks_enqueued_.store(0, std::memory_order_relaxed);
  essential_tasks_enqueued_.store(0, std::memory_order_relaxed);
  command_tasks_enqueued_.store(0, std::memory_order_relaxed);
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
  request_stop();
  join();
}

void CoreThread::request_stop() noexcept {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_requested_ = true;
  }
  cv_.notify_one();
}

void CoreThread::request_stop_from_core() noexcept {
  // Called on the core thread: still take the mutex to keep stop_requested_ coherent
  // with try_post()'s admission check.
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_requested_ = true;
  }
  cv_.notify_one();
}

void CoreThread::request_stop_when_idle_from_core() noexcept {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_when_idle_ = true;
  }
  cv_.notify_one();
}

void CoreThread::join() {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }
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

    if (bounded_core_thread_work_full(command_tasks_.size(), tasks_.size())) {
      tasks_dropped_full_.fetch_add(1, std::memory_order_relaxed);
      return PostResult::QueueFull;
    }

    try {
      tasks_.push_back(std::move(task));
      diagnostic_update_depths_(essential_tasks_.size(), command_tasks_.size(), tasks_.size());
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


CoreThread::PostResult CoreThread::try_post_command(Task task) {
  // Command ingress point. This queue is bounded together with ordinary work,
  // but drains before ordinary provider/frame work so public Core commands get
  // a prompt service opportunity under sustained provider production.
  if (!task) {
    return PostResult::Enqueued;
  }

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

    if (bounded_core_thread_work_full(command_tasks_.size(), tasks_.size())) {
      tasks_dropped_full_.fetch_add(1, std::memory_order_relaxed);
      return PostResult::QueueFull;
    }

    try {
      command_tasks_.push_back(std::move(task));
      diagnostic_update_depths_(essential_tasks_.size(), command_tasks_.size(), tasks_.size());
    } catch (...) {
      tasks_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      return PostResult::AllocFail;
    }
  }

  tasks_enqueued_.fetch_add(1, std::memory_order_relaxed);
  command_tasks_enqueued_.fetch_add(1, std::memory_order_relaxed);
  cv_.notify_one();
  return PostResult::Enqueued;
}


CoreThread::PostResult CoreThread::try_post_essential(Task task) {
  // Essential ingress point.
  // This queue is intentionally separate from the ordinary bounded queue so
  // lifecycle/native/error/capture-terminal facts are not dropped only because
  // ordinary frame/request work is saturated.
  if (!task) {
    return PostResult::Enqueued;
  }

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

    try {
      essential_tasks_.push_back(std::move(task));
      diagnostic_update_depths_(essential_tasks_.size(), command_tasks_.size(), tasks_.size());
    } catch (...) {
      tasks_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      return PostResult::AllocFail;
    }
  }

  tasks_enqueued_.fetch_add(1, std::memory_order_relaxed);
  essential_tasks_enqueued_.fetch_add(1, std::memory_order_relaxed);
  cv_.notify_one();
  return PostResult::Enqueued;
}

CoreThread::Stats CoreThread::stats_copy() const noexcept {
  Stats s;
  s.tasks_enqueued = tasks_enqueued_.load(std::memory_order_relaxed);
  s.essential_tasks_enqueued = essential_tasks_enqueued_.load(std::memory_order_relaxed);
  s.command_tasks_enqueued = command_tasks_enqueued_.load(std::memory_order_relaxed);
  s.tasks_dropped_full = tasks_dropped_full_.load(std::memory_order_relaxed);
  s.tasks_dropped_closed = tasks_dropped_closed_.load(std::memory_order_relaxed);
  s.tasks_dropped_allocfail = tasks_dropped_allocfail_.load(std::memory_order_relaxed);
  return s;
}

bool CoreThread::has_pending_command_tasks() const {
  std::lock_guard<std::mutex> lock(mu_);
  return !command_tasks_.empty();
}

void CoreThread::request_timer_tick() {
  // Forces an immediate wake and hook tick on the core thread.
  {
    std::lock_guard<std::mutex> lock(mu_);
    timer_tick_requested_ = true;
    diagnostic_timer_requested_.store(true, std::memory_order_relaxed);
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

void CoreThread::drain_tasks_locked(std::deque<Task>& essential_local,
                                     std::deque<Task>& command_local,
                                     std::deque<Task>& ordinary_local) {
  // Moves all pending tasks into local queues.
  // Guarantees:
  // - Tasks execute outside the mutex.
  // - FIFO order is preserved within each queue.
  // - Essential tasks are made available before command tasks.
  // - Command tasks are made available before ordinary tasks.
  // - Ordinary work is drained in a bounded FIFO slice. Remaining ordinary work
  //   stays queued for the next pump so newly posted command work can be observed
  //   before another ordinary slice runs.
  essential_local.clear();
  command_local.clear();
  ordinary_local.clear();
  essential_local.swap(essential_tasks_);
  command_local.swap(command_tasks_);
  const size_t ordinary_to_drain = std::min(tasks_.size(), kMaxOrdinaryTasksPerCoreThreadTurn);
  for (size_t i = 0; i < ordinary_to_drain; ++i) {
    ordinary_local.push_back(std::move(tasks_.front()));
    tasks_.pop_front();
  }
  diagnostic_update_depths_(essential_local.size() + essential_tasks_.size(),
                            command_local.size() + command_tasks_.size(),
                            ordinary_local.size() + tasks_.size());
}

void CoreThread::thread_main() {
  core_tid_ = std::this_thread::get_id();
  // From this point onward, execution is exclusively on the core thread.
  // No other thread may mutate core state.

  if (hooks_) {
    diagnostic_set_phase_from_core(DiagnosticPhase::EssentialLane);
    hooks_->on_core_start();
  }

  std::deque<Task> essential_local;
  std::deque<Task> command_local;
  std::deque<Task> ordinary_local;
  bool timer_tick_deferred_for_command = false;

  for (;;) {
    bool do_timer_tick = false;
    bool stopping = false;

    {
      std::unique_lock<std::mutex> lock(mu_);

      const bool has_deadline = has_deadline_;
      const uint64_t deadline_ns = deadline_ns_;

      auto predicate = [&]() {
        return stop_requested_ || stop_when_idle_ || !essential_tasks_.empty() ||
               !command_tasks_.empty() || !tasks_.empty() || timer_tick_requested_;
      };

      diagnostic_set_phase_from_core(DiagnosticPhase::IdleWaiting);

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

      // If a stop-when-idle request is pending and there is no work to drain,
      // convert it into a definitive stop. This closes task admission deterministically.
      if (stop_when_idle_ && essential_tasks_.empty() && command_tasks_.empty() &&
          tasks_.empty() && !timer_tick_requested_) {
        stop_requested_ = true;
      }

      stopping = stop_requested_;

      if (timer_tick_requested_) {
        do_timer_tick = true;
        timer_tick_requested_ = false;
        diagnostic_timer_requested_.store(false, std::memory_order_relaxed);
      }

      // Drain tasks while holding mutex, execute outside.
      drain_tasks_locked(essential_local, command_local, ordinary_local);
    }

    // Execute all tasks serially.
    // Determinism guarantee:
    // - No two tasks execute concurrently.
    // - Essential FIFO tasks execute before command FIFO tasks drained in the same pump.
    // - Command FIFO tasks execute before ordinary FIFO tasks drained in the same pump.
    for (size_t i = 0; i < essential_local.size(); ++i) {
      diagnostic_set_phase_from_core(DiagnosticPhase::EssentialLane);
      diagnostic_update_depths_(essential_local.size() - i, command_local.size(), ordinary_local.size());
      essential_local[i]();
    }
    essential_local.clear();
    diagnostic_update_depths_(0, command_local.size(), ordinary_local.size());

    for (size_t i = 0; i < command_local.size(); ++i) {
      diagnostic_set_phase_from_core(DiagnosticPhase::CommandLane);
      diagnostic_update_depths_(0, command_local.size() - i, ordinary_local.size());
      command_local[i]();
    }
    command_local.clear();
    diagnostic_update_depths_(0, 0, ordinary_local.size());

    for (size_t i = 0; i < ordinary_local.size(); ++i) {
      diagnostic_set_phase_from_core(DiagnosticPhase::OrdinaryLane);
      diagnostic_update_depths_(0, 0, ordinary_local.size() - i);
      ordinary_local[i]();
    }
    ordinary_local.clear();
    diagnostic_update_depths_(0, 0, 0);

    if (do_timer_tick && hooks_) {
      bool defer_timer_for_command = false;
      if (!stopping && !timer_tick_deferred_for_command) {
        std::lock_guard<std::mutex> lock(mu_);
        defer_timer_for_command = !command_tasks_.empty();
        if (defer_timer_for_command) {
          // Preserve the coalesced tick and give command-lane work posted while
          // this pump was executing a prompt service turn before timer work.
          timer_tick_requested_ = true;
          diagnostic_timer_requested_.store(true, std::memory_order_relaxed);
        }
      }

      if (defer_timer_for_command) {
        timer_tick_deferred_for_command = true;
      } else {
        // Timer tick runs strictly on the core thread.
        diagnostic_timer_running_.store(true, std::memory_order_relaxed);
        diagnostic_set_phase_from_core(DiagnosticPhase::TimerHook);
        hooks_->on_core_timer_tick();
        diagnostic_timer_running_.store(false, std::memory_order_relaxed);
        timer_tick_deferred_for_command = false;
      }
    }

    if (stopping) {
      // A drained task may request a timer tick while stop is already pending
      // (for example, a provider ingress task accepted before closure can enqueue
      // CoreRuntime facts and ask the hook to pump them). Do not strand that
      // accepted work solely because the stop flag was observed before the task ran.
      bool has_deferred_work = false;
      {
        std::lock_guard<std::mutex> lock(mu_);
        has_deferred_work = (!do_timer_tick && timer_tick_requested_) ||
                            !essential_tasks_.empty() || !command_tasks_.empty() || !tasks_.empty();
      }
      if (!has_deferred_work) {
        break;
      }
    }
  }

  if (hooks_) {
    diagnostic_set_phase_from_core(DiagnosticPhase::ShutdownStopChoreography);
    hooks_->on_core_stop();
  }
}

} // namespace cambang
