#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "core/camera_fact_types.h"
#include "core/capture_admission_context.h"
#include "core/result_fact_types.h"
#include "core/result_payload_kind.h"
#include "core/result_capability.h"
#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

// Explicit CamBANG-wide multiplier used by bounded retained-access
// calibration to split supported non-ready candidates into CHEAP vs EXPENSIVE.
// Keep this visible and testable; do not hide threshold policy in arbitrary
// helper code.
constexpr uint64_t kResultAccessCheapWithinBestMultiplier = 2;

// One plane's placement inside CoreResultPayloadCpu's single byte buffer.
struct CoreResultPayloadCpuPlane {
  size_t offset_bytes = 0;
  uint32_t stride_bytes = 0;
  uint32_t rows = 0;
};

// Retained CPU payload for one image.
//
// Packed and planar payloads share one contiguous byte buffer. That keeps the
// retained_bytes zero-copy adoption path usable for both, and matches how
// camera stacks actually deliver planar data (for NV12, luma then interleaved
// chroma in a single allocation).
//
// plane_count == 0 means a packed payload described entirely by the scalar
// stride below -- the shape every pre-planar path already expects. A planar
// payload populates plane_count and planes[], and its scalar stride_bytes
// describes plane 0 only.
struct CoreResultPayloadCpu {
  uint32_t format_fourcc = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  uint8_t plane_count = 0;
  CoreResultPayloadCpuPlane planes[kMaxPixelFormatPlanes]{};
  // Provider-declared colour interpretation, retained with the bytes because
  // no later layer can recover it from them. Only meaningful for YUV-family
  // payloads; packed RGB leaves it UNSPECIFIED.
  PayloadColorimetry colorimetry{};
  // Legacy/self-owned byte storage. New retained-result paths may instead keep
  // immutable provider-owned bytes alive through retained_bytes to avoid an
  // extra full-frame copy. Use data()/size_bytes()/empty() for reads.
  std::vector<uint8_t> bytes;
  std::shared_ptr<const std::vector<uint8_t>> retained_bytes{};

  const uint8_t* data() const noexcept {
    return retained_bytes ? retained_bytes->data() : bytes.data();
  }
  size_t size_bytes() const noexcept {
    return retained_bytes ? retained_bytes->size() : bytes.size();
  }
  bool empty() const noexcept { return size_bytes() == 0; }
  bool uses_retained_bytes() const noexcept { return static_cast<bool>(retained_bytes); }
  bool is_planar() const noexcept { return plane_count > 1; }

  // Start of `plane` within the retained buffer, or nullptr when the plane is
  // absent or its extent does not fit the buffer. Bounds are re-checked here
  // rather than trusted from retention time, because the buffer may be an
  // adopted provider allocation.
  const uint8_t* plane_data(uint32_t plane) const noexcept {
    if (plane >= plane_count) {
      return nullptr;
    }
    const CoreResultPayloadCpuPlane& p = planes[plane];
    if (p.rows == 0 || p.stride_bytes == 0) {
      return nullptr;
    }
    const size_t span = static_cast<size_t>(p.stride_bytes) * static_cast<size_t>(p.rows);
    const size_t total = size_bytes();
    if (span < p.stride_bytes || span > total || p.offset_bytes > (total - span)) {
      return nullptr;
    }
    return data() + p.offset_bytes;
  }
};

struct CoreResultAccessPostureKey {
  // Stable internal calibration epoch for a concrete applied production
  // posture/access domain. This is not retained artifact identity, frame
  // sequence, capture_id, timestamp, or public wrapper identity.
  uint64_t posture_id = 0;
  uint64_t stream_id = 0;
  uint64_t device_instance_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t format_fourcc = 0;
  ResultPayloadKind payload_kind = ResultPayloadKind::CPU_PACKED;
  bool has_retained_cpu_payload = false;
  bool has_retained_gpu_backing = false;
  bool gpu_materialization_available = false;
  bool gpu_materialization_requires_readback = false;
};

struct CoreRetainedAccessTruth {
  ResultCapability display_view = ResultCapability::UNSUPPORTED;
  ResultCapability to_image = ResultCapability::UNSUPPORTED;
  ResultCapability encoded_bytes = ResultCapability::UNSUPPORTED;
};

enum class CoreResultAccessOperation : uint8_t {
  DISPLAY_VIEW = 0,
  TO_IMAGE = 1,
  ENCODED_BYTES = 2,
};

