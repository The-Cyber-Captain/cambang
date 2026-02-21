// src/provider/provider_contract_datatypes.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cambang {

// Public semantics for repeating streams.
enum class StreamIntent : uint8_t {
  PREVIEW = 0,
  VIEWFINDER = 1,
};

// Scoped, stable error categories for provider results and failure signals.
// Keep categories stable across versions; mapping to text lives elsewhere.
enum class ProviderError : uint32_t {
  OK = 0,

  ERR_NOT_SUPPORTED,
  ERR_INVALID_ARGUMENT,
  ERR_BUSY,
  ERR_BAD_STATE,
  ERR_PLATFORM_CONSTRAINT,
  ERR_TRANSIENT_FAILURE,
  ERR_PROVIDER_FAILED,
  ERR_SHUTTING_DOWN,
};

// Deterministic result for provider method calls.
struct ProviderResult {
  ProviderError code = ProviderError::OK;

  constexpr bool ok() const { return code == ProviderError::OK; }

  static constexpr ProviderResult success() { return ProviderResult{ProviderError::OK}; }
  static constexpr ProviderResult failure(ProviderError c) { return ProviderResult{c}; }
};

// Hardware endpoint as reported by provider enumeration.
struct CameraEndpoint {
  std::string hardware_id; // stable platform camera identifier
  std::string name;        // optional human-readable label (may be empty)
};

// Normalized repeating stream request (validated by core).
struct StreamRequest {
  uint64_t stream_id = 0;            // core-issued
  uint64_t device_instance_id = 0;   // core-issued
  StreamIntent intent = StreamIntent::PREVIEW;

  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_fourcc = 0;        // canonical CamBANG FourCC-style format

  uint32_t target_fps_min = 0;       // 0 if unspecified
  uint32_t target_fps_max = 0;       // 0 if unspecified

  uint64_t profile_version = 0;      // core bookkeeping
};

// Normalized still capture request (validated by core).
struct CaptureRequest {
  uint64_t capture_id = 0;           // core-issued
  uint64_t device_instance_id = 0;   // core-issued

  uint64_t rig_id = 0;               // 0 if not a rig capture

  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_fourcc = 0;        // e.g., 'JPEG', 'RAW '

  uint64_t profile_version = 0;      // core bookkeeping
};

// Opaque spec patch payload (core-validated).
struct SpecPatchView {
  const void* data = nullptr;
  size_t size_bytes = 0;
};

// Native object reporting (for snapshot introspection).
// Native IDs are core-issued; provider reports create/destroy events.
struct NativeObjectCreateInfo {
  uint64_t native_id = 0;                 // core-issued
  uint32_t type = 0;                      // CamBANG-defined enum (core-owned definition)
  uint64_t root_id = 0;                   // lineage root id (core-issued)

  uint64_t owner_rig_id = 0;
  uint64_t owner_device_instance_id = 0;
  uint64_t owner_stream_id = 0;

  uint64_t created_ns = 0;                // monotonic if available (0 allowed)
  uint64_t bytes_allocated = 0;           // 0 if n/a
  uint32_t buffers_in_use = 0;            // 0 if n/a
};

struct NativeObjectDestroyInfo {
  uint64_t native_id = 0;                 // core-issued
  uint64_t destroyed_ns = 0;              // monotonic if available (0 allowed)
};

// Frame view delivered from provider.
// Provider retains buffer ownership until core calls release().
// release() must be safe and non-blocking; it is called from core thread context.
struct FrameView {
  // Correlation
  uint64_t device_instance_id = 0;
  uint64_t stream_id = 0;    // 0 if this frame belongs only to a still capture
  uint64_t capture_id = 0;   // 0 if this is a repeating stream frame

  // Image metadata
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_fourcc = 0;

  // Timing
  uint64_t timestamp_ns = 0; // platform timestamp; 0 if unknown

  // Buffer
  const uint8_t* data = nullptr;
  size_t size_bytes = 0;

  // Optional per-row stride (0 if tightly packed/unknown)
  uint32_t stride_bytes = 0;

  // Release hook
  using ReleaseFn = void (*)(void* user, const FrameView* frame);
  ReleaseFn release = nullptr;
  void* release_user = nullptr;

  void release_now() const {
    if (release) {
      release(release_user, this);
    }
  }
};

} // namespace cambang