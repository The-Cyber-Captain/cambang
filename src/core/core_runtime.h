// src/core/core_runtime.h
#pragma once

#include <atomic>
#include <deque>

#include "core/core_dispatcher.h"
#include "core/core_device_registry.h"
#include "core/core_publisher_buffer.h"
#include "core/core_runtime_state.h"
#include "core/core_snapshot.h"
#include "core/core_stream_registry.h"
#include "core/core_thread.h"
#include "core/latest_frame_mailbox.h"
#include "core/core_frame_sink.h"
#include "core/provider_callback_ingress.h"

#include "provider/icamera_provider.h"

namespace cambang {

// CoreRuntime is the owner of core execution components.
//
// It keeps CoreThread as a "dumb executor" and centralizes ownership of the
// dispatcher and ingress wiring at a higher layer.
//
// For this build slice it remains intentionally tiny: thread + dispatcher + ingress + snapshot/publisher.
class CoreRuntime final : private CoreThread::IHooks {
public:
  struct Stats {
    uint64_t publish_requests_coalesced = 0;
    uint64_t publish_requests_dropped_full = 0;
    uint64_t publish_requests_dropped_closed = 0;
    uint64_t publish_requests_dropped_allocfail = 0;
  };

  CoreRuntime();
  ~CoreRuntime();

  CoreRuntime(const CoreRuntime&) = delete;
  CoreRuntime& operator=(const CoreRuntime&) = delete;

  // Start/stop core thread.
  bool start();
  void stop();

  bool is_running() const { return core_thread_.is_running(); }

  CoreRuntimeState state_copy() const noexcept {
    return state_.load(std::memory_order_acquire);
  }

  // Post work onto the core thread.
  // Best-effort; drops on overflow (accounted in CoreThread::stats_copy()).
  //
  // IMPORTANT:
  // Do not block waiting for posted work to run unless you have confirmed
  // enqueue success (prefer try_post) and have a bounded fallback path.
  void post(CoreThread::Task task);

  // Best-effort post with explicit result.
  // Only LIVE accepts work; otherwise returns Closed (accounted).
  CoreThread::PostResult try_post(CoreThread::Task task);

  // ---------------------------------------------------------------------------
  // Core smoke-only hooks
  //
  // These exist solely to create deterministic queue pressure in the temporary
  // `src/smoke/core_spine_smoke.cpp` harness.
  //
  // IMPORTANT:
  // - Posting is best-effort; callers MUST check PostResult.
  // - Do not rely on these hooks in production/runtime code.
  // ---------------------------------------------------------------------------
#if defined(CAMBANG_INTERNAL_SMOKE)
  // TEMP: post directly onto the CoreThread queue, bypassing CoreRuntime request
  // gating and the requests_ pump.
  CoreThread::PostResult try_post_core_thread_unchecked(CoreThread::Task task) {
    return core_thread_.try_post(std::move(task));
  }
#endif

  // Request snapshot publication.
  //
  // Safe to call from any thread; publication work is executed on the core thread.
  // Best-effort: may be dropped if the core mailbox is full (accounted).
  void request_publish();

  Stats stats_copy() const noexcept;

  // Provider ingress accounting (copy-out; safe from any thread).
  ProviderCallbackIngress::Stats ingress_stats_copy() const noexcept { return ingress_.stats_copy(); }

  struct ShutdownDiag {
    uint8_t phase_code = 0;
    uint64_t phase_changes = 0;
  };

  ShutdownDiag shutdown_diag_copy() const noexcept;

  // Publisher access (primarily for scaffolding tests).
  // Publication itself always occurs on the core thread.
  CorePublisherBuffer& publisher() noexcept { return publisher_; }
  const CorePublisherBuffer& publisher() const noexcept { return publisher_; }

  // Dispatcher accounting (core-thread-only source of truth).
  //
  // NOTE: This returns a snapshot copy. Call only when you can guarantee
  // core-thread affinity (e.g., from a task posted to the core thread).
  [[nodiscard]] CoreDispatchStats dispatcher_stats() const noexcept { return dispatcher_.stats(); }

