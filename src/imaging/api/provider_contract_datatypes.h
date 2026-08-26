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

// `make_fourcc`, the `FOURCC_*` tags, and all pixel-layout arithmetic live in
// the pixel-format truth table. They are in namespace cambang, so every
// existing `FOURCC_RGBA` / `make_fourcc(...)` use in this header and its
// includers resolves unchanged.
#include "pixels/format/pixel_format_descriptor.h"

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

// --- Payload layout and colorimetry -----------------------------------------
//
// A payload's pixel format alone does not describe a YUV buffer. Range, matrix,
// transfer, and primaries are facts the provider knows at acquisition time and
// that nothing downstream can recover from the bytes. They travel with the
// payload so that any later conversion is correct rather than assumed.
//
// UNSPECIFIED is a truthful "the provider did not tell us", not a default
// value. Consumers must decide their own fallback explicitly rather than
// treating UNSPECIFIED as BT.601 (or anything else) silently.

enum class ColorRange : uint8_t {
  UNSPECIFIED = 0,
  LIMITED = 1,  // studio swing (Y 16-235, C 16-240 at 8-bit)
  FULL = 2,     // full swing (0-255 at 8-bit)
};

enum class ColorMatrix : uint8_t {
  UNSPECIFIED = 0,
  BT601 = 1,
  BT709 = 2,
  BT2020_NCL = 3,
};

enum class ColorTransfer : uint8_t {
  UNSPECIFIED = 0,
  BT709 = 1,
  SRGB = 2,
  PQ = 3,
  HLG = 4,
};

enum class ColorPrimaries : uint8_t {
  UNSPECIFIED = 0,
  BT709 = 1,
  BT2020 = 2,
  P3_D65 = 3,
};

struct PayloadColorimetry {
  ColorRange range = ColorRange::UNSPECIFIED;
  ColorMatrix matrix = ColorMatrix::UNSPECIFIED;
  ColorTransfer transfer = ColorTransfer::UNSPECIFIED;
  ColorPrimaries primaries = ColorPrimaries::UNSPECIFIED;
};

// Whether CamBANG has a YUV conversion implemented for this colour
// interpretation. Every conversion path -- CPU display, GPU shader -- gates on
// this, so adding a colour space is one edit rather than several.
//
// UNSPECIFIED is truthful absence, not a value, so the fallback has to be
// chosen explicitly rather than assumed: absence resolves to BT.601 limited.
//
// That fallback is KNOWN WRONG for Camera2 on measured hardware. This comment
// used to claim limited range "is what both current targets deliver for 8-bit
// 4:2:0 (Camera2's YUV_420_888 and WinRT's NV12)"; a Quest 3 reports
// ADATASPACE_JFIF for all three cameras -- BT.601-625, SMPTE 170M, and RANGE_
// FULL -- and its luma spans 0-255. See pixel_payload_and_result_contract.md
// 6.3.1 for the measurement and its scope limits. The fallback is left as-is
// deliberately: correcting it by flipping the guess would replace one
// assumption with another, and the platform declares the answer per buffer
// (AImage_getDataSpace) for a provider that does not yet read it. WinRT's half
// of the original claim remains untested.
//
// A *declared* colour space CamBANG cannot
// convert is refused outright -- rendering BT.709 content with BT.601
// coefficients yields a plausible image, which is worse than no image.
inline bool is_convertible_colorimetry(const PayloadColorimetry& c) noexcept {
  const bool matrix_ok =
      c.matrix == ColorMatrix::UNSPECIFIED || c.matrix == ColorMatrix::BT601;
  const bool range_ok =
      c.range == ColorRange::UNSPECIFIED || c.range == ColorRange::LIMITED;
  return matrix_ok && range_ok;
}

