// src/core/core_dispatcher.h
#pragma once

#include <cstdint>

#include "core/core_mailbox.h"
#include "core/core_device_registry.h"
#include "core/core_native_object_registry.h"
#include "core/core_stream_registry.h"
#include "core/core_frame_sink.h"

namespace cambang {

// Lightweight core-thread-only accounting for dispatcher activity.
//
// NOTE: This is intentionally NOT atomic: it must only be accessed and mutated
// on the core thread to preserve determinism and avoid incidental contention.
struct CoreDispatchStats final {
  uint64_t commands_total = 0;
  uint64_t commands_handled = 0;
  uint64_t commands_dropped = 0;

  uint64_t frames_received = 0;
  uint64_t frames_released = 0;

  uint64_t frames_unknown_stream = 0;
};

// CoreDispatcher is the core-thread-only consumer of CoreCommand.
//
// For this build slice, there is no downstream frame consumer yet.
// Therefore, the dispatcher proves ownership correctness by ALWAYS releasing
// provider frames immediately (release-on-drop at the dispatch boundary).
class CoreDispatcher final {
public:
  CoreDispatcher(CoreStreamRegistry* streams,
               CoreDeviceRegistry* devices,
               CoreNativeObjectRegistry* native_objects)
      : streams_(streams), devices_(devices), native_objects_(native_objects) {}

  ~CoreDispatcher() = default;

  CoreDispatcher(const CoreDispatcher&) = delete;
  CoreDispatcher& operator=(const CoreDispatcher&) = delete;

  // Must be called ONLY on the core thread.
  void dispatch(CoreCommand&& cmd);

  // Must be called ONLY on the core thread.
  [[nodiscard]] CoreDispatchStats stats() const noexcept { return stats_; }

  // Must be called ONLY on the core thread.
  void reset_stats() noexcept { stats_ = {}; }

  // Returns true if any dispatched command mutated relevant state that should trigger snapshot publication.
  bool consume_relevant_state_changed() noexcept {
    const bool v = relevant_state_changed_;
    relevant_state_changed_ = false;
    return v;
  }

  // Install an optional core-thread sink for provider frames.
  // If unset, frames are released immediately (release-on-drop).
  // Must be called before the core thread starts, or from the core thread.
  void set_frame_sink(ICoreFrameSink* sink) noexcept { frame_sink_ = sink; }

private:
  CoreStreamRegistry* streams_ = nullptr; // non-owning; core-thread-only
  CoreDeviceRegistry* devices_ = nullptr; // non-owning; core-thread-only
  CoreNativeObjectRegistry* native_objects_ = nullptr; // non-owning; core-thread-only

  bool relevant_state_changed_ = false;
  ICoreFrameSink* frame_sink_ = nullptr; // non-owning; core-thread-only
  CoreDispatchStats stats_{};
};

} // namespace cambang
