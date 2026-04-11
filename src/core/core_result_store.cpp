#include "core/core_result_store.h"

#include <cstring>

namespace cambang {

namespace {
uint32_t infer_bit_depth(uint32_t format_fourcc) {
  if (format_fourcc == FOURCC_RGBA || format_fourcc == FOURCC_BGRA) {
    return 8;
  }
  return 0;
}

CoreImageFactBundle build_default_facts(const CoreResultPayloadCpuPacked& payload) {
  CoreImageFactBundle facts{};
  facts.has_image_properties = true;
  facts.image_properties.width = payload.width;
  facts.image_properties.height = payload.height;
  facts.image_properties.format = payload.format_fourcc;
  facts.image_properties.orientation = 0;
  facts.image_properties.bit_depth = infer_bit_depth(payload.format_fourcc);

  facts.image_properties_provenance.width = ResultFactProvenance::HARDWARE_REPORTED;
  facts.image_properties_provenance.height = ResultFactProvenance::HARDWARE_REPORTED;
  facts.image_properties_provenance.format = ResultFactProvenance::HARDWARE_REPORTED;
  facts.image_properties_provenance.orientation = ResultFactProvenance::UNKNOWN;
  facts.image_properties_provenance.bit_depth = ResultFactProvenance::PROVIDER_DERIVED;
  return facts;
}

} // namespace

bool CoreResultStore::retain_frame(const FrameView& frame,
                                   std::optional<StreamIntent> stream_intent,
                                   uint64_t capture_timestamp_ns) {
  CoreResultPayloadCpuPacked payload{};
  if (!try_copy_cpu_packed_payload(frame, payload)) {
    return false;
  }

  const CoreImageFactBundle facts = build_default_facts(payload);

  std::lock_guard<std::mutex> lock(mutex_);

  if (frame.stream_id != 0) {
    auto stream_result = std::make_shared<CoreStreamResultData>();
    stream_result->stream_id = frame.stream_id;
    stream_result->device_instance_id = frame.device_instance_id;
    stream_result->intent = stream_intent.value_or(StreamIntent::PREVIEW);
    stream_result->capture_timestamp_ns = capture_timestamp_ns;
    stream_result->payload_kind = ResultPayloadKind::CPU_PACKED;
    stream_result->payload = payload;
    stream_result->facts = facts;
    latest_stream_results_[frame.stream_id] = std::move(stream_result);
  }

  if (frame.capture_id != 0) {
    auto capture_result = std::make_shared<CoreCaptureResultData>();
    capture_result->capture_id = frame.capture_id;
    capture_result->device_instance_id = frame.device_instance_id;
    capture_result->capture_timestamp_ns = capture_timestamp_ns;
    capture_result->payload_kind = ResultPayloadKind::CPU_PACKED;
    capture_result->payload = payload;
    capture_result->facts = facts;
    capture_results_by_capture_id_[frame.capture_id][frame.device_instance_id] = std::move(capture_result);
  }
  return frame.stream_id != 0 || frame.capture_id != 0;
}

SharedStreamResultData CoreResultStore::get_latest_stream_result(uint64_t stream_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = latest_stream_results_.find(stream_id);
  if (it == latest_stream_results_.end()) {
    return nullptr;
  }
  return it->second;
}

SharedCaptureResultData CoreResultStore::get_capture_result(uint64_t capture_id, uint64_t device_instance_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto cap_it = capture_results_by_capture_id_.find(capture_id);
  if (cap_it == capture_results_by_capture_id_.end()) {
    return nullptr;
  }
  const auto dev_it = cap_it->second.find(device_instance_id);
  if (dev_it == cap_it->second.end()) {
    return nullptr;
  }
  return dev_it->second;
}

std::vector<SharedCaptureResultData> CoreResultStore::get_capture_result_set(uint64_t capture_id) const {
  std::vector<SharedCaptureResultData> out;
  std::lock_guard<std::mutex> lock(mutex_);
  const auto cap_it = capture_results_by_capture_id_.find(capture_id);
  if (cap_it == capture_results_by_capture_id_.end()) {
    return out;
  }
  out.reserve(cap_it->second.size());
  for (const auto& [_, result] : cap_it->second) {
    out.push_back(result);
  }
  return out;
}

void CoreResultStore::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  latest_stream_results_.clear();
  capture_results_by_capture_id_.clear();
}

bool CoreResultStore::try_copy_cpu_packed_payload(const FrameView& frame, CoreResultPayloadCpuPacked& out) {
  if (frame.width == 0 || frame.height == 0 || frame.data == nullptr || frame.size_bytes == 0) {
    return false;
  }

  if (!(frame.format_fourcc == FOURCC_RGBA || frame.format_fourcc == FOURCC_BGRA)) {
    return false;
  }

  const size_t row_bytes = static_cast<size_t>(frame.width) * 4u;
  const size_t src_stride = (frame.stride_bytes == 0) ? row_bytes : static_cast<size_t>(frame.stride_bytes);
  const size_t h = static_cast<size_t>(frame.height);
  const size_t needed = (h == 0) ? 0 : ((h - 1u) * src_stride + row_bytes);
  if (frame.size_bytes < needed) {
    return false;
  }

  out.format_fourcc = frame.format_fourcc;
  out.width = frame.width;
  out.height = frame.height;
  out.stride_bytes = static_cast<uint32_t>(row_bytes);
  out.bytes.resize(row_bytes * h);

  const uint8_t* src = frame.data;
  uint8_t* dst = out.bytes.data();
  for (uint32_t y = 0; y < frame.height; ++y) {
    std::memcpy(dst, src, row_bytes);
    src += src_stride;
    dst += row_bytes;
  }

  return true;
}

} // namespace cambang
