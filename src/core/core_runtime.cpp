// src/core/core_runtime.cpp

#include "core/core_runtime.h"

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <memory>

#include <utility>

namespace cambang {

static bool banners_enabled() noexcept {
  const char* v = std::getenv("CAMBANG_BANNERS");
  // Spec: CAMBANG_BANNERS=0 disables banners.
  return !(v && v[0] == '0' && v[1] == '\0');
}

CoreRuntime::CoreRuntime()
    : core_thread_(),
      devices_(),
      streams_(),
      gen_counter_(0),
      current_gen_(0),
      version_(0),
      topology_version_(0),
      last_topology_sig_(0),
      dispatcher_(&streams_, &devices_, &native_objects_, &current_gen_, [this]() -> uint64_t {
        const auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_).count());
      }),
      ingress_(&core_thread_, [this](CoreCommand&& cmd) {
        // This lambda is executed ONLY on the core thread (posted by ingress).
        // Provider callbacks are "facts"; we enqueue them and process them before requests
        // on each core pump tick.
        assert(core_thread_.is_core_thread());
        enqueue_provider_fact(std::move(cmd));
      }, [this]() -> uint64_t {
        // Core monotonic timebase: ns since core epoch_ (session-relative).
        const auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_).count());
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

  // Reset per-generation snapshot bookkeeping (schema v1).
  // gen is monotonic across the app/server lifetime and increments only when a new core loop
  // is successfully started.
  const uint64_t pending_gen = gen_counter_;
  current_gen_ = pending_gen;
  version_ = 0;
  topology_version_ = 0;
  last_topology_sig_ = 0;
  has_topology_sig_ = false;
  provider_banner_printed_ = false;

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
  if (ok) {
    ++gen_counter_;
  } else {
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

  // Start "dirty": publish an initial baseline snapshot (version=0, topology_version=0)
  // via the normal coalesced publish path.
  publish_pending_.store(true, std::memory_order_release);
  core_thread_.request_timer_tick();
}

void CoreRuntime::on_core_timer_tick() {
  assert(core_thread_.is_core_thread());

  const auto now = std::chrono::steady_clock::now();
  const uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_).count());

  // Banner 2: Core-loop provider attachment (latched, effective).
  // Printed once per CoreRuntime session, the first time Core observes a non-null provider.
  if (!provider_banner_printed_ && banners_enabled()) {
    if (ICameraProvider* prov = provider_.load(std::memory_order_acquire)) {
#if defined(CAMBANG_INTERNAL_SMOKE)
      // Smoke can be stress-loop spammy: print only once per *process*.
      static std::atomic<bool> smoke_printed_process{false};
      if (smoke_printed_process.exchange(true)) {
        provider_banner_printed_ = true;
      } else {
      const int n = std::snprintf(core_banner_line_, sizeof(core_banner_line_),
                                  "[CamBANG][Core] provider attached (latched): %s / %s",
                                  "platform_backed", prov->provider_name());
      (void)n;
      std::fprintf(stdout, "%s\n", core_banner_line_);
      std::fflush(stdout);
      core_banner_line_pending_.store(true, std::memory_order_release);
      provider_banner_printed_ = true;
      }
#else
      const ProviderBannerInfo bi = describe_provider_for_banner(prov);
      const int n = std::snprintf(core_banner_line_, sizeof(core_banner_line_),
                                  "[CamBANG][Core] provider attached (latched): %s / %s",
                                  bi.provider_mode, bi.provider_name);
      (void)n;
      std::fprintf(stdout, "%s\n", core_banner_line_);
      std::fflush(stdout);
      core_banner_line_pending_.store(true, std::memory_order_release);
#endif
      provider_banner_printed_ = true;
    }
  }


  // 1) Drain provider facts ("what happened") first.
  while (!provider_facts_.empty()) {
    CoreCommand cmd = std::move(provider_facts_.front());
    provider_facts_.pop_front();
    dispatcher_.dispatch(std::move(cmd));
  }



// If provider facts mutated relevant state, request a coalesced publish.
if (dispatcher_.consume_relevant_state_changed()) {
  request_publish_from_core_unchecked();
}

  // 2) Drain queued requests ("what should we do").
  while (!requests_.empty()) {
    auto task = std::move(requests_.front());
    requests_.pop_front();
    if (task) {
      task();
    }
  }

  // 3) Retention / timers.
  const size_t retired_count =
      native_objects_.retire_destroyed_older_than(now_ns, kDestroyedNativeObjectRetentionWindowNs);
  if (retired_count > 0) {
    request_publish_from_core_unchecked();
  }

  if (const auto next_retirement_delay_ns =
          native_objects_.next_retirement_delay_ns(now_ns, kDestroyedNativeObjectRetentionWindowNs);
      next_retirement_delay_ns.has_value()) {
    core_thread_.set_timer_deadline_ns(*next_retirement_delay_ns);
  } else {
    core_thread_.clear_timer_deadline();
  }

  // 4) Snapshot publish (coalesced).
  if (publish_pending_.load(std::memory_order_acquire)) {
    // Clear pending first so a new request can enqueue even if publish work is heavy.
    publish_pending_.store(false, std::memory_order_release);

    SnapshotBuilder::Inputs in;
    in.devices = &devices_;
    in.streams = &streams_;
    in.ingress = &ingress_;
    in.native_objects = &native_objects_;

    const uint64_t topo_sig = snapshot_builder_.compute_topology_signature(in);

    // Publish-side topology signature for boundary diffing (Godot-facing).
    // This is updated on every successful snapshot build/publish.
    published_topology_sig_.store(topo_sig, std::memory_order_release);

    // topology_version is zero-indexed within each gen.
    // The first published snapshot establishes the baseline topology_version=0.
    if (!has_topology_sig_) {
      has_topology_sig_ = true;
      last_topology_sig_ = topo_sig;
    } else if (topo_sig != last_topology_sig_) {
      last_topology_sig_ = topo_sig;
      ++topology_version_;
    }

    const uint64_t gen_out = current_gen_;
    const uint64_t ver_out = version_;
    const uint64_t topo_out = topology_version_;
    const uint64_t timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_).count());

    CamBANGStateSnapshot snap = snapshot_builder_.build(in, gen_out, ver_out, topo_out, timestamp_ns);
    auto shared = std::make_shared<CamBANGStateSnapshot>(std::move(snap));

    // Advance per-generation publish counter only after snapshot assembly succeeds.
    ++version_;

    // Core-side publish marker used by the Godot-facing tick bridge to detect
    // "changed since last tick" in O(1).
    published_seq_.fetch_add(1, std::memory_order_acq_rel);

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
        (void)native_objects_.retire_destroyed_older_than(now_ns, kDestroyedNativeObjectRetentionWindowNs);
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

