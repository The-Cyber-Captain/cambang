// src/core/core_dispatcher.h
#pragma once

#include <cstdint>

#include "core/core_mailbox.h"

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
};

// CoreDispatcher is the core-thread-only consumer of CoreCommand.
//
// For this build slice, there is no downstream frame consumer yet.
// Therefore, the dispatcher proves ownership correctness by ALWAYS releasing
// provider frames immediately (release-on-drop at the dispatch boundary).
class CoreDispatcher final {
public:
  CoreDispatcher() = default;
  ~CoreDispatcher() = default;

  CoreDispatcher(const CoreDispatcher&) = delete;
  CoreDispatcher& operator=(const CoreDispatcher&) = delete;

  // Must be called ONLY on the core thread.
  void dispatch(CoreCommand&& cmd);

  // Must be called ONLY on the core thread.
  [[nodiscard]] CoreDispatchStats stats() const noexcept { return stats_; }

  // Must be called ONLY on the core thread.
  void reset_stats() noexcept { stats_ = {}; }

private:
  CoreDispatchStats stats_{};
};

} // namespace cambang
