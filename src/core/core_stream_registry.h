// src/core/core_stream_registry.h
#pragma once

#include <cstdint>
#include <map>

namespace cambang {

// Minimal per-stream core state registry.
//
// Purpose (this build slice):
// - Provide a deterministic place for the core to remember streams exist.
// - Accumulate per-stream counters as commands are dispatched.
// - No frame delivery semantics (always-release remains).
//
// Threading:
// - Core-thread-only. Not atomic. Determinism-first.
class CoreStreamRegistry final {
public:
  struct StreamRecord {
    uint64_t stream_id = 0;

    bool created = false;
    bool started = false;

    uint64_t frames_received = 0;
    uint64_t frames_released = 0;

    // Reserved for later flow control semantics; currently always 0.
    uint64_t frames_dropped = 0;

    uint32_t last_error_code = 0;
  };

  CoreStreamRegistry() = default;
  ~CoreStreamRegistry() = default;

  CoreStreamRegistry(const CoreStreamRegistry&) = delete;
  CoreStreamRegistry& operator=(const CoreStreamRegistry&) = delete;

  // Lifecycle
  bool on_stream_created(uint64_t stream_id);
  bool on_stream_destroyed(uint64_t stream_id);
  bool on_stream_started(uint64_t stream_id);
  bool on_stream_stopped(uint64_t stream_id, uint32_t error_code);

  // Frame accounting (stream must exist).
  bool on_frame_received(uint64_t stream_id);
  bool on_frame_released(uint64_t stream_id);

  // Error reporting (stream must exist).
  bool on_stream_error(uint64_t stream_id, uint32_t error_code);

  // Introspection (core-thread-only).
  const StreamRecord* find(uint64_t stream_id) const noexcept;

  // For future snapshot/publisher. Core-thread-only.
  const std::map<uint64_t, StreamRecord>& all() const noexcept { return streams_; }

private:
  std::map<uint64_t, StreamRecord> streams_; // key: stream_id
};

} // namespace cambang