// Result-facing access classification refinement. Provisional retained access
// truth remains structural; this narrow record lets bounded calibration replace
// only the result-facing answer for supported non-ready operations once
// evidence is available. A negative value means "no refined class".
struct CoreResultAccessClassificationRecord {
  std::atomic<int> display_view{-1};
  std::atomic<int> to_image{-1};
  std::atomic<int> encoded_bytes{-1};
};

using SharedResultAccessClassificationRecord =
    std::shared_ptr<CoreResultAccessClassificationRecord>;

ResultCapability resolve_result_access_classification(
    ResultCapability provisional,
    const SharedResultAccessClassificationRecord& record,
    CoreResultAccessOperation operation) noexcept;

void refine_result_access_classification(
    const SharedResultAccessClassificationRecord& record,
    CoreResultAccessOperation operation,
    ResultCapability classification) noexcept;

ResultCapability classify_supported_non_ready_result_access_from_normalized_costs(
    ResultCapability provisional,
    const uint64_t* normalized_costs,
    size_t normalized_cost_count) noexcept;

// Internal pre-production intent for the backing forms Core intends to retain
// for a result artifact. This is deliberately narrower than provider/source
// capability and remains separate from retained_access_truth, which records the
// public operations supported by the artifact that was actually retained.
struct CoreRetainedBackingPlan {
  ResultPayloadKind primary_kind = ResultPayloadKind::CPU_PACKED;
  bool retain_cpu_sidecar = false;
  bool retain_gpu_display = false;
};

struct CoreImageFactBundle {
  bool has_image_properties = false;
  ResultImagePropertiesFacts image_properties{};
  ResultImagePropertiesProvenance image_properties_provenance{};
};

// Private completed-result fact view. These source-neutral facts deliberately
// remain separate from the legacy flattened result facts above until a public
// result surface is explicitly approved.
struct CoreResolvedCaptureImageFacts {
  CameraStaticFacts camera;
  CaptureImageFacts image;
};

struct CoreStreamResultData {
  uint64_t stream_id = 0;
  uint64_t device_instance_id = 0;
  StreamIntent intent = StreamIntent::PREVIEW;
  uint64_t retained_frame_id = 0;
  uint32_t image_width = 0;
  uint32_t image_height = 0;
  uint32_t image_format_fourcc = 0;
  ResultPayloadKind payload_kind = ResultPayloadKind::CPU_PACKED;
  // For stream paths this may reference stream-owned live backing updated in
  // place while flowing; this is display/live-state retention, not frozen
  // per-frame GPU artifact identity.
  std::shared_ptr<void> retained_gpu_backing{};
  // Passive neutral descriptor for retained_gpu_backing. This does not drive
  // display or materialization yet; it records scalar GPU-primary facts while
  // legacy retained_gpu_backing remains the behavior path. If payload below is
  // current for the same retained frame, classify the result as
  // GPU-primary with CPU sidecar data rather than GPU-only.
  RetainedGpuBackingDescriptor retained_gpu_backing_descriptor{};
  CoreResultPayloadCpu payload{};
  CoreRetainedAccessTruth retained_access_truth{};
  SharedResultAccessClassificationRecord access_classification{};
  CoreResultAccessPostureKey access_posture{};
  // Non-zero only when payload was copied from the same FrameView as this
  // retained stream result. Used to distinguish current CPU materialization
  // from unsupported GPU-only readback.
  uint64_t payload_retained_frame_id = 0;
  CaptureImageFacts image_facts{};
  CoreImageFactBundle facts{};
};

struct CoreCaptureResultData {
  enum class ImageMemberRole : uint8_t {
    DEFAULT_METERED = 0,
    ADDITIONAL_BRACKET = 1,
  };

  // Image member of a single CaptureResult.
  // Result-level shared facts stay on CoreCaptureResultData; only genuinely
  // per-image fields live here.
  struct ImageMemberData {
    uint32_t image_member_index = 0;
    ImageMemberRole role = ImageMemberRole::DEFAULT_METERED;
    int32_t applied_exposure_compensation_milli_ev = 0;
    bool has_realized_exposure_compensation_milli_ev = false;
    int32_t realized_exposure_compensation_milli_ev = 0;
    uint64_t retained_frame_id = 0;
    std::optional<SourcedFact<ImageAcquisitionTiming>> acquisition_timing;
    ResultPayloadKind payload_kind = ResultPayloadKind::CPU_PACKED;
    CoreResultPayloadCpu payload{};
    std::shared_ptr<void> retained_gpu_backing{};
    RetainedGpuBackingDescriptor retained_gpu_backing_descriptor{};
    CoreRetainedAccessTruth retained_access_truth{};
    SharedResultAccessClassificationRecord access_classification{};
    CoreResultAccessPostureKey access_posture{};

