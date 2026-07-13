#include "core/core_result_store.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace cambang {

namespace capture_latency_trace_diagnostics {
inline uint32_t capture_inflight() noexcept { return 0u; }
inline uint32_t active_capture_count() noexcept { return 0u; }
inline void note_capture_admitted(uint32_t) noexcept {}
inline void note_capture_finished() noexcept {}
inline void reset_trace_group_seen() noexcept {}
inline void print_trace_group_seen_summary() noexcept {}
inline void print_line(const char*) noexcept {}
} // namespace capture_latency_trace_diagnostics

namespace {
// BEGIN TEMPORARY CAPTURE LATENCY DIAGNOSTICS
uint64_t capture_latency_trace_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void capture_latency_trace_printf(const char* format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  capture_latency_trace_diagnostics::print_line(buffer);
}
// END TEMPORARY CAPTURE LATENCY DIAGNOSTICS

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

bool has_valid_retained_cpu_packed_access_payload(
    const CoreResultPayloadCpuPacked& payload,
    uint32_t expected_width,
    uint32_t expected_height,
    uint32_t expected_format_fourcc) {
  if (payload.width == 0 || payload.height == 0 || payload.empty()) {
    return false;
  }
  if (payload.width != expected_width ||
      payload.height != expected_height ||
      payload.format_fourcc != expected_format_fourcc) {
    return false;
  }
  if (payload.format_fourcc != FOURCC_RGBA && payload.format_fourcc != FOURCC_BGRA) {
    return false;
  }
  const size_t expected_size =
      static_cast<size_t>(payload.width) * static_cast<size_t>(payload.height) * 4u;
  return payload.stride_bytes == payload.width * 4u &&
         payload.size_bytes() >= expected_size;
}

CoreRetainedAccessTruth build_stream_retained_access_truth(const CoreStreamResultData& result) {
  CoreRetainedAccessTruth truth{};
  const bool has_current_cpu_payload =
      result.payload_capture_timestamp_ns == result.capture_timestamp_ns &&
      has_valid_retained_cpu_packed_access_payload(
          result.payload, result.image_width, result.image_height, result.image_format_fourcc);

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
  return truth;
}

CoreRetainedAccessTruth build_capture_image_member_retained_access_truth(
    const CoreCaptureResultData::ImageMemberData& member) {
  CoreRetainedAccessTruth truth{};
  if (has_valid_retained_cpu_packed_access_payload(
          member.payload, member.payload.width, member.payload.height, member.payload.format_fourcc)) {
    truth.display_view = ResultCapability::CHEAP;
    truth.to_image = ResultCapability::CHEAP;
    return truth;
  }
  if (member.payload_kind == ResultPayloadKind::GPU_SURFACE &&
      member.retained_gpu_backing &&
      member.retained_gpu_backing_descriptor.valid &&
      member.retained_gpu_backing_descriptor.materialization_available) {
    truth.display_view = ResultCapability::EXPENSIVE;
    truth.to_image = ResultCapability::EXPENSIVE;
  }
  return truth;
}

CoreRetainedBackingPlan build_retained_backing_plan_from_requested(
    CoreRetainedProductionPlan requested,
    const FrameView& frame,
    bool has_cpu_payload) {
  CoreRetainedBackingPlan plan{};
  if (!requested.valid) {
    const bool gpu_primary =
        frame.primary_backing_kind == ProducerBackingKind::GPU &&
        static_cast<bool>(frame.primary_backing_artifact);
    if (gpu_primary) {
      plan.primary_kind = ResultPayloadKind::GPU_SURFACE;
      plan.retain_gpu_display = true;
      plan.retain_cpu_sidecar = has_cpu_payload && frame.retain_cpu_sidecar;
    }
    return plan;
  }

  if (requested.primary_gpu()) {
    plan.primary_kind = ResultPayloadKind::GPU_SURFACE;
    plan.retain_gpu_display = true;
    plan.retain_cpu_sidecar = requested.retain_cpu_sidecar() && has_cpu_payload;
  }
  return plan;
}

bool frame_matches_requested_retained_plan(
    const FrameView& frame,
    const CoreRetainedBackingPlan& plan,
    CoreRetainedProductionPlan requested,
    bool has_cpu_payload) noexcept {
  if (!requested.valid) {
    return true;
  }
  if (requested.primary_cpu()) {
    return frame.primary_backing_kind == ProducerBackingKind::CPU && has_cpu_payload;
  }
  if (frame.primary_backing_kind != ProducerBackingKind::GPU || !frame.primary_backing_artifact) {
    return false;
  }
  if (requested.retain_cpu_sidecar() && !has_cpu_payload) {
    return false;
  }
  if (!requested.retain_cpu_sidecar() && plan.retain_cpu_sidecar) {
    return false;
  }
  return true;
}


uint64_t next_posture_id(uint64_t& next_id) noexcept {
  const uint64_t id = next_id;
  if (next_id != std::numeric_limits<uint64_t>::max()) {
    ++next_id;
  }
  return id == 0 ? 1 : id;
}

CoreResultAccessPostureKey build_stream_access_posture_key(
    const CoreStreamResultData& result,
    bool has_current_cpu_payload,
    uint64_t posture_id) noexcept {
  CoreResultAccessPostureKey key{};
  key.posture_id = posture_id;
  key.stream_id = result.stream_id;
  key.device_instance_id = result.device_instance_id;
  key.width = result.image_width;
  key.height = result.image_height;
  key.format_fourcc = result.image_format_fourcc;
  key.payload_kind = result.payload_kind;
  key.has_retained_cpu_payload = has_current_cpu_payload;
  key.has_retained_gpu_backing = static_cast<bool>(result.retained_gpu_backing);
  key.gpu_materialization_available = result.retained_gpu_backing_descriptor.valid &&
                                      result.retained_gpu_backing_descriptor.materialization_available;
  key.gpu_materialization_requires_readback = result.retained_gpu_backing_descriptor.valid &&
                                             result.retained_gpu_backing_descriptor.materialization_requires_gpu_readback;
  return key;
}

CoreResultAccessPostureKey build_capture_member_access_posture_key(
    uint64_t capture_device_instance_id,
    const CoreCaptureResultData::ImageMemberData& member,
    bool has_cpu_payload,
    uint64_t posture_id) noexcept {
  CoreResultAccessPostureKey key{};
  key.posture_id = posture_id;
  key.stream_id = member.retained_gpu_backing_descriptor.valid ? member.retained_gpu_backing_descriptor.stream_id : 0;
  key.device_instance_id = capture_device_instance_id;
  key.width = member.payload.width != 0 ? member.payload.width : member.retained_gpu_backing_descriptor.width;
  key.height = member.payload.height != 0 ? member.payload.height : member.retained_gpu_backing_descriptor.height;
  key.format_fourcc = member.payload.format_fourcc != 0
      ? member.payload.format_fourcc
      : member.retained_gpu_backing_descriptor.format_fourcc;
  key.payload_kind = member.payload_kind;
  key.has_retained_cpu_payload = has_cpu_payload;
  key.has_retained_gpu_backing = static_cast<bool>(member.retained_gpu_backing);
  key.gpu_materialization_available = member.retained_gpu_backing_descriptor.valid &&
                                      member.retained_gpu_backing_descriptor.materialization_available;
  key.gpu_materialization_requires_readback = member.retained_gpu_backing_descriptor.valid &&
                                             member.retained_gpu_backing_descriptor.materialization_requires_gpu_readback;
  return key;
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

ResultCapability resolve_result_access_classification(
    ResultCapability provisional,
    const SharedResultAccessClassificationRecord& record,
    CoreResultAccessOperation operation) noexcept {
  if (provisional == ResultCapability::UNSUPPORTED ||
      provisional == ResultCapability::READY) {
    return provisional;
  }
  if (!record) {
    return provisional;
  }
  const std::atomic<int>* slot = nullptr;
  switch (operation) {
    case CoreResultAccessOperation::DISPLAY_VIEW:
      slot = &record->display_view;
      break;
    case CoreResultAccessOperation::TO_IMAGE:
      slot = &record->to_image;
      break;
    case CoreResultAccessOperation::ENCODED_BYTES:
      slot = &record->encoded_bytes;
      break;
  }
  if (!slot) {
    return provisional;
  }
  const int refined = slot->load(std::memory_order_acquire);
  if (refined < 0) {
    return provisional;
  }
  return static_cast<ResultCapability>(refined);
}

void refine_result_access_classification(
    const SharedResultAccessClassificationRecord& record,
    CoreResultAccessOperation operation,
    ResultCapability classification) noexcept {
  if (!record) {
    return;
  }
  std::atomic<int>* slot = nullptr;
  switch (operation) {
    case CoreResultAccessOperation::DISPLAY_VIEW:
      slot = &record->display_view;
      break;
    case CoreResultAccessOperation::TO_IMAGE:
      slot = &record->to_image;
      break;
    case CoreResultAccessOperation::ENCODED_BYTES:
      slot = &record->encoded_bytes;
      break;
  }
  if (!slot) {
    return;
  }
  slot->store(static_cast<int>(classification), std::memory_order_release);
}

ResultCapability classify_supported_non_ready_result_access_from_normalized_costs(
    ResultCapability provisional,
    const uint64_t* normalized_costs,
    size_t normalized_cost_count) noexcept {
  if (provisional == ResultCapability::UNSUPPORTED ||
      provisional == ResultCapability::READY) {
    return provisional;
  }
  if (!normalized_costs || normalized_cost_count <= 1) {
    return provisional;
  }

  uint64_t best = std::numeric_limits<uint64_t>::max();
  for (size_t i = 0; i < normalized_cost_count; ++i) {
    if (normalized_costs[i] < best) {
      best = normalized_costs[i];
    }
  }
  if (best == std::numeric_limits<uint64_t>::max()) {
    return provisional;
  }

  for (size_t i = 0; i < normalized_cost_count; ++i) {
    if (normalized_costs[i] <= best * kResultAccessCheapWithinBestMultiplier) {
      return ResultCapability::CHEAP;
    }
  }
  return ResultCapability::EXPENSIVE;
}

bool CoreResultStore::retain_frame(const FrameView& frame,
                                   std::optional<StreamIntent> stream_intent,
                                   uint64_t capture_timestamp_ns,
                                   uint64_t stream_applied_access_posture_epoch,
                                   uint64_t capture_applied_access_posture_epoch,
                                   CoreRetainedProductionPlan stream_requested_retained_plan,
                                   CoreRetainedProductionPlan capture_requested_retained_plan) {
  if (frame.stream_id != 0 && !stream_requested_retained_plan.valid) {
    return false;
  }
  if (frame.capture_id != 0 && !capture_requested_retained_plan.valid) {
    return false;
  }
  const uint64_t retain_begin_ns = capture_latency_trace_now_ns();
  uint64_t payload_copy_ns = 0;
  const bool has_cpu_payload = CoreResultStore::has_cpu_packed_payload(frame);
  const std::optional<CoreRetainedBackingPlan> stream_backing_plan =
      frame.stream_id != 0 ? std::make_optional(build_retained_backing_plan_from_requested(stream_requested_retained_plan, frame, has_cpu_payload)) : std::nullopt;
  const std::optional<CoreRetainedBackingPlan> capture_backing_plan =
      frame.capture_id != 0 ? std::make_optional(build_retained_backing_plan_from_requested(capture_requested_retained_plan, frame, has_cpu_payload)) : std::nullopt;
  CoreResultPayloadCpuPacked payload{};
  CoreImageFactBundle facts{};
  if (has_cpu_payload) {
    const uint64_t payload_copy_begin_ns = capture_latency_trace_now_ns();
    if (!CoreResultStore::try_copy_cpu_packed_payload(frame, payload)) {
      return false;
    }
    payload_copy_ns = capture_latency_trace_now_ns() - payload_copy_begin_ns;
  }

  SharedStreamResultData replaced_stream_result;
  std::lock_guard<std::mutex> lock(mutex_);

  if (frame.stream_id != 0) {
    if (!has_valid_result_image_description(frame)) {
      return false;
    }
    const CoreRetainedBackingPlan& plan = *stream_backing_plan;
    const bool gpu_primary = plan.primary_kind == ResultPayloadKind::GPU_SURFACE;
    if (!frame_matches_requested_retained_plan(frame, plan, stream_requested_retained_plan, has_cpu_payload)) {
      return false;
    }
    if (plan.primary_kind == ResultPayloadKind::CPU_PACKED && !has_cpu_payload) {
      return false;
    }
    std::shared_ptr<void> retained_gpu_backing =
        plan.retain_gpu_display ? frame.primary_backing_artifact : nullptr;
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
    stream_result->payload_kind = plan.primary_kind;
    stream_result->retained_gpu_backing = std::move(retained_gpu_backing);
    stream_result->retained_gpu_backing_descriptor = retained_gpu_backing_descriptor;
    if (plan.primary_kind == ResultPayloadKind::CPU_PACKED || plan.retain_cpu_sidecar) {
      if (frame.capture_id == 0) {
        stream_result->payload = std::move(payload);
      } else {
        // Defensive fallback for any future dual-routed FrameView. Current
        // stream and capture paths are distinct, but preserving capture payload
        // truth is more important than saving this copy in an unexpected mixed
        // route.
        stream_result->payload = payload;
      }
      stream_result->payload_capture_timestamp_ns = capture_timestamp_ns;
    }
    stream_result->retained_access_truth = build_stream_retained_access_truth(*stream_result);
    stream_result->access_classification =
        std::make_shared<CoreResultAccessClassificationRecord>();
    const bool stream_has_current_cpu_payload =
        stream_result->payload_capture_timestamp_ns == stream_result->capture_timestamp_ns &&
        has_valid_retained_cpu_packed_access_payload(
            stream_result->payload,
            stream_result->image_width,
            stream_result->image_height,
            stream_result->image_format_fourcc);
    stream_result->access_posture = build_stream_access_posture_key(
        *stream_result,
        stream_has_current_cpu_payload,
        resolve_stream_access_posture_id(
            *stream_result, stream_has_current_cpu_payload, stream_applied_access_posture_epoch));
    facts = build_default_facts(frame.width, frame.height, frame.format_fourcc);
    stream_result->facts = facts;
    auto& slot = latest_stream_results_[frame.stream_id];
    replaced_stream_result = std::move(slot);
    slot = std::move(stream_result);
  }

  const bool payload_adopted = payload.uses_retained_bytes();
  if (frame.capture_id != 0) {
    const CoreRetainedBackingPlan& plan = *capture_backing_plan;
    if (!frame_matches_requested_retained_plan(frame, plan, capture_requested_retained_plan, has_cpu_payload)) {
      return false;
    }
    if (plan.primary_kind == ResultPayloadKind::CPU_PACKED && !has_cpu_payload) {
      return false;
    }
    if (plan.primary_kind == ResultPayloadKind::GPU_SURFACE && !frame.primary_backing_artifact) {
      return false;
    }
    std::shared_ptr<void> retained_gpu_backing =
        plan.retain_gpu_display ? frame.primary_backing_artifact : nullptr;
    RetainedGpuBackingDescriptor retained_gpu_backing_descriptor =
        build_retained_gpu_backing_descriptor(frame, capture_timestamp_ns, plan.primary_kind == ResultPayloadKind::GPU_SURFACE);
    MutableCaptureResultData capture_result =
        build_default_image_capture_result(
            frame,
            plan,
            std::move(payload),
            std::move(retained_gpu_backing),
            retained_gpu_backing_descriptor,
            capture_timestamp_ns);
    if (capture_result) {
      const bool has_default_cpu_payload =
          has_valid_capture_image_member_payload(capture_result->default_image.payload);
      capture_result->default_image.access_posture = build_capture_member_access_posture_key(
          frame.device_instance_id,
          capture_result->default_image,
          has_default_cpu_payload,
          resolve_capture_member_access_posture_id(
              frame.device_instance_id,
              capture_result->default_image,
              has_default_cpu_payload,
              capture_applied_access_posture_epoch));
    }
    capture_results_by_capture_id_[frame.capture_id][frame.device_instance_id] = std::move(capture_result);
  }
  const bool retained = frame.stream_id != 0 || frame.capture_id != 0;
  if (frame.capture_id != 0) {
    capture_latency_trace_printf(
        "result_store_retain_frame capture_id=%llu device_id=%llu acquisition_session_id=%llu member=%u payload_copy_us=%llu payload_adopted=%u total_us=%llu retained=%u bytes=%llu",
        static_cast<unsigned long long>(frame.capture_id),
        static_cast<unsigned long long>(frame.device_instance_id),
        static_cast<unsigned long long>(frame.acquisition_session_id),
        static_cast<unsigned>(frame.capture_image.image_member_index),
        static_cast<unsigned long long>(payload_copy_ns / 1000ull),
        payload_adopted ? 1u : 0u,
        static_cast<unsigned long long>((capture_latency_trace_now_ns() - retain_begin_ns) / 1000ull),
        retained ? 1u : 0u,
        static_cast<unsigned long long>(frame.size_bytes));
  }
  return retained;
}

bool CoreResultStore::append_additional_capture_image(
    uint64_t capture_id,
    uint64_t device_instance_id,
    CoreCaptureResultData::ImageMemberData image_member,
    uint64_t capture_applied_access_posture_epoch,
    CoreRetainedProductionPlan capture_requested_retained_plan) {
  if (!capture_requested_retained_plan.valid) {
    return false;
  }
  const uint64_t append_begin_ns = capture_latency_trace_now_ns();
  const uint32_t image_member_index = image_member.image_member_index;
  const size_t payload_bytes = image_member.payload.size_bytes();
  std::lock_guard<std::mutex> lock(mutex_);
  auto cap_it = capture_results_by_capture_id_.find(capture_id);
  if (cap_it == capture_results_by_capture_id_.end()) {
    return false;
  }
  auto dev_it = cap_it->second.find(device_instance_id);
  if (dev_it == cap_it->second.end() || !dev_it->second) {
    return false;
  }
  const bool valid_cpu_payload = has_valid_capture_image_member_payload(image_member.payload);
  if (capture_requested_retained_plan.valid) {
    const bool member_gpu = image_member.payload_kind == ResultPayloadKind::GPU_SURFACE;
    if (capture_requested_retained_plan.primary_cpu() == member_gpu) {
      return false;
    }
    if (capture_requested_retained_plan.retain_cpu_sidecar() && !valid_cpu_payload) {
      return false;
    }
  }
  const bool valid_gpu_payload =
      image_member.payload_kind == ResultPayloadKind::GPU_SURFACE &&
      image_member.retained_gpu_backing &&
      image_member.retained_gpu_backing_descriptor.valid;
  if (!valid_cpu_payload && !valid_gpu_payload) {
    return false;
  }
  if (image_member.role != CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET) {
    return false;
  }
  if (image_member.image_member_index == 0u) {
    return false;
  }

  auto& result = dev_it->second;
  if (result->capture_image_facts_finalized) {
    return false;
  }
  const uint32_t expected_member_index =
      1u + static_cast<uint32_t>(result->additional_images.size());
  if (image_member.image_member_index != expected_member_index) {
    return false;
  }

  image_member.retained_access_truth =
      build_capture_image_member_retained_access_truth(image_member);
  image_member.access_classification =
      std::make_shared<CoreResultAccessClassificationRecord>();
  image_member.access_posture = build_capture_member_access_posture_key(
      device_instance_id,
      image_member,
      valid_cpu_payload,
      resolve_capture_member_access_posture_id(
          device_instance_id, image_member, valid_cpu_payload, capture_applied_access_posture_epoch));

  if (result.use_count() != 1) {
    // Preserve immutability for any result object already handed out through the
    // public terminal-gated result API. In the ordinary assembly path the store
    // owns the only reference here, so additional members append without
    // copy-constructing already-retained image payloads.
    result = std::make_shared<CoreCaptureResultData>(*result);
  }
  result->additional_images.push_back(std::move(image_member));
  capture_latency_trace_printf(
      "result_store_append_additional capture_id=%llu device_id=%llu member=%u payload_bytes=%llu total_us=%llu",
      static_cast<unsigned long long>(capture_id),
      static_cast<unsigned long long>(device_instance_id),
      static_cast<unsigned>(image_member_index),
      static_cast<unsigned long long>(payload_bytes),
      static_cast<unsigned long long>((capture_latency_trace_now_ns() - append_begin_ns) / 1000ull));
  return true;
}

bool CoreResultStore::finalize_capture_facts(
    uint64_t capture_id,
    uint64_t device_instance_id,
    std::optional<CaptureAdmissionContext> admission_context,
    const std::function<CoreResolvedCaptureImageFacts(uint32_t image_member_index)>&
        resolve_image_facts) {
  if (capture_id == 0 || device_instance_id == 0 || !resolve_image_facts) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const auto capture_it = capture_results_by_capture_id_.find(capture_id);
  if (capture_it == capture_results_by_capture_id_.end()) {
    return false;
  }
  const auto device_it = capture_it->second.find(device_instance_id);
  if (device_it == capture_it->second.end() || !device_it->second ||
      device_it->second->capture_image_facts_finalized) {
    return false;
  }

  MutableCaptureResultData& result = device_it->second;
  if (result.use_count() != 1) {
    result = std::make_shared<CoreCaptureResultData>(*result);
  }
  result->has_admission_context = admission_context.has_value();
  if (admission_context) {
    result->admission_context = std::move(*admission_context);
  }
  result->default_image.resolved_image_facts =
      resolve_image_facts(result->default_image.image_member_index);
  for (CoreCaptureResultData::ImageMemberData& member : result->additional_images) {
    member.resolved_image_facts = resolve_image_facts(member.image_member_index);
  }
  result->capture_image_facts_finalized = true;
  return true;
}

bool CoreResultStore::try_build_capture_image_member_data_from_frame(
    const FrameView& frame,
    CoreCaptureResultData::ImageMemberData& out_member,
    CoreRetainedProductionPlan requested_retained_plan) {
  if (!requested_retained_plan.valid) {
    return false;
  }
  const bool has_cpu_payload = CoreResultStore::has_cpu_packed_payload(frame);
  const CoreRetainedBackingPlan plan = build_retained_backing_plan_from_requested(requested_retained_plan, frame, has_cpu_payload);
  if (!frame_matches_requested_retained_plan(frame, plan, requested_retained_plan, has_cpu_payload)) {
    return false;
  }
  if (plan.primary_kind == ResultPayloadKind::CPU_PACKED) {
    if (!try_build_capture_image_member_data_from_frame(frame, out_member.payload)) {
      return false;
    }
  } else if (plan.primary_kind == ResultPayloadKind::GPU_SURFACE) {
    if (!frame.primary_backing_artifact) {
      return false;
    }
    if (plan.retain_cpu_sidecar && has_cpu_payload) {
      (void)try_build_capture_image_member_data_from_frame(frame, out_member.payload);
    }
    out_member.retained_gpu_backing = frame.primary_backing_artifact;
    out_member.retained_gpu_backing_descriptor =
        build_retained_gpu_backing_descriptor(frame, out_member.capture_timestamp_ns, true);
  } else {
    return false;
  }
  out_member.payload_kind = plan.primary_kind;
  out_member.retained_access_truth = build_capture_image_member_retained_access_truth(out_member);
  out_member.access_classification =
      std::make_shared<CoreResultAccessClassificationRecord>();
  // The store assigns the live posture id when the member is accepted into a
  // concrete capture result; this helper only builds provider-retained member truth.
  return true;
}


bool CoreResultStore::try_build_capture_image_member_data_from_frame(const FrameView& frame,
                                                                     CoreResultPayloadCpuPacked& out_payload) {
  const uint64_t build_begin_ns = capture_latency_trace_now_ns();
  if (!has_cpu_packed_payload(frame)) {
    return false;
  }
  const uint64_t copy_begin_ns = capture_latency_trace_now_ns();
  if (!try_copy_cpu_packed_payload(frame, out_payload)) {
    return false;
  }
  const uint64_t copy_ns = capture_latency_trace_now_ns() - copy_begin_ns;
  const bool valid = has_valid_capture_image_member_payload(out_payload);
  if (frame.capture_id != 0) {
    capture_latency_trace_printf(
        "result_store_build_member_payload capture_id=%llu device_id=%llu acquisition_session_id=%llu member=%u payload_copy_us=%llu payload_adopted=%u total_us=%llu valid=%u bytes=%llu",
        static_cast<unsigned long long>(frame.capture_id),
        static_cast<unsigned long long>(frame.device_instance_id),
        static_cast<unsigned long long>(frame.acquisition_session_id),
        static_cast<unsigned>(frame.capture_image.image_member_index),
        static_cast<unsigned long long>(copy_ns / 1000ull),
        out_payload.uses_retained_bytes() ? 1u : 0u,
        static_cast<unsigned long long>((capture_latency_trace_now_ns() - build_begin_ns) / 1000ull),
        valid ? 1u : 0u,
        static_cast<unsigned long long>(frame.size_bytes));
  }
  return valid;
}

MutableCaptureResultData CoreResultStore::build_default_image_capture_result(
    const FrameView& frame,
    CoreRetainedBackingPlan plan,
    CoreResultPayloadCpuPacked payload,
    std::shared_ptr<void> retained_gpu_backing,
    RetainedGpuBackingDescriptor retained_gpu_backing_descriptor,
    uint64_t capture_timestamp_ns) {
  if (frame.capture_image.routing != CaptureImageRouting::DEFAULT_METERED ||
      frame.capture_image.image_member_index != 0u) {
    return nullptr;
  }
  auto capture_result = std::make_shared<CoreCaptureResultData>();
  capture_result->capture_id = frame.capture_id;
  capture_result->device_instance_id = frame.device_instance_id;
  capture_result->acquisition_session_id = frame.acquisition_session_id;
  capture_result->image_width = frame.width;
  capture_result->image_height = frame.height;
  capture_result->image_format_fourcc = frame.format_fourcc;
  capture_result->payload_kind = plan.primary_kind;

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

  CoreImageFactBundle facts = build_default_facts(frame.width, frame.height, frame.format_fourcc);
  capture_result->default_image.payload_kind = plan.primary_kind;
  if (plan.primary_kind == ResultPayloadKind::CPU_PACKED || plan.retain_cpu_sidecar) {
    capture_result->default_image.payload = std::move(payload);
  }
  capture_result->default_image.retained_gpu_backing = std::move(retained_gpu_backing);
  capture_result->default_image.retained_gpu_backing_descriptor = retained_gpu_backing_descriptor;
  capture_result->default_image.retained_access_truth =
      build_capture_image_member_retained_access_truth(capture_result->default_image);
  capture_result->default_image.access_classification =
      std::make_shared<CoreResultAccessClassificationRecord>();
  const bool has_default_cpu_payload = has_valid_capture_image_member_payload(capture_result->default_image.payload);
  capture_result->default_image.access_posture = build_capture_member_access_posture_key(
      frame.device_instance_id,
      capture_result->default_image,
      has_default_cpu_payload,
      0);
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
  if (payload.empty()) {
    return false;
  }
  return true;
}


bool CoreResultStore::StreamAccessPostureDomainKey::operator<(
    const StreamAccessPostureDomainKey& other) const noexcept {
  if (stream_id != other.stream_id) return stream_id < other.stream_id;
  if (applied_epoch != other.applied_epoch) return applied_epoch < other.applied_epoch;
  if (width != other.width) return width < other.width;
  if (height != other.height) return height < other.height;
  if (format_fourcc != other.format_fourcc) return format_fourcc < other.format_fourcc;
  if (payload_kind != other.payload_kind) return payload_kind < other.payload_kind;
  if (has_retained_cpu_payload != other.has_retained_cpu_payload) {
    return has_retained_cpu_payload < other.has_retained_cpu_payload;
  }
  if (has_retained_gpu_backing != other.has_retained_gpu_backing) {
    return has_retained_gpu_backing < other.has_retained_gpu_backing;
  }
  if (gpu_materialization_available != other.gpu_materialization_available) {
    return gpu_materialization_available < other.gpu_materialization_available;
  }
  return gpu_materialization_requires_readback < other.gpu_materialization_requires_readback;
}

bool CoreResultStore::CaptureAccessPostureDomainKey::operator<(
    const CaptureAccessPostureDomainKey& other) const noexcept {
  if (device_instance_id != other.device_instance_id) return device_instance_id < other.device_instance_id;
  if (applied_epoch != other.applied_epoch) return applied_epoch < other.applied_epoch;
  if (width != other.width) return width < other.width;
  if (height != other.height) return height < other.height;
  if (format_fourcc != other.format_fourcc) return format_fourcc < other.format_fourcc;
  if (payload_kind != other.payload_kind) return payload_kind < other.payload_kind;
  if (has_retained_cpu_payload != other.has_retained_cpu_payload) {
    return has_retained_cpu_payload < other.has_retained_cpu_payload;
  }
  if (has_retained_gpu_backing != other.has_retained_gpu_backing) {
    return has_retained_gpu_backing < other.has_retained_gpu_backing;
  }
  if (gpu_materialization_available != other.gpu_materialization_available) {
    return gpu_materialization_available < other.gpu_materialization_available;
  }
  return gpu_materialization_requires_readback < other.gpu_materialization_requires_readback;
}

uint64_t CoreResultStore::resolve_stream_access_posture_id(
    const CoreStreamResultData& result,
    bool has_current_cpu_payload,
    uint64_t applied_epoch) {
  if (applied_epoch == 0) {
    applied_epoch = 1;
  }
  StreamAccessPostureDomainKey key{};
  key.stream_id = result.stream_id;
  key.applied_epoch = applied_epoch;
  key.width = result.image_width;
  key.height = result.image_height;
  key.format_fourcc = result.image_format_fourcc;
  key.payload_kind = result.payload_kind;
  key.has_retained_cpu_payload = has_current_cpu_payload;
  key.has_retained_gpu_backing = static_cast<bool>(result.retained_gpu_backing);
  key.gpu_materialization_available = result.retained_gpu_backing_descriptor.valid &&
                                      result.retained_gpu_backing_descriptor.materialization_available;
  key.gpu_materialization_requires_readback = result.retained_gpu_backing_descriptor.valid &&
                                             result.retained_gpu_backing_descriptor.materialization_requires_gpu_readback;
  auto [it, inserted] = stream_access_posture_ids_.emplace(key, 0);
  if (inserted) {
    it->second = next_posture_id(next_result_access_posture_id_);
  }
  return it->second;
}

uint64_t CoreResultStore::resolve_capture_member_access_posture_id(
    uint64_t device_instance_id,
    const CoreCaptureResultData::ImageMemberData& member,
    bool has_cpu_payload,
    uint64_t applied_epoch) {
  if (applied_epoch == 0) {
    applied_epoch = 1;
  }
  CaptureAccessPostureDomainKey key{};
  key.device_instance_id = device_instance_id;
  key.applied_epoch = applied_epoch;
  key.width = member.payload.width != 0 ? member.payload.width : member.retained_gpu_backing_descriptor.width;
  key.height = member.payload.height != 0 ? member.payload.height : member.retained_gpu_backing_descriptor.height;
  key.format_fourcc = member.payload.format_fourcc != 0
      ? member.payload.format_fourcc
      : member.retained_gpu_backing_descriptor.format_fourcc;
  key.payload_kind = member.payload_kind;
  key.has_retained_cpu_payload = has_cpu_payload;
  key.has_retained_gpu_backing = static_cast<bool>(member.retained_gpu_backing);
  key.gpu_materialization_available = member.retained_gpu_backing_descriptor.valid &&
                                      member.retained_gpu_backing_descriptor.materialization_available;
  key.gpu_materialization_requires_readback = member.retained_gpu_backing_descriptor.valid &&
                                             member.retained_gpu_backing_descriptor.materialization_requires_gpu_readback;
  auto [it, inserted] = capture_access_posture_ids_.emplace(key, 0);
  if (inserted) {
    it->second = next_posture_id(next_result_access_posture_id_);
  }
  return it->second;
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
  std::map<uint64_t, std::map<uint64_t, MutableCaptureResultData>> old_capture_results;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    old_stream_results.swap(latest_stream_results_);
    old_capture_results.swap(capture_results_by_capture_id_);
    stream_display_demand_last_seen_ns_.clear();
    stream_display_demand_refcounts_.clear();
    stream_access_posture_ids_.clear();
    capture_access_posture_ids_.clear();
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

  const bool can_adopt_tightly_packed_owner =
      frame.cpu_payload_owner &&
      src_stride == row_bytes &&
      frame.data == frame.cpu_payload_owner->data() &&
      frame.cpu_payload_owner->size() >= dst_size &&
      frame.size_bytes >= dst_size;
  if (can_adopt_tightly_packed_owner) {
    out.bytes.clear();
    out.retained_bytes = frame.cpu_payload_owner;
    return true;
  }

  if (dst_size > out.bytes.max_size()) {
    return false;
  }
  out.retained_bytes.reset();
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
