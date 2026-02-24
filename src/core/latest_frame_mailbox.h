// src/core/latest_frame_mailbox.h
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "provider/provider_contract_datatypes.h"

namespace cambang {

// Owned CPU-side copy of the most recent frame.
//
// Coherent state is guarded by a single mutex to avoid partial-atomic hazards.
// Optional published_seq_ supports a cheap "unchanged" fast-path.
class LatestFrameMailbox final {
public:
  struct Stats {
    uint64_t frames_received = 0;
    uint64_t frames_published = 0;
    uint64_t frames_dropped_unsupported = 0;
    uint64_t frames_dropped_invalid = 0;
  };

  struct FrameCopy {
    uint64_t seq = 0;

    // Correlation
    uint64_t device_instance_id = 0;
    uint64_t stream_id = 0;
    uint64_t capture_id = 0;

    // Image metadata
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format_fourcc = 0;
    uint32_t stride_bytes = 0;

    // Timing
    uint64_t timestamp_ns = 0;

    // Packed RGBA8 bytes owned by the mailbox (tightly packed; stride == width * 4).
    std::vector<uint8_t> bytes;
  };

  LatestFrameMailbox() = default;
  ~LatestFrameMailbox() = default;

  LatestFrameMailbox(const LatestFrameMailbox&) = delete;
  LatestFrameMailbox& operator=(const LatestFrameMailbox&) = delete;

  // Core-thread write. Copies bytes, then releases provider payload immediately.
  void write_from_core(FrameView frame);

  // Read a coherent snapshot if there is a new frame since last_seq.
  // Returns true and populates out if changed.
  bool try_copy_if_new(uint64_t last_seq, FrameCopy& out) const;

  // Snapshot dev-only stats (coherent under the same mutex).
  Stats get_stats() const;

private:
  mutable std::mutex m_;
  FrameCopy latest_;
  Stats stats_;
  std::atomic<uint64_t> published_seq_{0};
};

} // namespace cambang
