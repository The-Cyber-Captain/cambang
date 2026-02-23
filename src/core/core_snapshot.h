// src/core/core_snapshot.h
#pragma once

#include <cstdint>
#include <vector>

namespace cambang {

// Immutable snapshot of core state.
//
// For now, this snapshot only includes minimal per-stream state and counters.
// It is intentionally small and is built entirely on the core thread.
struct CoreSnapshot final {
  struct Stream final {
    std::uint64_t stream_id = 0;

    bool created = false;
    bool started = false;

    std::uint64_t frames_received = 0;
    std::uint64_t frames_released = 0;
    std::uint64_t frames_dropped = 0;

    std::int32_t last_error_code = 0;
  };

  // Monotonic sequence number assigned by CoreRuntime at publication time.
  std::uint64_t seq = 0;

  // Stream state at the time of publication.
  std::vector<Stream> streams;
};

} // namespace cambang
