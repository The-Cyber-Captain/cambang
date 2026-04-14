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
  std::shared_ptr<void> retained_gpu_backing{};
  CoreResultPayloadCpuPacked payload{};
  CoreImageFactBundle facts{};
};

struct CoreCaptureResultData {
  uint64_t capture_id = 0;
  uint64_t device_instance_id = 0;
  uint64_t capture_timestamp_ns = 0;
  ResultPayloadKind payload_kind = ResultPayloadKind::CPU_PACKED;
  CoreResultPayloadCpuPacked payload{};
  CoreImageFactBundle facts{};
};

using SharedStreamResultData = std::shared_ptr<const CoreStreamResultData>;
using SharedCaptureResultData = std::shared_ptr<const CoreCaptureResultData>;

class CoreResultStore final {
public:
  CoreResultStore() = default;
  ~CoreResultStore() = default;

  bool retain_frame(const FrameView& frame,
                    std::optional<StreamIntent> stream_intent,
                    uint64_t capture_timestamp_ns);

  SharedStreamResultData get_latest_stream_result(uint64_t stream_id) const;
  SharedCaptureResultData get_capture_result(uint64_t capture_id, uint64_t device_instance_id) const;
  std::vector<SharedCaptureResultData> get_capture_result_set(uint64_t capture_id) const;
  void clear();

private:
  static bool has_cpu_packed_payload(const FrameView& frame);
  static bool try_copy_cpu_packed_payload(const FrameView& frame, CoreResultPayloadCpuPacked& out);

  mutable std::mutex mutex_;
  std::map<uint64_t, SharedStreamResultData> latest_stream_results_;
  std::map<uint64_t, std::map<uint64_t, SharedCaptureResultData>> capture_results_by_capture_id_;
};

} // namespace cambang
