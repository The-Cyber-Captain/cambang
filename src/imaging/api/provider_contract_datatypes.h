// src/provider/provider_contract_datatypes.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/capture_admission_context.h"
#include "core/camera_fact_types.h"

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
  // Deadline exceeded before a terminal capture fact arrived. Used by Core's
  // own capture-admission watchdog (see icamera_provider.h's
  // capture_admission_watchdog_timeout_ns()); also available for a provider
  // to return directly for a genuine hardware-timeout failure.
  ERR_TIMEOUT,
};


// Internal producer backing vocabulary.
//
// This models producer realization capability and is intentionally separate
// from payload/result taxonomy (e.g., ResultPayloadKind).
enum class ProducerBackingKind : uint8_t {
  CPU = 0,
  GPU = 1,
};

enum class CoreProductionPostureShape : uint8_t {
  CpuPrimary = 0,
  GpuPrimaryNoCpuSidecar = 1,
  GpuPrimaryWithCpuSidecar = 2,
};

struct CoreRetainedProductionPlan {
  CoreProductionPostureShape posture = CoreProductionPostureShape::CpuPrimary;
  bool valid = false;

  constexpr bool primary_cpu() const noexcept { return posture == CoreProductionPostureShape::CpuPrimary; }
  constexpr bool primary_gpu() const noexcept { return posture != CoreProductionPostureShape::CpuPrimary; }
  constexpr bool retain_cpu_sidecar() const noexcept {
    return posture == CoreProductionPostureShape::CpuPrimary ||
           posture == CoreProductionPostureShape::GpuPrimaryWithCpuSidecar;
  }
  constexpr bool retain_gpu_display() const noexcept { return primary_gpu(); }
};

struct ProducerBackingCapabilities {
  bool cpu_backed_available = false;
  bool gpu_backed_available = false;
  bool gpu_with_cpu_sidecar_available = false;

  constexpr bool viable(CoreProductionPostureShape posture) const noexcept {
    switch (posture) {
      case CoreProductionPostureShape::CpuPrimary:
        return cpu_backed_available;
      case CoreProductionPostureShape::GpuPrimaryNoCpuSidecar:
        return gpu_backed_available;
      case CoreProductionPostureShape::GpuPrimaryWithCpuSidecar:
        return gpu_backed_available && gpu_with_cpu_sidecar_available;
    }
    return false;
  }
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
// Canonical nouns: Provider, Device, AcquisitionSession, Stream.
// See docs/provider_architecture.md and docs/state_snapshot.md.
//
// NOTE: In the current scaffolding slice, providers fill `NativeObjectCreateInfo.type`
// directly. Core-side registry wiring may later replace or validate these values.
// Keep values stable once used.
enum class NativeObjectType : uint32_t {
  Provider = 1,
  Device = 2,
  AcquisitionSession = 3,
  Stream = 4,
  FrameBufferLease = 5,
  GpuBacking = 6,
};

// -----------------------------------------------------------------------------
// Stream configuration inputs (core -> provider)
//
// CaptureProfile: structural capture properties (geometry, format, fps).
// PictureConfig:  picture appearance parameters (pattern selection + overlays).
//
// These are provider-agnostic datatypes and contain no platform headers.
// Defaulting is performed by Core via StreamTemplate and CaptureTemplate
// (provider defaults).
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

enum class CaptureStillImageMemberRole : uint8_t {
  DEFAULT_METERED = 0,
  ADDITIONAL_BRACKET = 1,
};

struct CaptureStillImageMember {
  uint32_t image_member_index = 0;
  CaptureStillImageMemberRole role = CaptureStillImageMemberRole::DEFAULT_METERED;
  int32_t intended_exposure_compensation_milli_ev = 0;
};

struct CaptureStillImageBundle {
  std::vector<CaptureStillImageMember> members{};
};

inline CaptureStillImageBundle make_default_metered_still_image_bundle() {
  CaptureStillImageBundle seq{};
  seq.members.push_back(CaptureStillImageMember{0u, CaptureStillImageMemberRole::DEFAULT_METERED});
  return seq;
}

inline bool is_valid_capture_still_image_bundle(
    const CaptureStillImageBundle& seq,
    bool supports_multi_image_still_sequence) noexcept {
  if (seq.members.empty()) {
    return false;
  }
  if (seq.members[0].image_member_index != 0u ||
      seq.members[0].role != CaptureStillImageMemberRole::DEFAULT_METERED ||
      seq.members[0].intended_exposure_compensation_milli_ev != 0) {
    return false;
  }
  for (size_t i = 0; i < seq.members.size(); ++i) {
    const auto& m = seq.members[i];
    if (m.image_member_index != static_cast<uint32_t>(i)) {
      return false;
    }
    if (i > 0) {
      if (m.role == CaptureStillImageMemberRole::DEFAULT_METERED) {
        return false;
      }
      if (m.role != CaptureStillImageMemberRole::ADDITIONAL_BRACKET) {
        return false;
      }
    }
  }
  if (seq.members.size() > 1 && !supports_multi_image_still_sequence) {
    return false;
  }
  return true;
}

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
  CoreRetainedProductionPlan requested_retained_plan{}; // core-selected internal production posture
};