// One plane of a CPU-readable payload. The provider retains ownership; plane
// pointers share the frame's release lifetime.
struct PayloadPlaneView {
  const uint8_t* data = nullptr;
  size_t size_bytes = 0;
  // Bytes between the start of consecutive rows. 0 means "tightly packed for
  // this plane at this width", i.e. the descriptor's row_bytes.
  uint32_t stride_bytes = 0;
  // Rows addressable in this plane. 0 means "the descriptor's row count at
  // this height".
  uint32_t rows = 0;
  // Bytes between consecutive samples within a row. 1 means samples are
  // adjacent, which is the case for every plane of a fully planar format and
  // for the luma plane of every format.
  //
  // This is what distinguishes members of a flexible format family. Android's
  // YUV_420_888 always presents three planes and documents that "the Y-plane
  // is guaranteed not to be interleaved with the U/V planes (in particular,
  // pixel stride is always 1 in yPlane.getPixelStride())", while the U and V
  // planes "are guaranteed to have the same row stride and pixel stride". A
  // chroma pixel stride of 2 means U and V are interleaved in one buffer --
  // NV12 or NV21 -- and 1 means they are separate, I420 or YV12. The device
  // decides at runtime and only the strides reveal which.
  //
  // Without this field a provider would have to resolve that privately, which
  // is precisely the conversion knowledge this contract exists to move out of
  // providers.
  //
  // SCOPE: this describes the SOURCE buffer's sample spacing, and applies only
  // where a plane's row is measured in single components. A semi-planar chroma
  // plane's row_bytes already spans both interleaved components, so its pixel
  // stride is 1 by this definition even though consecutive U samples sit two
  // bytes apart -- the interleaving is carried by the format, not repeated
  // here. Declaring 2 in that case double-counts and fails extent validation.
  uint32_t pixel_stride_bytes = 1;
};

// Full CPU payload geometry for one frame.
//
// `plane_count == 0` means the layout is absent and the legacy single-plane
// FrameView scalars (data/size_bytes/stride_bytes/format_fourcc) are
// authoritative. Providers that emit only packed RGB need not populate this.
struct PayloadLayout {
  uint32_t format_fourcc = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint8_t plane_count = 0;
  PayloadColorimetry colorimetry{};
  PayloadPlaneView planes[kMaxPixelFormatPlanes]{};

  constexpr bool present() const noexcept { return plane_count != 0; }
};

// Structural validation of a populated layout against its format descriptor.
//
// Checks that the plane count matches the format, every plane has bytes, and
// each plane's stride and size actually span the rows the format requires.
// This is geometry validation only: it says the buffer is addressable as
// claimed, not that any CamBANG path can retain or display it.
inline bool validate_payload_layout(const PayloadLayout& layout) noexcept {
  if (!layout.present() || layout.width == 0 || layout.height == 0) {
    return false;
  }
  const PixelFormatDescriptor desc = describe_pixel_format(layout.format_fourcc);
  if (!desc.valid || layout.plane_count != desc.plane_count) {
    return false;
  }
  for (uint32_t plane = 0; plane < layout.plane_count; ++plane) {
    const PayloadPlaneView& pv = layout.planes[plane];
    if (pv.data == nullptr || pv.size_bytes == 0) {
      return false;
    }
    const size_t row_bytes = static_cast<size_t>(plane_row_bytes(desc, plane, layout.width));
    const size_t rows = (pv.rows != 0)
        ? static_cast<size_t>(pv.rows)
        : static_cast<size_t>(plane_rows(desc, plane, layout.height));
    if (row_bytes == 0 || rows == 0 || rows < plane_rows(desc, plane, layout.height)) {
      return false;
    }
    // A row spans (samples-1)*pixel_stride + 1 sample-worth of bytes when
    // samples are interleaved, which exceeds the tightly packed row_bytes.
    const size_t pixel_stride =
        (pv.pixel_stride_bytes != 0) ? static_cast<size_t>(pv.pixel_stride_bytes) : 1u;
    if (pixel_stride == 0 || row_bytes == 0) {
      return false;
    }
    const size_t row_extent =
        (pixel_stride == 1) ? row_bytes : ((row_bytes - 1u) * pixel_stride + 1u);
    const size_t stride = (pv.stride_bytes != 0) ? static_cast<size_t>(pv.stride_bytes) : row_extent;
    if (stride < row_extent) {
      return false;
    }
    // Last row needs only its own row_bytes, not a full stride.
    if (stride > ((static_cast<size_t>(-1) - row_extent) / rows)) {
      return false;
    }
    if (pv.size_bytes < (stride * (rows - 1u)) + row_extent) {
      return false;
    }
  }
  return true;
}

// Provider-declared native pixel formats for a stream or capture shape.
//
// This is acquisition capability truth, parallel to ProducerBackingCapabilities
// and equally distinct from payload-kind policy: it states what the provider
// can emit without converting, in the provider's own preference order.
//
// `can_emit_packed_rgb` records whether the provider will convert to a packed
// RGBA/BGRA buffer on request. The default advertisement below describes every
// provider in the tree today: RGBA/BGRA native, conversion available.
struct ProducerFormatCapabilities {
  static constexpr uint8_t kMaxFormats = 8;

  uint32_t formats[kMaxFormats]{};
  uint8_t count = 0;
  bool can_emit_packed_rgb = true;

  constexpr bool supports(uint32_t fourcc) const noexcept {
    for (uint8_t i = 0; i < count; ++i) {
      if (formats[i] == fourcc) {
        return true;
      }
    }
    return false;
  }

