// src/core/latest_frame_mailbox.h
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "core/rgba_frame.h"
#include "provider/provider_contract_datatypes.h"

namespace cambang {

// LatestFrameMailbox
//
// This is a *dev-visible* sink for the FrameView pipeline.
//
// It normalizes supported provider frames into a display-friendly format
// (tightly packed RGBA8) and stores only the most recent frame.
//
// Why it exists:
// - Validates the end-to-end pipeline (provider -> core -> Godot) with strict
//   ownership and deterministic release semantics.
// - Provides a simple polling interface for Godot's main thread.
//
// What it is *not*:
// - A production/high-performance representation for camera frames.
//   YUV/RAW formats should use alternative sinks (GPU-native import, shader
//   conversion, etc.) rather than forcing CPU conversion here.
//
// Concurrency:
// - Single writer (core thread via sink)
// - Single/multiple readers (Godot main thread polling)
// - Coherent state guarded by one mutex.
// - Optional published_seq_ provides a cheap "unchanged" fast-path.
class LatestFrameMailbox final {
public:
  struct Stats {
    uint64_t frames_received = 0;
    uint64_t frames_published = 0;
    uint64_t frames_dropped_unsupported = 0;
    uint64_t frames_dropped_invalid = 0;
  };

  LatestFrameMailbox() = default;
  ~LatestFrameMailbox() = default;

  LatestFrameMailbox(const LatestFrameMailbox&) = delete;
  LatestFrameMailbox& operator=(const LatestFrameMailbox&) = delete;

  // Core-thread write.
  // Copies bytes into an owned RGBA8 buffer, then releases the provider payload
  // immediately (deterministic release semantics).
  void write_from_core(FrameView frame);

  // Read a coherent snapshot if there is a new frame since last_seq.
  // Returns true and populates out if changed.
  bool try_copy_if_new(uint64_t last_seq, RgbaFrame& out) const;

  // Snapshot dev-only stats (coherent under the same mutex).
  Stats get_stats() const;

private:
  mutable std::mutex m_;
  RgbaFrame latest_;
  Stats stats_;
  std::atomic<uint64_t> published_seq_{0};
};

} // namespace cambang