enum class CaptureSubmissionOrigin : uint8_t {
  DEVICE_CAPTURE = 0,
  RIG_CAPTURE = 1,
};

// Normalized still capture request (validated by core).
struct CaptureRequest {
  uint64_t capture_id = 0;           // core-issued
  uint64_t device_instance_id = 0;   // core-issued

  uint64_t rig_id = 0;               // 0 if not a rig capture
  bool has_admission_context = false;
  CaptureAdmissionContext admission_context{};

  uint32_t width = 0;
  uint32_t height = 0;
  // Materialized provider-agnostic still-result format FourCC. Current
  // implemented displayable still paths use packed pixel formats such as
  // FOURCC_RGBA / FOURCC_BGRA; encoded or RAW representations require matching
  // payload-kind/result support and are not enabled by this field alone.
  uint32_t format_fourcc = 0;
  PictureConfig picture{};
  CaptureStillImageBundle still_image_bundle{};

  uint64_t profile_version = 0;      // core bookkeeping
  CoreRetainedProductionPlan requested_retained_plan{}; // core-selected internal production posture
};

// Normalized provider capture submission. A device capture is represented as a
// one-device submission; a rig capture is represented as one grouped submission
// containing all admitted member-device requests for a shared capture_id/rig_id.
struct CaptureSubmission {
  uint64_t capture_id = 0;
  CaptureSubmissionOrigin origin = CaptureSubmissionOrigin::DEVICE_CAPTURE;
  uint64_t rig_id = 0;
  std::vector<CaptureRequest> device_requests{};
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
  uint64_t owner_acquisition_session_id = 0;
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

// Internal still-capture image routing marker (provider -> core).
//
// Default-initialized and legacy-populated frames remain DEFAULT_METERED.
// Providers that begin emitting bracket stills can set ADDITIONAL_BRACKET
// to route those frames into capture additional_images.
enum class CaptureImageRouting : uint8_t {
  DEFAULT_METERED = 0,
  ADDITIONAL_BRACKET = 1,
};

struct CaptureImageFrameMetadata {
  CaptureImageRouting routing = CaptureImageRouting::DEFAULT_METERED;
  uint32_t image_member_index = 0;
  int32_t applied_exposure_compensation_milli_ev = 0;
  bool has_realized_exposure_compensation_milli_ev = false;
  int32_t realized_exposure_compensation_milli_ev = 0;
};

// Neutral description of a retained GPU backing. This struct intentionally
// carries only scalar identity, shape, and capability facts so provider/core
// metadata can describe GPU-backed frames without exposing rendering-resource
// ownership details to CoreResultStore or Godot-facing result objects.
struct RetainedGpuBackingDescriptor {
  bool valid = false;
  uint64_t stream_id = 0;
  // Opaque provider-scoped backing identity or generation. Zero means the
  // provider/core path knows a GPU backing exists but has no scalar identity.
  uint64_t backing_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  uint32_t format_fourcc = 0;
  bool display_available = false;
  bool materialization_available = false;
  bool materialization_requires_gpu_readback = false;
};

// Frame view delivered from provider.
// Provider retains buffer ownership until core calls release().
// release() must be safe and non-blocking; it is called from core thread context.
struct FrameView {
  // Correlation
  uint64_t device_instance_id = 0;
  uint64_t stream_id = 0;    // 0 if this frame belongs only to a still capture
  uint64_t acquisition_session_id = 0; // 0 if unavailable/unknown
  uint64_t capture_id = 0;   // 0 if this is a repeating stream frame
  CaptureImageFrameMetadata capture_image{};