    CoreResolvedCaptureImageFacts resolved_image_facts{};
  };

  uint64_t capture_id = 0;
  uint64_t device_instance_id = 0;
  uint64_t acquisition_session_id = 0;

  uint32_t image_width = 0;
  uint32_t image_height = 0;
  uint32_t image_format_fourcc = 0;
  ResultPayloadKind payload_kind = ResultPayloadKind::CPU_PACKED;

  ImageMemberData default_image{};
  std::vector<ImageMemberData> additional_images{};
  bool has_additional_images() const noexcept { return !additional_images.empty(); }
  uint32_t image_member_count() const noexcept { return 1u + static_cast<uint32_t>(additional_images.size()); }
  const ImageMemberData* image_member_at(uint32_t image_member_index) const noexcept {
    if (image_member_index == 0) return &default_image;
    const uint32_t additional_index = image_member_index - 1u;
    if (additional_index >= static_cast<uint32_t>(additional_images.size())) return nullptr;
    return &additional_images[additional_index];
  }
  ImageMemberData* image_member_at(uint32_t image_member_index) noexcept {
    if (image_member_index == 0) return &default_image;
    const uint32_t additional_index = image_member_index - 1u;
    if (additional_index >= static_cast<uint32_t>(additional_images.size())) return nullptr;
    return &additional_images[additional_index];
  }

  CoreImageFactBundle facts{};
  bool has_admission_context = false;
  CaptureAdmissionContext admission_context{};
  bool capture_image_facts_finalized = false;
};

using SharedStreamResultData = std::shared_ptr<const CoreStreamResultData>;
using SharedCaptureResultData = std::shared_ptr<const CoreCaptureResultData>;
using MutableCaptureResultData = std::shared_ptr<CoreCaptureResultData>;

// --- Structural admissibility of a retained CPU payload ----------------------
//
// Single source of truth for "can this operation structurally use these bytes".
//
// Core derives Operation Support from these, and the Godot access paths gate on
// the same functions. That coupling is the point. The contract deliberately
// keeps two layers of checking -- capability methods consume Operation Support,
// while materialization methods stay defensive about the concrete path they
// take (see pixel_payload_and_result_contract.md 11.2) -- but the defensive
// layer may only fail for TRANSIENT reasons. It must never refuse on a
// structural ground the capability already admitted, because that makes
// can_x() a lie: the capability reports supported and the operation returns
// nothing.
//
// That exact divergence occurred four times while planar support was added,
// once in each place these facts were independently re-derived. Independent
// re-derivation from the same data is the defect; these functions remove it.

// The retained payload belongs to the current frame and describes the same
// image the result reports. Shared precondition, no format opinion.
inline bool retained_cpu_bytes_are_current(const CoreStreamResultData& result) noexcept {
  if (result.payload_retained_frame_id == 0 ||
      result.payload_retained_frame_id != result.retained_frame_id) {
    return false;
  }
  if (result.payload.width == 0 || result.payload.height == 0 || result.payload.empty()) {
    return false;
  }
  return result.payload.width == result.image_width &&
         result.payload.height == result.image_height &&
         result.payload.format_fourcc == result.image_format_fourcc;
}

// Directly readable as packed RGBA/BGRA: required by any path that hands the
// retained bytes to a FORMAT_RGBA8 image without converting them.
inline bool retained_cpu_payload_is_packed_readable(const CoreResultPayloadCpu& payload) noexcept {
  if (payload.empty() || !is_packed_rgb_format(payload.format_fourcc)) {
    return false;
  }
  const PixelFormatDescriptor desc = describe_pixel_format(payload.format_fourcc);
  const size_t expected = min_tight_size_bytes(desc, payload.width, payload.height);
  return expected != 0 &&
         payload.stride_bytes == plane_row_bytes(desc, 0, payload.width) &&
         payload.size_bytes() >= expected;
}