TryCreateStreamStatus CoreRuntime::try_create_stream(
    uint64_t stream_id,
    uint64_t device_instance_id,
    StreamIntent intent,
    const CaptureProfile* request_profile,
    const PictureConfig* request_picture,
    uint64_t profile_version) noexcept {
  if (stream_id == 0 || device_instance_id == 0 || profile_version == 0) {
    return TryCreateStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryCreateStreamStatus::Busy;
  }

  // Compute effective config (core owns defaulting).
  const StreamTemplate tmpl = prov->stream_template();

  StreamRequest effective{};
  effective.stream_id = stream_id;
  effective.device_instance_id = device_instance_id;
  effective.intent = intent;
  effective.profile_version = profile_version;
  effective.profile = request_profile ? *request_profile : tmpl.profile;
  effective.picture = request_picture ? *request_picture : tmpl.picture;

  const CoreThread::PostResult pr = try_post([this, effective]() {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) return;

    // Declare before calling into the provider so any synchronous callbacks
    // can resolve the record deterministically.
    (void)streams_.declare_stream_effective(effective);

    const ProviderResult r = p->create_stream(effective);
    if (!r.ok()) {
      // Best-effort rollback; create_stream failure must not leave a ghost record.
      (void)streams_.forget_stream(effective.stream_id);
    }
  });

  return (pr == CoreThread::PostResult::Enqueued) ? TryCreateStreamStatus::OK
                                                  : TryCreateStreamStatus::Busy;
}

TryStartStreamStatus CoreRuntime::try_start_stream(uint64_t stream_id) noexcept {
  if (stream_id == 0) {
    return TryStartStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryStartStreamStatus::Busy;
  }

  // NOTE: CoreStreamRegistry is core-thread-only. Do not read it on the caller thread.
  // The start is dispatched onto the core thread where the record is resolved.
  const CoreThread::PostResult pr = try_post([this, stream_id]() {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) return;

    const CoreStreamRegistry::StreamRecord* rec = streams_.find(stream_id);
    if (!rec) {
      // Stream record not declared (yet) or already destroyed.
      // Non-blocking API: caller retries as needed.
      return;
    }

    (void)p->start_stream(stream_id, rec->profile, rec->picture);
    (void)streams_.on_stream_started(stream_id);
  });

  return (pr == CoreThread::PostResult::Enqueued) ? TryStartStreamStatus::OK
                                                  : TryStartStreamStatus::Busy;
}

TryStopStreamStatus CoreRuntime::try_stop_stream(uint64_t stream_id) noexcept {
  if (stream_id == 0) {
    return TryStopStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryStopStreamStatus::Busy;
  }

  const CoreThread::PostResult pr = try_post([this, stream_id]() {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) return;
    (void)p->stop_stream(stream_id);
    (void)streams_.on_stream_stopped(stream_id, /*error_code=*/0);
  });

  return (pr == CoreThread::PostResult::Enqueued) ? TryStopStreamStatus::OK
                                                  : TryStopStreamStatus::Busy;
}

TryDestroyStreamStatus CoreRuntime::try_destroy_stream(uint64_t stream_id) noexcept {
  if (stream_id == 0) {
    return TryDestroyStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryDestroyStreamStatus::Busy;
  }

  const CoreThread::PostResult pr = try_post([this, stream_id]() {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) return;

    // Best-effort: stop before destroy.
    (void)p->stop_stream(stream_id);
    const ProviderResult dr = p->destroy_stream(stream_id);
    if (dr.ok()) {
      (void)streams_.on_stream_destroyed(stream_id);
      // Ensure core does not retain a ghost record.
      (void)streams_.forget_stream(stream_id);
    }
  });

  return (pr == CoreThread::PostResult::Enqueued) ? TryDestroyStreamStatus::OK
                                                  : TryDestroyStreamStatus::Busy;
}

TrySetStreamPictureStatus CoreRuntime::try_set_stream_picture_config(
    uint64_t stream_id,
    const PictureConfig& picture) noexcept {
  if (stream_id == 0) {
    return TrySetStreamPictureStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TrySetStreamPictureStatus::Busy;
  }
  if (!prov->supports_stream_picture_updates()) {
    return TrySetStreamPictureStatus::NotSupported;
  }

  const CoreThread::PostResult pr = try_post([this, stream_id, picture]() {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) return;
    (void)p->set_stream_picture_config(stream_id, picture);
    (void)streams_.set_picture(stream_id, picture);
  });

  return (pr == CoreThread::PostResult::Enqueued) ? TrySetStreamPictureStatus::OK
                                                  : TrySetStreamPictureStatus::Busy;
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
