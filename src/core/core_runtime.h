// src/core/core_runtime.h
#pragma once

#include <atomic>

#include "core/core_dispatcher.h"
#include "core/core_publisher_buffer.h"
#include "core/core_snapshot.h"
#include "core/core_stream_registry.h"
#include "core/core_thread.h"
#include "core/provider_callback_ingress.h"

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

  // Post work onto the core thread.
  // Best-effort; drops on overflow (accounted in CoreThread::stats_copy()).
  void post(CoreThread::Task task) { core_thread_.post(std::move(task)); }

  // Request snapshot publication.
  //
  // Safe to call from any thread; publication work is executed on the core thread.
  // Best-effort: may be dropped if the core mailbox is full (accounted).
  void request_publish();

  Stats stats_copy() const noexcept;

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

private:
  // CoreThread::IHooks
  void on_core_start() override {}
  void on_core_timer_tick() override {}
  void on_core_stop() override {}

private:
  CoreThread core_thread_;
  CoreStreamRegistry streams_;
  std::uint64_t snapshot_seq_ = 0;

  CorePublisherBuffer publisher_;
  CoreDispatcher dispatcher_;
  ProviderCallbackIngress ingress_;

  // Publish coalescing flag (any-thread).
  std::atomic<bool> publish_pending_{false};

  // Stats
  std::atomic<uint64_t> publish_requests_coalesced_{0};
  std::atomic<uint64_t> publish_requests_dropped_full_{0};
  std::atomic<uint64_t> publish_requests_dropped_closed_{0};
  std::atomic<uint64_t> publish_requests_dropped_allocfail_{0};
};

} // namespace cambang
