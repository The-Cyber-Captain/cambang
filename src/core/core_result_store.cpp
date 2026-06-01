#include "core/core_result_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace cambang {

namespace {
constexpr uint64_t kDisplayDemandLeaseNs = 250'000'000ull;
bool display_demand_trace_enabled() {
  const char* v = std::getenv("CAMBANG_DEV_DISPLAY_DEMAND_TRACE");
  return v && v[0] != '\0' && v[0] != '0';
}

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

RetainedGpuBackingDescriptor build_retained_gpu_backing_descriptor(
    const FrameView& frame,
    uint64_t capture_timestamp_ns,
    bool gpu_primary) {
  RetainedGpuBackingDescriptor descriptor = frame.retained_gpu_backing_descriptor;
  if (descriptor.valid) {
    // The provider owns identity/capability truth; core only normalizes
    // correlation and image fields that are already present on this FrameView.
    descriptor.stream_id = frame.stream_id;
    descriptor.capture_timestamp_ns = capture_timestamp_ns;
    descriptor.width = frame.width;
    descriptor.height = frame.height;
    descriptor.stride_bytes = frame.stride_bytes;
    descriptor.format_fourcc = frame.format_fourcc;
    return descriptor;
  }

  if (!gpu_primary) {
    return descriptor;
  }

  // Compatibility scaffold for legacy providers that expose a GPU primary
  // artifact before they provide neutral descriptor metadata. Zero backing_id
  // explicitly means no scalar provider identity/generation was supplied.
  descriptor.valid = true;
  descriptor.stream_id = frame.stream_id;
  descriptor.backing_id = 0;
  descriptor.capture_timestamp_ns = capture_timestamp_ns;
  descriptor.width = frame.width;
  descriptor.height = frame.height;
  descriptor.stride_bytes = frame.stride_bytes;
  descriptor.format_fourcc = frame.format_fourcc;
  descriptor.display_available = static_cast<bool>(frame.primary_backing_artifact);
  descriptor.materialization_available = false;
  descriptor.materialization_requires_gpu_readback = false;
  return descriptor;
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

  SharedStreamResultData replaced_stream_result;
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
    RetainedGpuBackingDescriptor retained_gpu_backing_descriptor =
        build_retained_gpu_backing_descriptor(frame, capture_timestamp_ns, gpu_primary);

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
    stream_result->retained_gpu_backing_descriptor = retained_gpu_backing_descriptor;
    stream_result->payload = payload;
    if (has_cpu_payload) {
      stream_result->payload_capture_timestamp_ns = capture_timestamp_ns;
    }
    facts = build_default_facts(frame.width, frame.height, frame.format_fourcc);
    stream_result->facts = facts;
    auto& slot = latest_stream_results_[frame.stream_id];
    replaced_stream_result = std::move(slot);
    slot = std::move(stream_result);
  }

  if (frame.capture_id != 0) {
    if (!has_cpu_payload) {
      return false;
    }
    SharedCaptureResultData capture_result =
        build_default_image_capture_result(frame, payload, capture_timestamp_ns);
    capture_results_by_capture_id_[frame.capture_id][frame.device_instance_id] = std::move(capture_result);
  }
  return frame.stream_id != 0 || frame.capture_id != 0;
}

bool CoreResultStore::append_additional_capture_image(
    uint64_t capture_id,
    uint64_t device_instance_id,
    CoreCaptureResultData::ImageMemberData image_member) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto cap_it = capture_results_by_capture_id_.find(capture_id);
  if (cap_it == capture_results_by_capture_id_.end()) {
    return false;
  }
  auto dev_it = cap_it->second.find(device_instance_id);
  if (dev_it == cap_it->second.end() || !dev_it->second) {
    return false;
  }
  if (!has_valid_capture_image_member_payload(image_member.payload)) {
    return false;
  }
  if (image_member.role != CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET) {
    return false;
  }
  if (image_member.image_member_index == 0u) {
    return false;
  }

  auto updated = std::make_shared<CoreCaptureResultData>(*dev_it->second);
  const uint32_t expected_member_index =
      1u + static_cast<uint32_t>(updated->additional_images.size());
  if (image_member.image_member_index != expected_member_index) {
    return false;
  }
  updated->additional_images.push_back(std::move(image_member));
  dev_it->second = std::move(updated);
  return true;
}

bool CoreResultStore::try_build_capture_image_member_data_from_frame(const FrameView& frame,
                                                                     CoreResultPayloadCpuPacked& out_payload) {
  if (!has_cpu_packed_payload(frame)) {
    return false;
  }
  if (!try_copy_cpu_packed_payload(frame, out_payload)) {
    return false;
  }
  return has_valid_capture_image_member_payload(out_payload);
}

