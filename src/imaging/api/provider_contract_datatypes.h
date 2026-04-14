// src/provider/provider_contract_datatypes.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Pattern preset vocabulary is provider-agnostic and lives in the Pattern Module.
// It is safe to depend on here (no platform headers).
#include "pixels/pattern/pattern_registry.h"
#include "pixels/pattern/pattern_spec.h"

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


// Internal producer backing vocabulary.
//
// This models producer realization capability and is intentionally separate
// from payload/result taxonomy (e.g., ResultPayloadKind).
enum class ProducerBackingKind : uint8_t {
  CPU = 0,
  GPU = 1,
};

struct ProducerBackingCapabilities {
  bool cpu_backed_available = false;
  bool gpu_backed_available = false;
};

// Deterministic result for provider method calls.
struct ProviderResult {
  ProviderError code = ProviderError::OK;

  constexpr bool ok() const { return code == ProviderError::OK; }

  static constexpr ProviderResult success() { return ProviderResult{ProviderError::OK}; }
  static constexpr ProviderResult failure(ProviderError c) { return ProviderResult{c}; }
};


// Native object type vocabulary (core-owned).
//
// Canonical nouns: Provider, Device, Stream, FrameProducer.
// See docs/provider_architecture.md and docs/state_snapshot.md.
//
// NOTE: In the current scaffolding slice, providers fill `NativeObjectCreateInfo.type`
// directly. Core-side registry wiring may later replace or validate these values.
// Keep values stable once used.
enum class NativeObjectType : uint32_t {
  Provider = 1,
  Device = 2,
  Stream = 3,
  FrameProducer = 4,
};

// -----------------------------------------------------------------------------
// Stream configuration inputs (core -> provider)
//
// CaptureProfile: structural capture properties (geometry, format, fps).
// PictureConfig:  picture appearance parameters (pattern selection + overlays).
//
// These are provider-agnostic datatypes and contain no platform headers.
// Defaulting is performed by Core via StreamTemplate (provider default).
// -----------------------------------------------------------------------------

struct CaptureProfile {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_fourcc = 0;   // canonical CamBANG FourCC-style format
  uint32_t target_fps_min = 0;  // 0 if unspecified
  uint32_t target_fps_max = 0;  // 0 if unspecified
};

struct PictureConfig {
  // Pattern selection (synthetic/stub). Platform-backed providers may interpret
  // this as picture adjustment parameters subject to capability.
  PatternPreset preset = PatternPreset::XyXor;
  uint32_t seed = 0;

  // Synthetic source-generation cadence (independent of stream/profile FPS).
  // Render-driving frame ordinal samples synthetic time using:
  // floor(timestamp_ns * generator_fps_num / (1e9 * generator_fps_den)).
  // If either term is 0, source is treated as static.
  uint32_t generator_fps_num = 30;
  uint32_t generator_fps_den = 1;

  // Overlays (implemented by the Pattern Module renderer for synthetic/stub).
  bool overlay_frame_index_offsets = true;
  bool overlay_moving_bar = true;

  // Optional preset parameters.
  uint8_t solid_r = 0;
  uint8_t solid_g = 0;
  uint8_t solid_b = 0;
  uint8_t solid_a = 0xFF;

  uint32_t checker_size_px = 16;
};

struct StreamTemplate {
  CaptureProfile profile{};
  PictureConfig picture{};
};

struct CaptureTemplate {
  CaptureProfile profile{};
  PictureConfig picture{};
};

// Convert PictureConfig + geometry to a renderer PatternSpec.
// If out_preset_valid is provided, it is set to whether cfg.preset existed in registry.
// Invalid presets deterministically fall back to XyXor.
inline PatternSpec to_pattern_spec(const PictureConfig& cfg,
                                  uint32_t width,
                                  uint32_t height,
                                  PatternSpec::PackedFormat fmt,
                                  bool* out_preset_valid = nullptr) noexcept {
  const auto* info = find_preset_info(cfg.preset);
  const bool valid = (info != nullptr);
  if (out_preset_valid) *out_preset_valid = valid;

  // Deterministic fallback.
  if (!info) {
    info = find_preset_info(PatternPreset::XyXor);
  }

  PatternSpec spec{};
  spec.width = width;
  spec.height = height;
  spec.format = fmt;
  spec.seed = cfg.seed;
  spec.overlay_frame_index_offsets = cfg.overlay_frame_index_offsets;
  spec.overlay_moving_bar = cfg.overlay_moving_bar;

  spec.algo = info ? info->algo : PatternAlgoId::XyXor;
  spec.dynamic_base = info ? info->dynamic_base : false;

  const uint32_t caps = info ? info->caps : static_cast<uint32_t>(kCapsNone);
  if ((caps & static_cast<uint32_t>(kCapsRgba)) != 0u) {
    spec.solid_r = cfg.solid_r;
    spec.solid_g = cfg.solid_g;
    spec.solid_b = cfg.solid_b;
    spec.solid_a = cfg.solid_a;
  }
  if ((caps & static_cast<uint32_t>(kCapsCheckerSize)) != 0u) {
    spec.checker_size_px = cfg.checker_size_px;
  }
  return spec;
}

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

  // Effective stream configuration (owned by core; passed to provider).
  CaptureProfile profile{};
  PictureConfig picture{};

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
  PictureConfig picture{};

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

  uint64_t owner_device_instance_id = 0;
  uint64_t owner_stream_id = 0;
  uint64_t owner_provider_native_id = 0;  // 0 if unknown/none
  uint64_t owner_rig_id = 0;              // 0 if unknown/none

  bool has_created_ns = false;            // true if provider supplied created_ns
  uint64_t created_ns = 0;                // provider value (0 is valid when has_created_ns=true)
  uint64_t bytes_allocated = 0;           // 0 if n/a
  uint32_t buffers_in_use = 0;            // 0 if n/a
};

struct NativeObjectDestroyInfo {
  uint64_t native_id = 0;                 // core-issued
  bool has_destroyed_ns = false;          // true if provider supplied destroyed_ns
  uint64_t destroyed_ns = 0;              // provider value (0 is valid when has_destroyed_ns=true)
};

// Provider-agnostic capture timestamp.
// See provider_architecture.md §7.x (canonical).
enum class CaptureTimestampDomain : uint8_t {
  PROVIDER_MONOTONIC = 0,
  CORE_MONOTONIC = 1,
  // NOTE: Avoid the Windows GDI macro `OPAQUE` (commonly defined as 2 via wingdi.h)
  // which can break compilation when <windows.h> is included before this header.
  // Semantics remain "opaque / ordering-only" as described in provider_architecture.md.
  DOMAIN_OPAQUE = 2,
};

struct CaptureTimestamp {
  uint64_t value = 0;     // integer tick value
  uint32_t tick_ns = 0;   // tick period in nanoseconds (1 = ns, 100 = 100ns, etc.)
  CaptureTimestampDomain domain = CaptureTimestampDomain::DOMAIN_OPAQUE;
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
  ProducerBackingKind primary_backing_kind = ProducerBackingKind::CPU;

  // Timing
  CaptureTimestamp capture_timestamp{};

  // Buffer
  const uint8_t* data = nullptr;
  size_t size_bytes = 0;
  // Optional opaque primary artifact for non-CPU-backed frames.
  // For ProducerBackingKind::GPU this carries the authoritative provider->core
  // primary backing when available.
  std::shared_ptr<void> primary_backing_artifact{};

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