// Usable by a path that CONVERTS rather than reading bytes as-is: the live
// display view and explicit to_image() materialization both qualify. A planar
// payload has no directly readable form but does have a supported conversion.
inline bool retained_cpu_payload_is_convertible(const CoreResultPayloadCpu& payload) noexcept {
  if (retained_cpu_payload_is_packed_readable(payload)) {
    return true;
  }
  if (payload.empty() || !payload.is_planar()) {
    return false;
  }
  const PixelFormatDescriptor desc = describe_pixel_format(payload.format_fourcc);
  if (!desc.valid || payload.plane_count != desc.plane_count) {
    return false;
  }
  if (desc.is_yuv && !is_convertible_colorimetry(payload.colorimetry)) {
    return false;
  }
  for (uint32_t plane = 0; plane < payload.plane_count; ++plane) {
    if (payload.plane_data(plane) == nullptr) {
      return false;
    }
  }
  return true;
}

// Operation Support for a retained stream result, derived from the
// admissibility predicates above so a reported capability and the path that
// implements it cannot disagree. Inline and pure, so the agreement itself is
// directly testable.
inline CoreRetainedAccessTruth build_stream_retained_access_truth(const CoreStreamResultData& result);

// Result-level convenience: currency plus the corresponding payload question.
inline bool stream_result_has_packed_cpu_access(const CoreStreamResultData& result) noexcept {
  return retained_cpu_bytes_are_current(result) &&
         retained_cpu_payload_is_packed_readable(result.payload);
}

inline bool stream_result_has_convertible_cpu_access(const CoreStreamResultData& result) noexcept {
  return retained_cpu_bytes_are_current(result) &&
         retained_cpu_payload_is_convertible(result.payload);
}

inline CoreRetainedAccessTruth build_stream_retained_access_truth(const CoreStreamResultData& result) {
  CoreRetainedAccessTruth truth{};
  const bool has_current_cpu_payload = stream_result_has_packed_cpu_access(result);

  if (result.payload_kind == ResultPayloadKind::GPU_SURFACE) {
    if (result.retained_gpu_backing) {
      truth.display_view = ResultCapability::READY;
    }
    if (has_current_cpu_payload) {
      truth.to_image = ResultCapability::CHEAP;
    } else if (result.retained_gpu_backing &&
               result.retained_gpu_backing_descriptor.valid &&
               result.retained_gpu_backing_descriptor.materialization_available) {
      truth.to_image = ResultCapability::EXPENSIVE;
    }
    return truth;
  }

  if (result.payload_kind == ResultPayloadKind::CPU_PACKED && has_current_cpu_payload) {
    truth.display_view = ResultCapability::CHEAP;
    truth.to_image = ResultCapability::CHEAP;
  }

  if (result.payload_kind == ResultPayloadKind::CPU_PLANAR &&
      stream_result_has_convertible_cpu_access(result)) {
    // Never READY: the target representation is not already retained, it is
    // produced by a full-frame colour conversion (on CPU here, on GPU where a
    // RenderingDevice exists). EXPENSIVE is the provisional classification
    // per the conversion example in the capability vocabulary; bounded
    // calibration refines CHEAP vs EXPENSIVE from measured evidence.
    truth.display_view = ResultCapability::EXPENSIVE;
    // Materialization is the same full-frame conversion, performed on demand
    // and driven by the same shared colorimetry, so it is supported and
    // equally non-ready.
    truth.to_image = ResultCapability::EXPENSIVE;
  }
  return truth;
}


// Threading model note (applies also to CoreCaptureAssemblyRegistry and
// CoreCaptureCohortRegistry): most CoreRuntime-owned registries are
// core-thread-only with no internal lock, relying solely on CoreThread's
// single-owner invariant; any cross-thread visibility goes through a
// snapshot copy built while running on the core thread. This store is a
// deliberate, narrow exception: CoreRuntime::get_capture_result[_set]() are
// read directly and synchronously from the calling (e.g. Godot) thread with
// no core-thread round trip, so this store must be safe for concurrent
// core-thread writes and external-thread reads and provides its own
// internal locking accordingly.
class CoreResultStore final {
public:
  enum class DisplayDemandReason : uint8_t {
    NONE = 0,
    PERSISTENT_REFCOUNT = 1,
    LEASE = 2,
  };
  struct DisplayDemandState {
    bool active = false;
    DisplayDemandReason reason = DisplayDemandReason::NONE;
    uint32_t refcount = 0;
  };