  // Appends a format if there is room and it is not already advertised.
  // Returns false when the entry was dropped, so a provider advertising more
  // than kMaxFormats fails visibly rather than silently truncating.
  constexpr bool add(uint32_t fourcc) noexcept {
    if (supports(fourcc)) {
      return true;
    }
    if (count >= kMaxFormats) {
      return false;
    }
    formats[count++] = fourcc;
    return true;
  }

  static constexpr ProducerFormatCapabilities packed_rgb_only() noexcept {
    ProducerFormatCapabilities caps{};
    caps.formats[0] = FOURCC_RGBA;
    caps.formats[1] = FOURCC_BGRA;
    caps.count = 2;
    caps.can_emit_packed_rgb = true;
    return caps;
  }
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
  // Device Capture Id, and only meaningful for DEVICE_CAPTURE origin, where a
  // submission wraps exactly one request and shares its id. A rig submission
  // has N members with N distinct Device Capture Ids and no single one of its
  // own, so this is 0 there -- see rig_capture_id.
  uint64_t capture_id = 0;
  CaptureSubmissionOrigin origin = CaptureSubmissionOrigin::DEVICE_CAPTURE;
  uint64_t rig_id = 0;
  // Rig Capture Id, set for RIG_CAPTURE origin and 0 otherwise. A separate
  // field because it is a separate id space: putting it in capture_id would
  // make one field mean two incompatible things depending on origin, which is
  // the confusion capture_identity_and_lifecycle.md 2.1 removes.
  uint64_t rig_capture_id = 0;
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

// How a retained GPU backing's memory is describable to Core.
//
// This exists because `stride_bytes` and `format_fourcc` are only meaningful
// for some GPU resources. The synthetic provider's backing is a linear RGBA8
// texture CamBANG allocated itself, so both fields are exact. A real
// platform-backed still is not: Camera2 delivers AIMAGE_FORMAT_PRIVATE through
// an AHardwareBuffer whose layout is tiled or vendor-defined, and WinRT
// delivers a D3D11 texture behind IDirect3DSurface. For those there is no
// row stride Core can compute with, and the pixel format may be vendor-private.
//
// Defaulting to LINEAR keeps every descriptor written before this enum existed
// truthful -- all of them are linear -- and makes the opaque case something a
// provider has to declare deliberately.
enum class GpuBackingLayoutKind : uint8_t {
  // Rows of `stride_bytes`, pixels described by `format_fourcc`. Both fields
  // are authoritative.
  LINEAR = 0,
  // A real GPU resource whose memory layout Core cannot describe. Producers
  // must leave `stride_bytes` zero; `format_fourcc` may also be zero when even
  // the pixel format is vendor-private. Nothing may compute a byte extent from
  // this descriptor's stride -- use retained_gpu_backing_footprint_bytes().
  //
  // Named OPAQUE_EXTERNAL rather than the obvious OPAQUE because wingdi.h
  // defines OPAQUE as a macro, and this header is reachable from every MSVC
  // translation unit in the tree.
  OPAQUE_EXTERNAL = 1,
};

// Neutral description of a retained GPU backing. This struct intentionally
// carries only scalar identity, shape, and capability facts so provider/core
// metadata can describe GPU-backed frames without exposing rendering-resource
// ownership details to CoreResultStore or Godot-facing result objects.
struct RetainedGpuBackingDescriptor {
  bool valid = false;
  uint64_t stream_id = 0;
  // Opaque backing identity or generation. Zero means the provider/core path
  // knows a GPU backing exists but has no scalar identity.
  //
  // Must be unique across the whole provider, not merely within one stream: it
  // is what identifies a backing on its own, including for a still capture,
  // whose stream_id is legitimately 0. A per-stream counter here would collide
  // across streams in any descriptor-keyed cache. Recreating a backing must
  // mint a fresh value so a stale generation is detectable.
  uint64_t backing_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  GpuBackingLayoutKind layout_kind = GpuBackingLayoutKind::LINEAR;
  // Authoritative only for LINEAR. Must be zero for OPAQUE.
  uint32_t stride_bytes = 0;
  // Authoritative only for LINEAR. May be zero for OPAQUE when the pixel
  // format is vendor-private.
  uint32_t format_fourcc = 0;
  // Producer-declared footprint of the GPU allocation, when the producer
  // genuinely knows it. Zero means "not declared" and Core estimates instead;
  // it does not mean the backing is free. Read through
  // retained_gpu_backing_footprint_bytes() rather than directly, so retention
  // accounting and access-cost evidence cannot disagree.
  uint64_t allocation_size_bytes = 0;
  bool display_available = false;
  // Display is available, but producing a Godot-usable texture from this
  // backing costs real work the first time -- importing an AHardwareBuffer
  // into Vulkan, or opening a D3D11 shared handle. A backing CamBANG already
  // holds as a native texture leaves this false. Import must never be
  // performed eagerly at retention time; this bit exists so display access can
  // be classified honestly and the work deferred to actual demand.
  bool display_requires_import = false;
  bool materialization_available = false;
  bool materialization_requires_gpu_readback = false;
};

// Deliberately conservative bytes-per-pixel used only when a GPU backing
// declares no allocation size and has no linear stride to measure. It
// over-counts every subsampled format (NV12 is 1.5), which is the safe
// direction: retention accounting that over-counts evicts early, while
// under-counting lets GPU allocations accumulate unbounded.
inline constexpr uint64_t kUndeclaredGpuBackingBytesPerPixel = 4;

// Single source of truth for "how much memory does this GPU backing occupy".
// Three call sites previously open-coded this and two of them disagreed, one
// silently yielding zero for any backing without a linear stride.
inline uint64_t retained_gpu_backing_footprint_bytes(
    const RetainedGpuBackingDescriptor& descriptor) noexcept {
  if (!descriptor.valid) {
    return 0;
  }
  if (descriptor.allocation_size_bytes != 0) {
    return descriptor.allocation_size_bytes;
  }
  if (descriptor.layout_kind == GpuBackingLayoutKind::LINEAR &&
      descriptor.stride_bytes != 0 && descriptor.height != 0) {
    return static_cast<uint64_t>(descriptor.stride_bytes) *
           static_cast<uint64_t>(descriptor.height);
  }
  if (descriptor.width != 0 && descriptor.height != 0) {
    return static_cast<uint64_t>(descriptor.width) *
           static_cast<uint64_t>(descriptor.height) *
           kUndeclaredGpuBackingBytesPerPixel;
  }
  return 0;
}

// Whether a descriptor's own fields are mutually consistent. Core checks this
// where a descriptor enters retention; provider compliance checks it at the
// seam.
inline bool retained_gpu_backing_descriptor_is_self_consistent(
    const RetainedGpuBackingDescriptor& descriptor) noexcept {
  if (!descriptor.valid) {
    return true;
  }
  if (descriptor.width == 0 || descriptor.height == 0) {
    return false;
  }
  if (descriptor.layout_kind == GpuBackingLayoutKind::OPAQUE_EXTERNAL &&
      descriptor.stride_bytes != 0) {
    return false;
  }
  return true;
}

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

