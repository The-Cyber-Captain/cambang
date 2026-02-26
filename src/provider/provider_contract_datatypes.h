// src/provider/provider_contract_datatypes.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cambang {

// --- FourCC helpers ---------------------------------------------------------
// Canonical CamBANG pixel formats use a FourCC-style 32-bit tag.
// Use these helpers instead of ad-hoc literals to keep format handling stable.
constexpr uint32_t make_fourcc(char a, char b, char c, char d) {
  return (static_cast<uint32_t>(static_cast<unsigned char>(a))      ) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b)) <<  8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

// Common formats used by dev scaffolding.
// NOTE: Do not assume these are the only formats CamBANG will ever support.
inline constexpr uint32_t FOURCC_RGBA = make_fourcc('R', 'G', 'B', 'A');
inline constexpr uint32_t FOURCC_BGRA = make_fourcc('B', 'G', 'R', 'A');

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

// Provider-agnostic capture timestamp.
// See provider_architecture.md §7.x (canonical).
enum class CaptureTimestampDomain : uint8_t {
  PROVIDER_MONOTONIC = 0,
  CORE_MONOTONIC = 1,
  OPAQUE = 2,
};

struct CaptureTimestamp {
  uint64_t value = 0;     // integer tick value
  uint32_t tick_ns = 0;   // tick period in nanoseconds (1 = ns, 100 = 100ns, etc.)
  CaptureTimestampDomain domain = CaptureTimestampDomain::OPAQUE;
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
  CaptureTimestamp capture_timestamp{};

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