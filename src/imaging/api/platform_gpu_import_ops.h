#pragma once

#include <cstdint>
#include <memory>

namespace cambang {

// Neutral seam through which a platform provider asks the host renderer for a
// frozen GPU copy of a native camera buffer's planes -- the producer side of
// FrameView::gpu_compute_sidecar for real cameras.
//
// The provider stays renderer-free: it hands over an opaque native buffer
// (AHardwareBuffer* on Android) and gets back an opaque sidecar it attaches to
// the member. The implementation lives in the Godot layer, which owns the
// GPU device, and installs this table there. The same pattern as
// SyntheticGpuBackingRuntimeOps.

struct PlatformGpuCopyRequest {
  // AHardwareBuffer* on Android. Borrowed: the provider keeps the buffer's
  // owner (its AImage) alive until on_source_released runs.
  void* native_buffer = nullptr;
  // The member's extent; must equal the buffer's.
  uint32_t width = 0;
  uint32_t height = 0;
  // The CPU payload's chroma order. True for NV21: the interleaved chroma
  // plane then carries Cr before Cb, matching the CPU plane byte for byte.
  bool chroma_v_first = false;
  // Called exactly once, on an unspecified thread, when the GPU no longer reads
  // the buffer -- after the copy has executed, or straight away if it never
  // will. Only called when begin_copy returned a job.
  void (*on_source_released)(void* user) = nullptr;
  void* user = nullptr;
};

struct PlatformGpuImportOps final {
  // Whether the active GPU device can copy this platform's native camera
  // buffers at all. Cheap after the first call; any thread.
  bool (*can_copy_native_buffers)() noexcept = nullptr;
  // Starts a copy. Returns a job handle, or null when the copy cannot start --
  // in which case on_source_released is NOT called and the caller frees the
  // source itself. Called on the provider's image thread; never blocks on the
  // render thread.
  std::shared_ptr<void> (*begin_copy)(const PlatformGpuCopyRequest& request) noexcept = nullptr;
  // Waits up to timeout_ns for the job to be submitted ahead of any use, and
  // returns the sidecar, or null on failure or timeout. A job that timed out
  // is abandoned and cleaned up by the implementation; its source is still
  // released through on_source_released.
  std::shared_ptr<void> (*finish_copy)(const std::shared_ptr<void>& job,
                                       uint64_t timeout_ns) noexcept = nullptr;
};

// The table is non-owning and must stay alive until clear has returned. Clear
// closes admission first, then waits for every admitted call to return.
void set_platform_gpu_import_ops(const PlatformGpuImportOps* ops) noexcept;
void clear_platform_gpu_import_ops() noexcept;

bool platform_gpu_import_can_copy_native_buffers() noexcept;
std::shared_ptr<void> platform_gpu_import_begin_copy(const PlatformGpuCopyRequest& request) noexcept;
std::shared_ptr<void> platform_gpu_import_finish_copy(const std::shared_ptr<void>& job,
                                                      uint64_t timeout_ns) noexcept;

} // namespace cambang
