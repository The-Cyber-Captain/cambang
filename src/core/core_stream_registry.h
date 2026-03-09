// src/core/core_stream_registry.h
#pragma once

#include <cstdint>
#include <map>

#include "imaging/api/provider_contract_datatypes.h"

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
  enum class StopOrigin : uint8_t {
    None = 0,
    User = 1,
    Provider = 2,
  };

  struct StreamRecord {
    uint64_t stream_id = 0;

    uint64_t device_instance_id = 0;
    StreamIntent intent = StreamIntent::PREVIEW;
    uint64_t profile_version = 0;

    CaptureProfile profile{};
    PictureConfig picture{};

    bool created = false;
    bool started = false;
    bool stop_requested_by_core = false;
    StopOrigin last_stop_origin = StopOrigin::None;

    uint64_t frames_received = 0;
    uint64_t frames_released = 0;

    // Reserved for later flow control semantics; currently always 0.
    uint64_t frames_dropped = 0;
    uint64_t last_frame_ts_ns = 0;
    uint32_t ingress_queue_depth = 0;

    uint32_t last_error_code = 0;
  };

  CoreStreamRegistry() = default;
  ~CoreStreamRegistry() = default;

  CoreStreamRegistry(const CoreStreamRegistry&) = delete;
  CoreStreamRegistry& operator=(const CoreStreamRegistry&) = delete;

  // Lifecycle
  // declare_stream_effective: called by core surfaces that create streams.
  // Providers must not apply implicit defaults; core stores effective config.
  bool declare_stream_effective(const StreamRequest& effective);
  bool on_stream_created(uint64_t stream_id);
  bool on_stream_destroyed(uint64_t stream_id);
  bool on_stream_started(uint64_t stream_id);
  bool on_stream_stopped(uint64_t stream_id, uint32_t error_code);
  bool mark_stop_requested_by_core(uint64_t stream_id);

  // Frame accounting (stream must exist).
  bool on_frame_received(uint64_t stream_id, uint64_t integrated_ts_ns, uint32_t ingress_queue_depth);
  bool on_frame_released(uint64_t stream_id);
  bool on_frame_dropped(uint64_t stream_id);

  // Mutable config updates (stream should exist).
  bool set_picture(uint64_t stream_id, const PictureConfig& picture);

  // Best-effort cleanup for failed creations (core-thread-only).
  bool forget_stream(uint64_t stream_id);

  // Error reporting (stream must exist).
  bool on_stream_error(uint64_t stream_id, uint32_t error_code);

  // Introspection (core-thread-only).
  const StreamRecord* find(uint64_t stream_id) const noexcept;

  // For future snapshot/publisher. Core-thread-only.
  const std::map<uint64_t, StreamRecord>& all() const noexcept { return streams_; }
  bool has_flowing_stream_for_device(uint64_t device_instance_id) const noexcept;

private:
  std::map<uint64_t, StreamRecord> streams_; // key: stream_id
};

} // namespace cambang