  CoreResultStore() = default;
  ~CoreResultStore() = default;

  bool retain_frame(const FrameView& frame,
                    std::optional<StreamIntent> stream_intent,
                    uint64_t stream_applied_access_posture_epoch = 0,
                    uint64_t capture_applied_access_posture_epoch = 0,
                    CoreRetainedProductionPlan stream_requested_retained_plan = {},
                    CoreRetainedProductionPlan capture_requested_retained_plan = {});
  bool append_additional_capture_image(uint64_t capture_id,
                                       uint64_t device_instance_id,
                                       CoreCaptureResultData::ImageMemberData image_member,
                                       uint64_t capture_applied_access_posture_epoch = 0,
                                       CoreRetainedProductionPlan capture_requested_retained_plan = {});
  bool finalize_capture_facts(
      uint64_t capture_id,
      uint64_t device_instance_id,
      std::optional<CaptureAdmissionContext> admission_context,
      const std::function<CoreResolvedCaptureImageFacts(uint32_t image_member_index)>&
          resolve_image_facts);
  static bool try_build_capture_image_member_data_from_frame(
      const FrameView& frame,
      CoreCaptureResultData::ImageMemberData& out_member,
      CoreRetainedProductionPlan requested_retained_plan = {});
  static bool try_build_capture_image_member_data_from_frame(const FrameView& frame,
                                                              CoreResultPayloadCpu& out_payload);

  SharedStreamResultData get_latest_stream_result(uint64_t stream_id) const;
  SharedCaptureResultData get_capture_result(uint64_t capture_id, uint64_t device_instance_id) const;
  std::vector<SharedCaptureResultData> get_capture_result_set(uint64_t capture_id) const;
  void remove_stream_result(uint64_t stream_id);

  // Retention (unbounded-growth fix, ledger #52); mirrors
  // CoreCaptureAssemblyRegistry::retire_terminal_older_than() -- see that
  // header's doc comment for why retirement is time-based only (not tied to
  // supersession by a newer capture or to device close; both were tried and
  // each broke a real, deliberate provider_compliance_verify check), and the
  // note that already-issued CamBANGCaptureResult wrappers are unaffected
  // (they hold their own independent SharedCaptureResultData reference).
  // Called once per entry CoreRuntime retires via
  // CoreCaptureAssemblyRegistry::retire_terminal_older_than().
  void remove_capture_result(uint64_t capture_id, uint64_t device_instance_id);

  struct EvictedCaptureResult {
    uint64_t capture_id = 0;
    uint64_t device_instance_id = 0;
  };
  // Byte-budget retention (ledger #53), complementary to the time-based
  // retire_terminal_older_than() sweep above: bounds total retained capture
  // bytes (literal CPU-packed bytes plus an *estimated* GPU-backed footprint
  // derived only from the Core-visible RetainedGpuBackingDescriptor, never
  // the opaque retained_gpu_backing handle itself) so a long-running,
  // multi-camera repeated-capture session cannot grow result memory without
  // bound even while individual captures remain within the time window.
  // Walks capture_results_by_capture_id_ in ascending capture_id order
  // (already oldest-first), skipping any (capture_id, device_instance_id)
  // for which is_evictable returns false, evicting the rest until
  // at-or-under byte_budget or no evictable candidates remain. The caller
  // (CoreRuntime) must supply is_evictable backed by
  // CoreCaptureAssemblyRegistry::terminal_capture_device_pairs() so a
  // still-in-flight capture (its result data actively being appended to /
  // finalized by the capture pipeline) is never evicted merely for being
  // byte-heavy or old -- only already-terminal (COMPLETED/FAILED) entries
  // are eligible, mirroring retire_terminal_older_than()'s own restriction.
  //
  // is_evictable MUST NOT itself acquire any other registry's lock: it runs
  // while mutex_ is held, and CoreCaptureAssemblyRegistry::mutex_ (or any
  // other registry lock) must never be nested inside mutex_ here -- every
  // cross-registry access elsewhere in this codebase locks registries
  // sequentially, never nested, and this predicate must keep that invariant
  // (build it from a snapshot taken before calling in, e.g.
  // terminal_capture_device_pairs(), not a live cross-registry query).
  // Evicted entries are moved into the returned vector while mutex_ is
  // held, and the lock is released before that vector (and the payloads/
  // GPU backing shared_ptrs it holds) is destroyed, so freeing memory never
  // blocks a concurrent get_latest_stream_result()/get_capture_result() read.
  std::vector<EvictedCaptureResult> evict_over_byte_budget(
      uint64_t byte_budget,
      const std::function<bool(uint64_t capture_id, uint64_t device_instance_id)>& is_evictable);
  uint64_t total_estimated_capture_bytes() const;