  // Image metadata
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_fourcc = 0;
  ProducerBackingKind primary_backing_kind = ProducerBackingKind::CPU;

  // Optional provider-authored timing for this exact acquired frame. A present
  // zero-valued acquisition mark is valid and remains distinct from absence.
  std::optional<SourcedFact<ImageAcquisitionTiming>> acquisition_timing{};

  // Buffer
  const uint8_t* data = nullptr;
  size_t size_bytes = 0;
  // Internal provider->Core retention intent for CPU bytes. This distinguishes
  // CPU bytes deliberately published for primary/sidecar retention from
  // provider-local staging/upload details. CPU-primary frames with CPU payload
  // are still retained as primary by Core; GPU-primary frames retain CPU sidecar
  // data only when this remains true.
  bool retain_cpu_sidecar = true;
  // Optional immutable owner for tightly packed CPU payload bytes. Providers may
  // set this only when the pointed-to vector exactly backs data/size_bytes and
  // will not be mutated after posting. Core may then retain/adopt the shared
  // payload instead of copying it. release_now() still releases provider-side
  // frame bookkeeping; this shared owner is the retained-result byte lifetime.
  std::shared_ptr<const std::vector<uint8_t>> cpu_payload_owner{};
  // Optional opaque primary artifact for non-CPU-backed frames.
  // For ProducerBackingKind::GPU this carries the authoritative provider->core
  // primary backing when available.
  std::shared_ptr<void> primary_backing_artifact{};
  // Neutral metadata for the primary GPU backing above. This tranche keeps the
  // legacy primary_backing_artifact path authoritative for behavior; the
  // descriptor is passive scaffolding for later resource-ownership isolation.
  RetainedGpuBackingDescriptor retained_gpu_backing_descriptor{};
  // Echo of the Core-requested internal retention posture that produced this frame.
  CoreRetainedProductionPlan requested_retained_plan{};

  // Optional per-row stride (0 if tightly packed/unknown)
  uint32_t stride_bytes = 0;

  // Release hook.
  //
  // THREADING (load-bearing, not an implementation detail): release() has no
  // fixed thread affinity relative to the thread that called on_frame() to
  // deliver this frame, and the calling thread differs by outcome:
  //   - Normal path: once Core has finished with the frame (consumed/retained
  //     it, or is dropping it after successful ingress), release() is invoked
  //     from Core's own dedicated core thread -- NOT the Provider thread that
  //     delivered the frame.
  //   - Ingress-failure path (Core could not accept the frame, e.g. queue
  //     full or closing): release() is invoked synchronously, still inside
  //     the Provider's own on_frame() call, on whatever thread the Provider
  //     used to call on_frame().
  // A Provider's release callback MUST therefore be safe to invoke from a
  // thread other than the one that produced the frame, and must not assume
  // any particular thread/context affinity (e.g. a GPU context or buffer-pool
  // API that requires same-thread symmetry with acquisition is NOT safe to
  // drive directly from this callback without its own internal marshalling).
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
