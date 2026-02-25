// src/core/latest_frame_mailbox.cpp

#include "core/latest_frame_mailbox.h"

#include <cstring>

namespace cambang {

namespace {

inline bool validate_frame_minimal(const FrameView& f, size_t row_bytes, size_t src_stride) {
  if (f.width == 0 || f.height == 0) {
    return false;
  }
  if (!f.data || f.size_bytes == 0) {
    return false;
  }
  // Minimum size check for a strided 2D buffer:
  // last row start + row_bytes must be within size_bytes.
  const size_t h = static_cast<size_t>(f.height);
  const size_t needed = (h == 0) ? 0 : ((h - 1u) * src_stride + row_bytes);
  return f.size_bytes >= needed;
}

} // namespace

void LatestFrameMailbox::write_from_core(FrameView frame) {
  // IMPORTANT: This mailbox is intentionally conservative.
  //
  // It exists to provide a stable, low-friction debug/display path.
  // It only publishes tightly packed RGBA8 frames for Godot.
  //
  // Unsupported formats (e.g. NV12/YUY2/RAW) are dropped *without conversion*
  // to avoid accidentally baking in CPU-side overhead as the default path.
  // Those formats should be handled by alternative sinks later.

  // Always release provider payload on exit.
  struct ReleaseOnExit {
    FrameView f;
    ~ReleaseOnExit() { f.release_now(); }
  } release_on_exit{frame};

  const bool is_rgba = (frame.format_fourcc == FOURCC_RGBA);
  const bool is_bgra = (frame.format_fourcc == FOURCC_BGRA);
  const bool supported = is_rgba || is_bgra;
  const size_t row_bytes = static_cast<size_t>(frame.width) * 4u;
  const size_t src_stride = (frame.stride_bytes == 0)
                                ? row_bytes
                                : static_cast<size_t>(frame.stride_bytes);

  uint64_t new_seq = 0;
  {
    std::lock_guard<std::mutex> lock(m_);
    stats_.frames_received += 1;

    if (!supported) {
      stats_.frames_dropped_unsupported += 1;
      return;
    }
    if (!validate_frame_minimal(frame, row_bytes, src_stride)) {
      stats_.frames_dropped_invalid += 1;
      return;
    }

    // Advance seq inside the lock.
    latest_.seq += 1;
    new_seq = latest_.seq;

    // Correlation + metadata.
    latest_.device_instance_id = frame.device_instance_id;
    latest_.stream_id = frame.stream_id;
    latest_.capture_id = frame.capture_id;

    latest_.width = frame.width;
    latest_.height = frame.height;
    latest_.timestamp_ns = frame.timestamp_ns;

    // Normalize to tightly packed RGBA8.
    // Dev-only exception: if provider supplies BGRA8, perform a channel swizzle
    // (B↔R) only. This is *not* a colorspace conversion and does not imply
    // future YUV/RAW CPU conversions belong here.
    latest_.pixels.resize(row_bytes * static_cast<size_t>(frame.height));
    const uint8_t* src = frame.data;
    uint8_t* dst = latest_.pixels.data();

    if (is_rgba) {
      stats_.accepted_rgba += 1;
      for (uint32_t y = 0; y < frame.height; ++y) {
        std::memcpy(dst, src, row_bytes);
        src += src_stride;
        dst += row_bytes;
      }
    } else { // BGRA
      stats_.accepted_bgra_swizzled += 1;
      for (uint32_t y = 0; y < frame.height; ++y) {
        // Swizzle per pixel: BGRA -> RGBA
        const uint8_t* s = src;
        uint8_t* d = dst;
        for (uint32_t x = 0; x < frame.width; ++x) {
          const uint8_t b = s[0];
          const uint8_t g = s[1];
          const uint8_t r = s[2];
          const uint8_t a = s[3];
          d[0] = r;
          d[1] = g;
          d[2] = b;
          d[3] = 255; // MF RGB32/BGRA alpha is often undefined/0; force opaque for dev preview.
          s += 4;
          d += 4;
        }
        src += src_stride;
        dst += row_bytes;
      }
    }

    stats_.frames_published += 1;
  }

  // Publish after releasing lock so readers can fast-path check.
  published_seq_.store(new_seq, std::memory_order_release);
}

bool LatestFrameMailbox::try_copy_if_new(uint64_t last_seq, RgbaFrame& out) const {
  // Cheap fast-path: skip lock if unchanged.
  const uint64_t pub = published_seq_.load(std::memory_order_acquire);
  if (pub == 0 || pub == last_seq) {
    return false;
  }

  std::lock_guard<std::mutex> lock(m_);
  if (latest_.seq == 0 || latest_.seq == last_seq) {
    return false;
  }

  // Never surface an empty/invalid frame to Godot.
  if (latest_.width == 0 || latest_.height == 0 || latest_.pixels.empty()) {
    return false;
  }
  const size_t expected = static_cast<size_t>(latest_.width) *
                          static_cast<size_t>(latest_.height) * 4u;
  if (latest_.pixels.size() != expected) {
    return false;
  }

  out = latest_; // copies vector
  return true;
}

LatestFrameMailbox::Stats LatestFrameMailbox::get_stats() const {
  std::lock_guard<std::mutex> lock(m_);
  return stats_;
}

} // namespace cambang
