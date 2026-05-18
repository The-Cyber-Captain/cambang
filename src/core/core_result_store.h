#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "core/result_fact_types.h"
#include "core/result_payload_kind.h"
#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

struct CoreResultPayloadCpuPacked {
  uint32_t format_fourcc = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride_bytes = 0;
  std::vector<uint8_t> bytes;
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

struct CoreStreamResultData {
  uint64_t stream_id = 0;
  uint64_t device_instance_id = 0;
  StreamIntent intent = StreamIntent::PREVIEW;
  uint64_t capture_timestamp_ns = 0;
  uint32_t image_width = 0;
  uint32_t image_height = 0;
  uint32_t image_format_fourcc = 0;
  ResultPayloadKind payload_kind = ResultPayloadKind::CPU_PACKED;
  // For stream paths this may reference stream-owned live backing updated in
  // place while flowing; this is display/live-state retention, not frozen
  // per-frame GPU artifact identity.
  std::shared_ptr<void> retained_gpu_backing{};
  CoreResultPayloadCpuPacked payload{};
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
    uint64_t capture_timestamp_ns = 0;
    CoreResultPayloadCpuPacked payload{};

    bool has_capture_attributes = false;
    ResultCaptureAttributesFacts capture_attributes{};
    ResultCaptureAttributesProvenance capture_attributes_provenance{};
  };

  uint64_t capture_id = 0;
  uint64_t device_instance_id = 0;

  uint32_t image_width = 0;
  uint32_t image_height = 0;
  uint32_t image_format_fourcc = 0;
  ResultPayloadKind payload_kind = ResultPayloadKind::CPU_PACKED;

  ImageMemberData default_image{};
  std::vector<ImageMemberData> additional_images{};

  CoreImageFactBundle facts{};
};

using SharedStreamResultData = std::shared_ptr<const CoreStreamResultData>;
using SharedCaptureResultData = std::shared_ptr<const CoreCaptureResultData>;

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
                    uint64_t capture_timestamp_ns);
  bool append_additional_capture_image(uint64_t capture_id,
                                       uint64_t device_instance_id,
                                       CoreCaptureResultData::ImageMemberData image_member);

  SharedStreamResultData get_latest_stream_result(uint64_t stream_id) const;
  SharedCaptureResultData get_capture_result(uint64_t capture_id, uint64_t device_instance_id) const;
  std::vector<SharedCaptureResultData> get_capture_result_set(uint64_t capture_id) const;
  void remove_stream_result(uint64_t stream_id);
  void mark_stream_display_demand(uint64_t stream_id, uint64_t now_ns);
  void retain_stream_display_demand(uint64_t stream_id);
  void release_stream_display_demand(uint64_t stream_id);
  bool is_stream_display_demand_active(uint64_t stream_id, uint64_t now_ns) const;
  DisplayDemandState get_stream_display_demand_state(uint64_t stream_id, uint64_t now_ns) const;
  void clear();

private:
  static bool has_cpu_packed_payload(const FrameView& frame);
  static bool try_copy_cpu_packed_payload(const FrameView& frame, CoreResultPayloadCpuPacked& out);
  static bool has_valid_capture_image_member_payload(const CoreResultPayloadCpuPacked& payload);
  static SharedCaptureResultData build_default_image_capture_result(const FrameView& frame,
                                                                    const CoreResultPayloadCpuPacked& payload,
                                                                    uint64_t capture_timestamp_ns);

  mutable std::mutex mutex_;
  std::map<uint64_t, SharedStreamResultData> latest_stream_results_;
  std::map<uint64_t, std::map<uint64_t, SharedCaptureResultData>> capture_results_by_capture_id_;
  std::map<uint64_t, uint64_t> stream_display_demand_last_seen_ns_;
  std::map<uint64_t, uint32_t> stream_display_demand_refcounts_;
};

} // namespace cambang