SharedCaptureResultData CoreResultStore::build_default_image_capture_result(
    const FrameView& frame,
    const CoreResultPayloadCpuPacked& payload,
    uint64_t capture_timestamp_ns) {
  if (frame.capture_image.routing != CaptureImageRouting::DEFAULT_METERED ||
      frame.capture_image.image_member_index != 0u) {
    return nullptr;
  }
  auto capture_result = std::make_shared<CoreCaptureResultData>();
  capture_result->capture_id = frame.capture_id;
  capture_result->device_instance_id = frame.device_instance_id;
  capture_result->image_width = payload.width;
  capture_result->image_height = payload.height;
  capture_result->image_format_fourcc = payload.format_fourcc;
  capture_result->payload_kind = ResultPayloadKind::CPU_PACKED;

  // Current default-only still-capture behavior: retained still payload is
  // accepted as the CaptureResult default image.
  capture_result->default_image.image_member_index = 0;
  capture_result->default_image.role = CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED;
  capture_result->default_image.applied_exposure_compensation_milli_ev =
      frame.capture_image.applied_exposure_compensation_milli_ev;
  capture_result->default_image.has_realized_exposure_compensation_milli_ev =
      frame.capture_image.has_realized_exposure_compensation_milli_ev;
  capture_result->default_image.realized_exposure_compensation_milli_ev =
      frame.capture_image.realized_exposure_compensation_milli_ev;
  if (capture_result->default_image.applied_exposure_compensation_milli_ev != 0) {
    return nullptr;
  }
  capture_result->default_image.capture_timestamp_ns = capture_timestamp_ns;
  capture_result->default_image.payload = payload;

  CoreImageFactBundle facts = build_default_facts(payload.width, payload.height, payload.format_fourcc);
  facts.has_capture_attributes = false;
  facts.capture_attributes = ResultCaptureAttributesFacts{};
  facts.capture_attributes_provenance = ResultCaptureAttributesProvenance{};
  capture_result->facts = facts;
  return capture_result;
}

bool CoreResultStore::has_valid_capture_image_member_payload(const CoreResultPayloadCpuPacked& payload) {
  if (payload.width == 0 || payload.height == 0) {
    return false;
  }
  if (payload.format_fourcc != FOURCC_RGBA && payload.format_fourcc != FOURCC_BGRA) {
    return false;
  }
  if (payload.bytes.empty()) {
    return false;
  }
  return true;
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
  SharedStreamResultData removed_stream_result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = latest_stream_results_.find(stream_id);
    if (it != latest_stream_results_.end()) {
      removed_stream_result = std::move(it->second);
      latest_stream_results_.erase(it);
    }
    stream_display_demand_last_seen_ns_.erase(stream_id);
    stream_display_demand_refcounts_.erase(stream_id);
  }
}

void CoreResultStore::clear() {
  std::map<uint64_t, SharedStreamResultData> old_stream_results;
  std::map<uint64_t, std::map<uint64_t, SharedCaptureResultData>> old_capture_results;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    old_stream_results.swap(latest_stream_results_);
    old_capture_results.swap(capture_results_by_capture_id_);
    stream_display_demand_last_seen_ns_.clear();
    stream_display_demand_refcounts_.clear();
  }
}

void CoreResultStore::mark_stream_display_demand(uint64_t stream_id, uint64_t now_ns) {
  if (stream_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto result_it = latest_stream_results_.find(stream_id);
  if (result_it == latest_stream_results_.end()) {
    stream_display_demand_last_seen_ns_.erase(stream_id);
    return;
  }
  stream_display_demand_last_seen_ns_[stream_id] = now_ns;
}

void CoreResultStore::retain_stream_display_demand(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (latest_stream_results_.find(stream_id) == latest_stream_results_.end()) {
    return;
  }
  uint32_t& refs = stream_display_demand_refcounts_[stream_id];
  if (refs != std::numeric_limits<uint32_t>::max()) {
    refs += 1u;
  }
  if (display_demand_trace_enabled()) {
    std::printf("[CamBANG][DemandTrace] retain stream_id=%llu refcount=%u\n",
                static_cast<unsigned long long>(stream_id),
                refs);
  }
}

void CoreResultStore::release_stream_display_demand(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = stream_display_demand_refcounts_.find(stream_id);
  if (it == stream_display_demand_refcounts_.end()) {
    return;
  }
  if (it->second <= 1u) {
    if (display_demand_trace_enabled()) {
      std::printf("[CamBANG][DemandTrace] release stream_id=%llu refcount=0\n",
                  static_cast<unsigned long long>(stream_id));
    }
    stream_display_demand_refcounts_.erase(it);
    return;
  }
  it->second -= 1u;
  if (display_demand_trace_enabled()) {
    std::printf("[CamBANG][DemandTrace] release stream_id=%llu refcount=%u\n",
                static_cast<unsigned long long>(stream_id),
                it->second);
  }
}

bool CoreResultStore::is_stream_display_demand_active(uint64_t stream_id, uint64_t now_ns) const {
  return get_stream_display_demand_state(stream_id, now_ns).active;
}

CoreResultStore::DisplayDemandState CoreResultStore::get_stream_display_demand_state(
    uint64_t stream_id,
    uint64_t now_ns) const {
  DisplayDemandState state{};
  if (stream_id == 0) {
    return state;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto ref_it = stream_display_demand_refcounts_.find(stream_id);
  if (ref_it != stream_display_demand_refcounts_.end() && ref_it->second > 0u) {
    state.active = true;
    state.reason = DisplayDemandReason::PERSISTENT_REFCOUNT;
    state.refcount = ref_it->second;
    return state;
  }
  const auto it = stream_display_demand_last_seen_ns_.find(stream_id);
  if (it == stream_display_demand_last_seen_ns_.end()) {
    return state;
  }
  const uint64_t last_seen_ns = it->second;
  if (now_ns < last_seen_ns) {
    state.active = true;
    state.reason = DisplayDemandReason::LEASE;
    return state;
  }
  state.active = (now_ns - last_seen_ns) <= kDisplayDemandLeaseNs;
  state.reason = state.active ? DisplayDemandReason::LEASE : DisplayDemandReason::NONE;
  return state;
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
