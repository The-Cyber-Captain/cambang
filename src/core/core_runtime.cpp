// src/core/core_runtime.cpp

#include "core/core_runtime.h"

#include <cassert>
#include <memory>

#include <utility>

namespace cambang {

CoreRuntime::CoreRuntime()
    : core_thread_(),
      devices_(),
      streams_(),
      gen_(0),
      topology_gen_(0),
      last_topology_sig_(0),
      dispatcher_(&streams_, &devices_),
      ingress_(&core_thread_, [this](CoreCommand&& cmd) {
        // This lambda is executed ONLY on the core thread (posted by ingress).
        // Provider callbacks are "facts"; we enqueue them and process them before requests
        // on each core pump tick.
        assert(core_thread_.is_core_thread());
        enqueue_provider_fact(std::move(cmd));
      }) {
#if defined(CAMBANG_ENABLE_DEV_NODES)
  // Dev-only latest-frame sink (core thread dispatch path).
  dispatcher_.set_frame_sink(&latest_frame_sink_);
#endif
}

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
  shutdown_phase_ = ShutdownPhase::NONE;
  shutdown_phase_code_.store(0, std::memory_order_relaxed);
  shutdown_phase_changes_.store(0, std::memory_order_relaxed);
  shutdown_final_publish_requested_ = false;
  shutdown_wait_ticks_ = 0;

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
  epoch_ = std::chrono::steady_clock::now();
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

    SnapshotBuilder::Inputs in;
    in.devices = &devices_;
    in.streams = &streams_;
    in.ingress = &ingress_;

    const uint64_t topo_sig = snapshot_builder_.compute_topology_signature(in);
    if (topo_sig != last_topology_sig_) {
      last_topology_sig_ = topo_sig;
      ++topology_gen_;
    }

    ++gen_;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_).count());

    CamBANGStateSnapshot snap = snapshot_builder_.build(in, gen_, topology_gen_, timestamp_ns);
    auto shared = std::make_shared<CamBANGStateSnapshot>(std::move(snap));

    if (IStateSnapshotPublisher* pub = snapshot_publisher_.load(std::memory_order_acquire)) {
      pub->publish(std::move(shared));
    }
  }

  // 5) Shutdown choreography (§10).
  if (shutdown_requested_) {
    auto set_phase = [this](ShutdownPhase p) {
      if (shutdown_phase_ != p) {
        shutdown_phase_ = p;
        shutdown_phase_code_.store(static_cast<uint8_t>(p), std::memory_order_relaxed);
        shutdown_phase_changes_.fetch_add(1, std::memory_order_relaxed);
      }
    };

    if (shutdown_phase_ == ShutdownPhase::NONE) {
      set_phase(ShutdownPhase::STOP_STREAMS);
      shutdown_wait_ticks_ = 0;
    }

    constexpr uint32_t kMaxShutdownWaitTicks = 200; // deterministic bound; avoids teardown hangs.

    ICameraProvider* prov = provider_.load(std::memory_order_acquire);

    auto any_stream_started = [this]() -> bool {
      for (const auto& kv : streams_.all()) {
        if (kv.second.started) {
          return true;
        }
      }
      return false;
    };

    auto any_streams_exist = [this]() -> bool {
      return !streams_.all().empty();
    };

    auto any_device_open = [this]() -> bool {
      for (const auto& kv : devices_.all()) {
        if (kv.second.open) {
          return true;
        }
      }
      return false;
    };

    switch (shutdown_phase_) {
      case ShutdownPhase::STOP_STREAMS: {
        // Step 3: stop streams (deterministic order).
        if (prov) {
          for (const auto& kv : streams_.all()) {
            const auto& rec = kv.second;
            if (rec.started) {
              (void)prov->stop_stream(rec.stream_id);
            }
          }
        }
        set_phase(ShutdownPhase::AWAIT_STREAMS_STOPPED);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::AWAIT_STREAMS_STOPPED: {
        // Best-effort drain: wait for provider facts to reflect stopped streams.
        if (any_stream_started() && shutdown_wait_ticks_++ < kMaxShutdownWaitTicks) {
          core_thread_.request_timer_tick();
          return;
        }
        set_phase(ShutdownPhase::DESTROY_STREAMS);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::DESTROY_STREAMS: {
        // Step 4 (part): tear down stream instances (destroy) before closing devices.
        if (prov) {
          for (const auto& kv : streams_.all()) {
            const auto& rec = kv.second;
            if (rec.created) {
              (void)prov->destroy_stream(rec.stream_id);
            }
          }
        }
        set_phase(ShutdownPhase::AWAIT_STREAMS_DESTROYED);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::AWAIT_STREAMS_DESTROYED: {
        // Best-effort: wait until provider confirms destruction.
        if (any_streams_exist() && shutdown_wait_ticks_++ < kMaxShutdownWaitTicks) {
          core_thread_.request_timer_tick();
          return;
        }
        set_phase(ShutdownPhase::CLOSE_DEVICES);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::CLOSE_DEVICES: {
        if (prov) {
          for (const auto& kv : devices_.all()) {
            const auto& rec = kv.second;
            if (rec.open) {
              (void)prov->close_device(rec.device_instance_id);
            }
          }
        }
        set_phase(ShutdownPhase::AWAIT_DEVICES_CLOSED);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::AWAIT_DEVICES_CLOSED: {
        if (any_device_open() && shutdown_wait_ticks_++ < kMaxShutdownWaitTicks) {
          core_thread_.request_timer_tick();
          return;
        }
        set_phase(ShutdownPhase::PROVIDER_SHUTDOWN);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::PROVIDER_SHUTDOWN: {
        // Step 5: request provider shutdown (idempotent).
        if (prov) {
          (void)prov->shutdown();
        }
        set_phase(ShutdownPhase::FINAL_RETENTION_SWEEP);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::FINAL_RETENTION_SWEEP: {
        // Step 6: final retention sweep (not yet implemented in this slice).
        set_phase(ShutdownPhase::FINAL_PUBLISH);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::FINAL_PUBLISH: {
        // Step 7: final snapshot publication.
        if (!shutdown_final_publish_requested_) {
          request_publish_from_core_unchecked();
          shutdown_final_publish_requested_ = true;
          core_thread_.request_timer_tick();
          return;
        }

        set_phase(ShutdownPhase::EXIT);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::EXIT: {
        // Step 8: exit when fully drained and publish completed.
        const bool publish_still_pending = publish_pending_.load(std::memory_order_acquire);
        if ((provider_facts_.empty() && requests_.empty() && !publish_still_pending) ||
            (shutdown_wait_ticks_++ >= kMaxShutdownWaitTicks)) {
          core_thread_.request_stop_from_core();
        } else {
          core_thread_.request_timer_tick();
        }
        return;
      }

      default:
        break;
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

CoreRuntime::ShutdownDiag CoreRuntime::shutdown_diag_copy() const noexcept {
  ShutdownDiag d;
  d.phase_code = shutdown_phase_code_.load(std::memory_order_relaxed);
  d.phase_changes = shutdown_phase_changes_.load(std::memory_order_relaxed);
  return d;
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