  // Optional multi-plane CPU payload geometry plus colorimetry.
  //
  // Absent (plane_count == 0) means this frame is single-plane and the scalar
  // fields above are authoritative -- that is the case for every provider in
  // the tree today, so no existing provider needs to populate this. A provider
  // emitting a planar or semi-planar payload must populate it, because the
  // scalars cannot describe more than one plane.
  //
  // Read through effective_payload_layout() rather than directly, so callers
  // get the same answer for legacy and layout-bearing frames.
  PayloadLayout payload_layout{};

  // Resolved CPU payload geometry for this frame.
  //
  // Returns the populated layout when present, otherwise synthesizes the
  // equivalent single-plane layout from the scalar fields. Colorimetry is
  // carried only by an explicit layout; a synthesized one reports UNSPECIFIED,
  // which is truthful -- a legacy frame never stated it.
  PayloadLayout effective_payload_layout() const {
    if (payload_layout.present()) {
      PayloadLayout out = payload_layout;
      // Tolerate a layout that carries planes but leaves the shared image
      // description to the scalars.
      if (out.format_fourcc == 0) out.format_fourcc = format_fourcc;
      if (out.width == 0) out.width = width;
      if (out.height == 0) out.height = height;
      return out;
    }

    PayloadLayout out{};
    if (data == nullptr || size_bytes == 0) {
      return out;
    }
    const PixelFormatDescriptor desc = describe_pixel_format(format_fourcc);
    if (!desc.valid || desc.plane_count != 1) {
      // A multi-plane format delivered through the single-plane scalars is
      // malformed; report absence rather than a layout that claims one plane
      // holds all of it.
      return out;
    }
    out.format_fourcc = format_fourcc;
    out.width = width;
    out.height = height;
    out.plane_count = 1;
    out.planes[0].data = data;
    out.planes[0].size_bytes = size_bytes;
    out.planes[0].stride_bytes = stride_bytes;
    out.planes[0].rows = height;
    return out;
  }

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
