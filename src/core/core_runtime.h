// src/core/core_runtime.h
#pragma once

#include "core/core_dispatcher.h"
#include "core/core_thread.h"
#include "core/provider_callback_ingress.h"

namespace cambang {

// CoreRuntime is the owner of core execution components.
//
// It keeps CoreThread as a "dumb executor" and centralizes ownership of the
// dispatcher and ingress wiring at a higher layer.
//
// For this build slice it remains intentionally tiny: thread + dispatcher + ingress.
class CoreRuntime final : private CoreThread::IHooks {
public:
  CoreRuntime();
  ~CoreRuntime();

  CoreRuntime(const CoreRuntime&) = delete;
  CoreRuntime& operator=(const CoreRuntime&) = delete;

  // Start/stop core thread.
  bool start();
  void stop();

  bool is_running() const { return core_thread_.is_running(); }

  // Post work onto the core thread.
  void post(CoreThread::Task task) { core_thread_.post(std::move(task)); }

  // Dispatcher accounting (core-thread-only source of truth).
  //
  // NOTE: This returns a snapshot copy. Call only when you can guarantee
  // core-thread affinity (e.g., from a task posted to the core thread).
  [[nodiscard]] CoreDispatchStats dispatcher_stats() const noexcept {
    return dispatcher_.stats();
  }

  // Provider callback ingress (transport boundary).
  IProviderCallbacks* provider_callbacks() { return &ingress_; }

  // CoreThread::IHooks
private:
  void on_core_start() override {}
  void on_core_timer_tick() override {}
  void on_core_stop() override {}

private:
  CoreThread core_thread_;
  CoreDispatcher dispatcher_;
  ProviderCallbackIngress ingress_;
};

} // namespace cambang
