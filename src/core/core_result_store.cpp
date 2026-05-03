#include "core/core_result_store.h"

#include <cstring>
#include <limits>

namespace cambang {

namespace {
constexpr uint64_t kDisplayDemandLeaseNs = 250'000'000ull;

bool checked_mul_size_t(size_t a, size_t b, size_t& out) {
  if (a != 0 && b > (std::numeric_limits<size_t>::max() / a)) {
    return false;
  }
  out = a * b;
  return true;
}

bool checked_add_size_t(size_t a, size_t b, size_t& out) {
  if (b > (std::numeric_limits<size_t>::max() - a)) {
    return false;
  }
  out = a + b;
  return true;
}

uint32_t infer_bit_depth(uint32_t format_fourcc) {
  if (format_fourcc == FOURCC_RGBA || format_fourcc == FOURCC_BGRA) {
    return 8;
  }
  return 0;
}

CoreImageFactBundle build_default_facts(uint32_t width, uint32_t height, uint32_t format_fourcc) {
  CoreImageFactBundle facts{};
  facts.has_image_properties = true;
  facts.image_properties.width = width;
  facts.image_properties.height = height;
  facts.image_properties.format = format_fourcc;
  facts.image_properties.orientation = 0;
  facts.image_properties.bit_depth = infer_bit_depth(format_fourcc);

  facts.image_properties_provenance.width = ResultFactProvenance::HARDWARE_REPORTED;
  facts.image_properties_provenance.height = ResultFactProvenance::HARDWARE_REPORTED;
  facts.image_properties_provenance.format = ResultFactProvenance::HARDWARE_REPORTED;
  facts.image_properties_provenance.orientation = ResultFactProvenance::UNKNOWN;
  facts.image_properties_provenance.bit_depth = ResultFactProvenance::PROVIDER_DERIVED;
  return facts;
}

bool has_valid_result_image_description(const FrameView& frame) {
  if (frame.width == 0 || frame.height == 0) {
    return false;
  }
  return frame.format_fourcc == FOURCC_RGBA || frame.format_fourcc == FOURCC_BGRA;
}

} // namespace

bool CoreResultStore::retain_frame(const FrameView& frame,
                                   std::optional<StreamIntent> stream_intent,
                                   uint64_t capture_timestamp_ns) {
  const bool has_cpu_payload = has_cpu_packed_payload(frame);
  CoreResultPayloadCpuPacked payload{};
  CoreImageFactBundle facts{};
  if (has_cpu_payload) {
    if (!try_copy_cpu_packed_payload(frame, payload)) {
      return false;
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);

  if (frame.stream_id != 0) {
    if (!has_valid_result_image_description(frame)) {
      return false;
    }
    const bool gpu_primary =
        frame.primary_backing_kind == ProducerBackingKind::GPU &&
        static_cast<bool>(frame.primary_backing_artifact);
    if (!gpu_primary && !has_cpu_payload) {
      return false;
    }
    std::shared_ptr<void> retained_gpu_backing = frame.primary_backing_artifact;

    auto stream_result = std::make_shared<CoreStreamResultData>();
    stream_result->stream_id = frame.stream_id;
    stream_result->device_instance_id = frame.device_instance_id;
    stream_result->intent = stream_intent.value_or(StreamIntent::PREVIEW);
    stream_result->capture_timestamp_ns = capture_timestamp_ns;
    stream_result->image_width = frame.width;
    stream_result->image_height = frame.height;
    stream_result->image_format_fourcc = frame.format_fourcc;
    stream_result->payload_kind = retained_gpu_backing
        ? ResultPayloadKind::GPU_SURFACE
        : ResultPayloadKind::CPU_PACKED;
    stream_result->retained_gpu_backing = std::move(retained_gpu_backing);
    stream_result->payload = payload;
    facts = build_default_facts(frame.width, frame.height, frame.format_fourcc);
    stream_result->facts = facts;
    latest_stream_results_[frame.stream_id] = std::move(stream_result);
  }

  if (frame.capture_id != 0) {
    if (!has_cpu_payload) {
      return false;
    }
    auto capture_result = std::make_shared<CoreCaptureResultData>();
    capture_result->capture_id = frame.capture_id;
    capture_result->device_instance_id = frame.device_instance_id;
    capture_result->capture_timestamp_ns = capture_timestamp_ns;
    capture_result->payload_kind = ResultPayloadKind::CPU_PACKED;
    capture_result->payload = payload;
    facts = build_default_facts(payload.width, payload.height, payload.format_fourcc);
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

void CoreResultStore::remove_stream_result(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  latest_stream_results_.erase(stream_id);
  stream_display_demand_last_seen_ns_.erase(stream_id);
}

void CoreResultStore::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  latest_stream_results_.clear();
  capture_results_by_capture_id_.clear();
  stream_display_demand_last_seen_ns_.clear();
}

void CoreResultStore::mark_stream_display_demand(uint64_t stream_id, uint64_t now_ns) {
  if (stream_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  stream_display_demand_last_seen_ns_[stream_id] = now_ns;
}

bool CoreResultStore::is_stream_display_demand_active(uint64_t stream_id, uint64_t now_ns) const {
  if (stream_id == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = stream_display_demand_last_seen_ns_.find(stream_id);
  if (it == stream_display_demand_last_seen_ns_.end()) {
    return false;
  }
  const uint64_t last_seen_ns = it->second;
  if (now_ns < last_seen_ns) {
    return true;
  }
  return (now_ns - last_seen_ns) <= kDisplayDemandLeaseNs;
}

bool CoreResultStore::try_copy_cpu_packed_payload(const FrameView& frame, CoreResultPayloadCpuPacked& out) {
  if (!has_cpu_packed_payload(frame)) {
    return false;
  }

  if (!(frame.format_fourcc == FOURCC_RGBA || frame.format_fourcc == FOURCC_BGRA)) {
    return false;
  }
  if (frame.width > (std::numeric_limits<uint32_t>::max() / 4u)) {
    return false;
  }

  size_t row_bytes = 0;
  if (!checked_mul_size_t(static_cast<size_t>(frame.width), 4u, row_bytes)) {
    return false;
  }
  const size_t src_stride = (frame.stride_bytes == 0) ? row_bytes : static_cast<size_t>(frame.stride_bytes);
  if (src_stride < row_bytes) {
    return false;
  }
  const size_t h = static_cast<size_t>(frame.height);
  size_t stride_span = 0;
  if (h > 1u && !checked_mul_size_t(h - 1u, src_stride, stride_span)) {
    return false;
  }
  size_t needed = 0;
  if (!checked_add_size_t(stride_span, row_bytes, needed) || frame.size_bytes < needed) {
    return false;
  }

  size_t dst_size = 0;
  if (!checked_mul_size_t(row_bytes, h, dst_size)) {
    return false;
  }

  out.format_fourcc = frame.format_fourcc;
  out.width = frame.width;
  out.height = frame.height;
  out.stride_bytes = static_cast<uint32_t>(row_bytes);
  if (dst_size > out.bytes.max_size()) {
    return false;
  }
  out.bytes.resize(dst_size);

  const uint8_t* src = frame.data;
  uint8_t* dst = out.bytes.data();
  for (size_t y = 0; y < h; ++y) {
    std::memcpy(dst, src, row_bytes);
    src += src_stride;
    dst += row_bytes;
  }

  return true;
}

bool CoreResultStore::has_cpu_packed_payload(const FrameView& frame) {
  return frame.width != 0 &&
         frame.height != 0 &&
         frame.data != nullptr &&
         frame.size_bytes != 0;
}

} // namespace cambang