  void mark_stream_display_demand(uint64_t stream_id, uint64_t now_ns);
  void retain_stream_display_demand(uint64_t stream_id);
  void release_stream_display_demand(uint64_t stream_id);
  bool is_stream_display_demand_active(uint64_t stream_id, uint64_t now_ns) const;
  DisplayDemandState get_stream_display_demand_state(uint64_t stream_id, uint64_t now_ns) const;
  void clear();

private:
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
  friend struct CoreResultStoreSmokeAccess;
#endif
  static bool has_cpu_payload(const FrameView& frame);
  // Retains a frame's CPU bytes, dispatching on the format's layout class.
  static bool try_copy_cpu_payload(const FrameView& frame, CoreResultPayloadCpu& out);
  static bool try_copy_cpu_packed_payload(const FrameView& frame, CoreResultPayloadCpu& out);
  static bool try_copy_cpu_planar_payload(const FrameView& frame, CoreResultPayloadCpu& out);
  static bool has_valid_capture_image_member_payload(const CoreResultPayloadCpu& payload);
  bool try_issue_retained_frame_id(uint64_t& out_id) noexcept;
  static MutableCaptureResultData build_default_image_capture_result(const FrameView& frame,
                                                                     CoreRetainedBackingPlan plan,
                                                                     CoreResultPayloadCpu payload,
                                                                     std::shared_ptr<void> retained_gpu_backing,
                                                                     RetainedGpuBackingDescriptor retained_gpu_backing_descriptor);

  mutable std::mutex mutex_;
  std::map<uint64_t, SharedStreamResultData> latest_stream_results_;
  std::map<uint64_t, std::map<uint64_t, MutableCaptureResultData>> capture_results_by_capture_id_;
  // Running total of compute_capture_result_bytes() across every entry
  // currently in capture_results_by_capture_id_; kept incrementally in sync
  // under mutex_ by retain_frame()/append_additional_capture_image()
  // (add)/remove_capture_result()/evict_over_byte_budget() (subtract) so
  // evict_over_byte_budget() never has to re-walk+recompute the whole store.
  uint64_t total_estimated_capture_bytes_ = 0;
  uint64_t next_retained_frame_id_ = 1;
  struct StreamAccessPostureDomainKey {
    uint64_t stream_id = 0;
    uint64_t applied_epoch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format_fourcc = 0;
    ResultPayloadKind payload_kind = ResultPayloadKind::CPU_PACKED;
    bool has_retained_cpu_payload = false;
    bool has_retained_gpu_backing = false;
    bool gpu_materialization_available = false;
    bool gpu_materialization_requires_readback = false;

    bool operator<(const StreamAccessPostureDomainKey& other) const noexcept;
  };

  struct CaptureAccessPostureDomainKey {
    uint64_t device_instance_id = 0;
    uint64_t applied_epoch = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format_fourcc = 0;
    ResultPayloadKind payload_kind = ResultPayloadKind::CPU_PACKED;
    bool has_retained_cpu_payload = false;
    bool has_retained_gpu_backing = false;
    bool gpu_materialization_available = false;
    bool gpu_materialization_requires_readback = false;

    bool operator<(const CaptureAccessPostureDomainKey& other) const noexcept;
  };

  uint64_t resolve_stream_access_posture_id(const CoreStreamResultData& result,
                                            bool has_current_cpu_payload,
                                            uint64_t applied_epoch);
  uint64_t resolve_capture_member_access_posture_id(uint64_t device_instance_id,
                                                    const CoreCaptureResultData::ImageMemberData& member,
                                                    bool has_cpu_payload,
                                                    uint64_t applied_epoch);

  std::map<uint64_t, uint64_t> stream_display_demand_last_seen_ns_;
  std::map<uint64_t, uint32_t> stream_display_demand_refcounts_;
  std::map<StreamAccessPostureDomainKey, uint64_t> stream_access_posture_ids_;
  std::map<CaptureAccessPostureDomainKey, uint64_t> capture_access_posture_ids_;
  uint64_t next_result_access_posture_id_ = 1;
};

} // namespace cambang
