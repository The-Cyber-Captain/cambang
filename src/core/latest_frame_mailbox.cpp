// src/core/latest_frame_mailbox.cpp

#include "core/latest_frame_mailbox.h"

#include <cstring>

namespace cambang {

void LatestFrameMailbox::write_from_core(FrameView frame) {
  // Copy under lock for coherent state.
  uint64_t new_seq = 0;
  {
    std::lock_guard<std::mutex> lock(m_);

    // Advance seq inside the lock.
    latest_.seq += 1;
    new_seq = latest_.seq;

    latest_.device_instance_id = frame.device_instance_id;
    latest_.stream_id = frame.stream_id;
    latest_.capture_id = frame.capture_id;

    latest_.width = frame.width;
    latest_.height = frame.height;
    latest_.format_fourcc = frame.format_fourcc;
    latest_.stride_bytes = frame.stride_bytes;

    latest_.timestamp_ns = frame.timestamp_ns;

    latest_.bytes.resize(frame.size_bytes);
    if (frame.size_bytes > 0 && frame.data) {
      std::memcpy(latest_.bytes.data(), frame.data, frame.size_bytes);
    }
  }

  // Release provider payload deterministically after copy.
  frame.release_now();
  // Publish after releasing lock so readers can fast-path check.
  published_seq_.store(new_seq, std::memory_order_release);
}

bool LatestFrameMailbox::try_copy_if_new(uint64_t last_seq, FrameCopy& out) const {
  // Cheap fast-path: skip lock if unchanged.
  const uint64_t pub = published_seq_.load(std::memory_order_acquire);
  if (pub == 0 || pub == last_seq) {
    return false;
  }

  std::lock_guard<std::mutex> lock(m_);
  if (latest_.seq == 0 || latest_.seq == last_seq) {
    return false;
  }

  out = latest_; // copies vector
  return true;
}

} // namespace cambang
