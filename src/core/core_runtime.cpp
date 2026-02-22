// src/core/core_runtime.cpp

#include "core/core_runtime.h"

#include <cassert>

#include <utility>

namespace cambang {

CoreRuntime::CoreRuntime()
    : core_thread_(),
      streams_(),
      snapshot_seq_(0),
      publisher_(),
      dispatcher_(&streams_),
      ingress_(&core_thread_, [this](CoreCommand&& cmd) {
        // This lambda is executed ONLY on the core thread (posted by ingress).
        // Provider callbacks are "facts"; we enqueue them and process them before requests
        // on each core pump tick.
        assert(core_thread_.is_core_thread());
        enqueue_provider_fact(std::move(cmd));
      }) {}

CoreRuntime::~CoreRuntime() {
  stop();
}

bool CoreRuntime::start() {
  // Idempotent start: already running is a success.
  const CoreRuntimeState st0 = state_.load(std::memory_order_acquire);
  if (st0 == CoreRuntimeState::LIVE || st0 == CoreRuntimeState::STARTING) {
    return true;
  }
  if (st0 == CoreRuntimeState::TEARING_DOWN) {
    return false;
  }

  // Attempt to transition CREATED/STOPPED -> STARTING.
  CoreRuntimeState expected = st0;
  while (expected == CoreRuntimeState::CREATED || expected == CoreRuntimeState::STOPPED) {
    if (state_.compare_exchange_weak(expected, CoreRuntimeState::STARTING,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire)) {
      break;
    }
  }
  if (state_.load(std::memory_order_acquire) != CoreRuntimeState::STARTING) {
    // Another thread moved us to LIVE/STARTING concurrently.
    const CoreRuntimeState st1 = state_.load(std::memory_order_acquire);
    return (st1 == CoreRuntimeState::LIVE || st1 == CoreRuntimeState::STARTING);
  }

  publish_pending_.store(false, std::memory_order_relaxed);
  publish_requests_coalesced_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_full_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_closed_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_allocfail_.store(0, std::memory_order_relaxed);

  // Reset core-thread-only pump state.
  provider_facts_.clear();
  requests_.clear();
  shutdown_requested_ = false;
  shutdown_final_publish_requested_ = false;

  const bool ok = core_thread_.start(this);
  if (!ok) {
    // Failed to start core thread; revert to a sensible stable state.
    state_.store(CoreRuntimeState::STOPPED, std::memory_order_release);
  }
  return ok;
}

void CoreRuntime::stop() {
  // Idempotent stop.
  const CoreRuntimeState st0 = state_.exchange(CoreRuntimeState::TEARING_DOWN, std::memory_order_acq_rel);
  if (st0 == CoreRuntimeState::STOPPED || st0 == CoreRuntimeState::CREATED) {
    state_.store(st0, std::memory_order_release);
    return;
  }

  // Deterministic shutdown request:
  // - Stop accepting new external requests immediately (state == TEARING_DOWN).
  // - Keep provider fact ingestion best-effort until the core thread closes.
  // - Core thread executes a deterministic shutdown pump and requests stop only at the end.
  if (core_thread_.is_running()) {
    const auto r = core_thread_.try_post([this]() {
      assert(core_thread_.is_core_thread());
      shutdown_requested_ = true;
      core_thread_.request_timer_tick();
    });

    if (r != CoreThread::PostResult::Enqueued) {
      // If we cannot schedule shutdown initiation (queue full / alloc fail / closed),
      // fall back to an immediate stop request.
      core_thread_.request_stop();
    }

    core_thread_.join();
  }

  state_.store(CoreRuntimeState::STOPPED, std::memory_order_release);
}


void CoreRuntime::on_core_start() {
  // Core thread has started; begin accepting new work.
  state_.store(CoreRuntimeState::LIVE, std::memory_order_release);
}

void CoreRuntime::on_core_timer_tick() {
  assert(core_thread_.is_core_thread());

  // 1) Drain provider facts ("what happened") first.
  while (!provider_facts_.empty()) {
    CoreCommand cmd = std::move(provider_facts_.front());
    provider_facts_.pop_front();
    dispatcher_.dispatch(std::move(cmd));
  }

  // 2) Drain queued requests ("what should we do").
  while (!requests_.empty()) {
    auto task = std::move(requests_.front());
    requests_.pop_front();
    if (task) {
      task();
    }
  }

  // 3) Retention / timers would run here (not yet implemented).

  // 4) Snapshot publish (coalesced).
  if (publish_pending_.load(std::memory_order_acquire)) {
    // Clear pending first so a new request can enqueue even if publish work is heavy.
    publish_pending_.store(false, std::memory_order_release);

    CoreSnapshot snap;
    snap.seq = ++snapshot_seq_;

    snap.streams.reserve(streams_.all().size());
    for (const auto& kv : streams_.all()) {
      const auto& rec = kv.second;
      CoreSnapshot::Stream s;
      s.stream_id = rec.stream_id;
      s.created = rec.created;
      s.started = rec.started;
      s.frames_received = rec.frames_received;
      s.frames_released = rec.frames_released;
      s.frames_dropped = rec.frames_dropped;
      s.last_error_code = static_cast<std::int32_t>(rec.last_error_code);
      snap.streams.push_back(s);
    }

    publisher_.publish(std::move(snap));
  }

  // 5) Shutdown branch: request a final publish, then stop only once fully drained.
  if (shutdown_requested_) {
    if (!shutdown_final_publish_requested_) {
      // Final snapshot publication before exit.
      request_publish_from_core_unchecked();
      shutdown_final_publish_requested_ = true;
      // Ensure another tick runs to perform the publish if it couldn't run in this tick.
      core_thread_.request_timer_tick();
      return;
    }

    const bool publish_still_pending = publish_pending_.load(std::memory_order_acquire);
    if (provider_facts_.empty() && requests_.empty() && !publish_still_pending) {
      // Deterministic stop point reached: no pending facts, no pending requests, final publish done.
      core_thread_.request_stop_from_core();
    }
  }
}

void CoreRuntime::on_core_stop() {
  // Core thread is exiting. Ensure external gating sees STOPPED promptly.
  state_.store(CoreRuntimeState::STOPPED, std::memory_order_release);
}

void CoreRuntime::post(CoreThread::Task task) {
  (void)try_post(std::move(task));
}

CoreThread::PostResult CoreRuntime::try_post(CoreThread::Task task) {
  const CoreRuntimeState st = state_.load(std::memory_order_acquire);
  if (st != CoreRuntimeState::LIVE) {
    return core_thread_.reject_closed();
  }

  // Marshal requests into the core pump queue; the pump enforces ordering (facts before requests).
  return core_thread_.try_post([this, t = std::move(task)]() mutable {
    assert(core_thread_.is_core_thread());
    enqueue_request(std::move(t));
  });
}

CoreRuntime::Stats CoreRuntime::stats_copy() const noexcept {
  Stats s;
  s.publish_requests_coalesced = publish_requests_coalesced_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_full = publish_requests_dropped_full_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_closed = publish_requests_dropped_closed_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_allocfail = publish_requests_dropped_allocfail_.load(std::memory_order_relaxed);
  return s;
}

void CoreRuntime::request_publish() {
  // Lifecycle gating: only LIVE accepts publish work.
  if (state_.load(std::memory_order_acquire) != CoreRuntimeState::LIVE) {
    publish_requests_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
    publish_pending_.store(false, std::memory_order_release);
    return;
  }

  // Coalesce publish requests to avoid spamming the core mailbox.
  const bool was_pending = publish_pending_.exchange(true, std::memory_order_acq_rel);
  if (was_pending) {
    publish_requests_coalesced_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Schedule a pump tick to perform publish after draining facts/requests.
  // We post directly to CoreThread (not via CoreRuntime::try_post) to avoid
  // routing this through the requests_ queue.
  const CoreThread::PostResult r = core_thread_.try_post([this]() {
    assert(core_thread_.is_core_thread());
    core_thread_.request_timer_tick();
  });

  if (r == CoreThread::PostResult::Enqueued) {
    return;
  }

  // Failed to enqueue: clear pending so callers can retry later.
  publish_pending_.store(false, std::memory_order_release);

  switch (r) {
    case CoreThread::PostResult::QueueFull:
      publish_requests_dropped_full_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Closed:
      publish_requests_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::AllocFail:
      publish_requests_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Enqueued:
      break;
  }
}

void CoreRuntime::enqueue_provider_fact(CoreCommand&& cmd) {
  assert(core_thread_.is_core_thread());
  provider_facts_.push_back(std::move(cmd));
  core_thread_.request_timer_tick();
}

void CoreRuntime::enqueue_request(CoreThread::Task task) {
  assert(core_thread_.is_core_thread());
  requests_.push_back(std::move(task));
  core_thread_.request_timer_tick();
}

void CoreRuntime::request_publish_from_core_unchecked() {
  assert(core_thread_.is_core_thread());
  // Coalesce naturally: if already pending, keep it pending.
  publish_pending_.store(true, std::memory_order_release);
}

} // namespace cambang