  // Stream registry introspection (core-thread-only).
  const CoreStreamRegistry::StreamRecord* stream_record(uint64_t stream_id) const noexcept {
    return streams_.find(stream_id);
  }

  // Provider callback ingress (transport boundary).
  IProviderCallbacks* provider_callbacks() { return &ingress_; }

  // Dev-only: latest-frame mailbox read access.
  // This remains core-owned state; Godot must treat it as read-only.
  const LatestFrameMailbox& latest_frame_mailbox() const noexcept { return latest_frame_mailbox_; }

  // Optional: attach a provider instance so CoreRuntime can perform deterministic
  // shutdown choreography (stop streams, tear down devices, provider shutdown).
  // Non-owning; caller must ensure the provider outlives CoreRuntime::stop().
  void attach_provider(ICameraProvider* provider) noexcept {
    provider_.store(provider, std::memory_order_release);
  }

private:
  // CoreThread::IHooks
  void on_core_start() override;
  void on_core_timer_tick() override;
  void on_core_stop() override;

  // Core-thread-only enqueue helpers.
  void enqueue_provider_fact(CoreCommand&& cmd);
  void enqueue_request(CoreThread::Task task);
  void request_publish_from_core_unchecked();

private:
  CoreThread core_thread_;
  CoreDeviceRegistry devices_;
  CoreStreamRegistry streams_;
  std::uint64_t snapshot_seq_ = 0;

  std::atomic<CoreRuntimeState> state_{CoreRuntimeState::CREATED};

  CorePublisherBuffer publisher_;

  LatestFrameMailbox latest_frame_mailbox_;
  class LatestFrameMailboxSink final : public ICoreFrameSink {
  public:
    explicit LatestFrameMailboxSink(LatestFrameMailbox* mb) : mb_(mb) {}
    void on_frame(FrameView frame) override {
      if (mb_) {
        mb_->write_from_core(std::move(frame));
      } else {
        frame.release_now();
      }
    }
  private:
    LatestFrameMailbox* mb_ = nullptr;
  };
  LatestFrameMailboxSink latest_frame_sink_{&latest_frame_mailbox_};

  CoreDispatcher dispatcher_;
  ProviderCallbackIngress ingress_;

  // Core pump queues (core-thread-only).
  std::deque<CoreCommand> provider_facts_;
  std::deque<CoreThread::Task> requests_;

  // Shutdown orchestration (core-thread-only).
  enum class ShutdownPhase : uint8_t {
    NONE = 0,
    STOP_STREAMS,
    AWAIT_STREAMS_STOPPED,
    DESTROY_STREAMS,
    AWAIT_STREAMS_DESTROYED,
    CLOSE_DEVICES,
    AWAIT_DEVICES_CLOSED,
    PROVIDER_SHUTDOWN,
    FINAL_RETENTION_SWEEP,
    FINAL_PUBLISH,
    EXIT
  };

  bool shutdown_requested_ = false;
  ShutdownPhase shutdown_phase_ = ShutdownPhase::NONE;
  // Copy-out diagnostics for tests (no mutable references exposed).
  std::atomic<uint8_t> shutdown_phase_code_{0};
  std::atomic<uint64_t> shutdown_phase_changes_{0};
  bool shutdown_final_publish_requested_ = false;
  uint32_t shutdown_wait_ticks_ = 0;

  // Optional provider for deterministic shutdown.
  std::atomic<ICameraProvider*> provider_{nullptr};

  // Publish coalescing flag (any-thread).
  std::atomic<bool> publish_pending_{false};

  // Stats
  std::atomic<uint64_t> publish_requests_coalesced_{0};
  std::atomic<uint64_t> publish_requests_dropped_full_{0};
  std::atomic<uint64_t> publish_requests_dropped_closed_{0};
  std::atomic<uint64_t> publish_requests_dropped_allocfail_{0};
};

} // namespace cambang
