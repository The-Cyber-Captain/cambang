// src/core/rgba_frame.h
#pragma once

#include <cstdint>
#include <vector>

namespace cambang {

// Display-normalized frame.
//
// Intent (temporary, but durable):
// - This type represents a *tightly packed RGBA8* image intended for debug
//   visibility inside the Godot test harness.
// - It is **not** the canonical on-wire provider format and is not suitable as
//   a high-performance path for YUV/RAW cameras.
//
// Real-world use-case:
// - A dev-only sink can normalize provider frames (various strides / formats)
//   into RGBA8 so Godot can upload it to an ImageTexture on the main thread.
// - Production paths are expected to use alternative sinks (e.g. GPU-native
//   YUV import + shader conversion) without changing provider ingress or core
//   determinism.
struct RgbaFrame {
  // Monotonic sequence number assigned by the sink/mailbox.
  uint64_t seq = 0;

  // Correlation (copied from provider FrameView for debugging).
  uint64_t device_instance_id = 0;
  uint64_t stream_id = 0;
  uint64_t capture_id = 0;

  // Image metadata.
  uint32_t width = 0;
  uint32_t height = 0;

  // Capture timestamp in nanoseconds (as provided).
  uint64_t timestamp_ns = 0;

  // Tightly packed RGBA8: pixels.size() == width * height * 4.
  std::vector<uint8_t> pixels;
};

} // namespace cambang
