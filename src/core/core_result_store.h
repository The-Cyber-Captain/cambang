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

struct CoreResultPayloadCpuPacked {
  uint32_t format_fourcc = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
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

  bool has_capture_attributes = false;
  ResultCaptureAttributesFacts capture_attributes{};
  ResultCaptureAttributesProvenance capture_attributes_provenance{};

  bool has_location_attributes = false;
  ResultLocationAttributesFacts location_attributes{};
  ResultLocationAttributesProvenance location_attributes_provenance{};

  bool has_optical_calibration = false;
  ResultOpticalCalibrationFacts optical_calibration{};
  ResultOpticalCalibrationProvenance optical_calibration_provenance{};
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
  CoreResultPayloadCpuPacked payload{};
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
    CoreResultPayloadCpuPacked payload{};
    std::shared_ptr<void> retained_gpu_backing{};
    RetainedGpuBackingDescriptor retained_gpu_backing_descriptor{};
    CoreRetainedAccessTruth retained_access_truth{};
    SharedResultAccessClassificationRecord access_classification{};
    CoreResultAccessPostureKey access_posture{};

    bool has_capture_attributes = false;
    ResultCaptureAttributesFacts capture_attributes{};
    ResultCaptureAttributesProvenance capture_attributes_provenance{};

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

class CoreResultStore final {
public:
  using DisplayDemandLeaseToken = uint64_t;

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
                                                              CoreResultPayloadCpuPacked& out_payload);

  SharedStreamResultData get_latest_stream_result(uint64_t stream_id) const;
  SharedCaptureResultData get_capture_result(uint64_t capture_id, uint64_t device_instance_id) const;
  std::vector<SharedCaptureResultData> get_capture_result_set(uint64_t capture_id) const;
  void remove_stream_result(uint64_t stream_id);
  void mark_stream_display_demand(uint64_t stream_id, uint64_t now_ns);
  DisplayDemandLeaseToken retain_stream_display_demand_lease(uint64_t stream_id) noexcept;
  bool release_stream_display_demand_lease(DisplayDemandLeaseToken token) noexcept;
  bool is_stream_display_demand_active(uint64_t stream_id, uint64_t now_ns) const;
  DisplayDemandState get_stream_display_demand_state(uint64_t stream_id, uint64_t now_ns) const;
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
  uint32_t stream_display_demand_refcount_for_smoke(uint64_t stream_id) const noexcept;
#endif
  void clear();

private:
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
  friend struct CoreResultStoreSmokeAccess;
#endif
  static bool has_cpu_packed_payload(const FrameView& frame);
  static bool try_copy_cpu_packed_payload(const FrameView& frame, CoreResultPayloadCpuPacked& out);
  static bool has_valid_capture_image_member_payload(const CoreResultPayloadCpuPacked& payload);
  bool try_issue_retained_frame_id(uint64_t& out_id) noexcept;
  static MutableCaptureResultData build_default_image_capture_result(const FrameView& frame,
                                                                     CoreRetainedBackingPlan plan,
                                                                     CoreResultPayloadCpuPacked payload,
                                                                     std::shared_ptr<void> retained_gpu_backing,
                                                                     RetainedGpuBackingDescriptor retained_gpu_backing_descriptor);

  mutable std::mutex mutex_;
  std::map<uint64_t, SharedStreamResultData> latest_stream_results_;
  std::map<uint64_t, std::map<uint64_t, MutableCaptureResultData>> capture_results_by_capture_id_;
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
  std::map<DisplayDemandLeaseToken, uint64_t> stream_display_demand_leases_;
  DisplayDemandLeaseToken next_display_demand_lease_token_ = 1;
  std::map<StreamAccessPostureDomainKey, uint64_t> stream_access_posture_ids_;
  std::map<CaptureAccessPostureDomainKey, uint64_t> capture_access_posture_ids_;
  uint64_t next_result_access_posture_id_ = 1;
};

} // namespace cambang
