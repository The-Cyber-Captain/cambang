#include "core/core_result_store.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>

#include "pixels/format/pixel_format_descriptor.h"

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
  // 0 remains "CamBANG cannot state a bit depth for this format", which is what
  // an unnamed FourCC yields from the descriptor table.
  return describe_pixel_format(format_fourcc).bits_per_component;
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

// A frame's image description is usable when CamBANG can name the format and
// therefore reason about its geometry. Whether that format can be retained,
// displayed, or materialized is decided separately, by the paths that
// implement it.
bool has_valid_result_image_description(const FrameView& frame) {
  if (frame.width == 0 || frame.height == 0) {
    return false;
  }
  return is_known_pixel_format(frame.format_fourcc);
}

CoreRetainedAccessTruth build_capture_image_member_retained_access_truth(
    const CoreCaptureResultData::ImageMemberData& member) {
  CoreRetainedAccessTruth truth{};
  if (retained_cpu_payload_is_packed_readable(member.payload)) {
    truth.display_view = ResultCapability::CHEAP;
    truth.to_image = ResultCapability::CHEAP;
    return truth;
  }
  // A planar member converts on demand through the same shared routine the
  // stream paths use, so it is supported and equally non-ready. Without this
  // a planar capture retains truthfully and then reports no way to reach it.
  if (retained_cpu_payload_is_convertible(member.payload)) {
    truth.display_view = ResultCapability::EXPENSIVE;
    truth.to_image = ResultCapability::EXPENSIVE;
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

// CPU-primary retention covers both packed and planar payloads. Plan checks
// ask "is this CPU primary", never "is this specifically packed" -- the packed
// vs planar distinction belongs to access truth, which is stricter.
constexpr bool is_cpu_primary_kind(ResultPayloadKind kind) noexcept {
  return kind == ResultPayloadKind::CPU_PACKED || kind == ResultPayloadKind::CPU_PLANAR;
}

// The CPU-primary payload kind implied by a frame's format.
ResultPayloadKind cpu_primary_kind_for_frame(const FrameView& frame) noexcept {
  const PixelFormatDescriptor desc = describe_pixel_format(frame.format_fourcc);
  return (desc.valid && desc.layout_class != PixelLayoutClass::Packed)
      ? ResultPayloadKind::CPU_PLANAR
      : ResultPayloadKind::CPU_PACKED;
}

CoreRetainedBackingPlan build_retained_backing_plan_from_requested(
    CoreRetainedProductionPlan requested,
    const FrameView& frame,
    bool has_cpu_payload) {
  CoreRetainedBackingPlan plan{};
  plan.primary_kind = cpu_primary_kind_for_frame(frame);
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
    bool gpu_primary) {
  RetainedGpuBackingDescriptor descriptor = frame.retained_gpu_backing_descriptor;
  if (descriptor.valid) {
    // The provider owns identity/capability truth; core only normalizes
    // correlation and image fields that are already present on this FrameView.
    descriptor.stream_id = frame.stream_id;
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
  descriptor.width = frame.width;
  descriptor.height = frame.height;
  descriptor.stride_bytes = frame.stride_bytes;
  descriptor.format_fourcc = frame.format_fourcc;
  descriptor.display_available = static_cast<bool>(frame.primary_backing_artifact);
  descriptor.materialization_available = false;
  descriptor.materialization_requires_gpu_readback = false;
  return descriptor;
}

// Capture-result byte-budget accounting (ledger #53). Estimates a single
// image member's resource footprint: literal bytes for a CPU_PACKED payload,
// plus an estimated footprint for a GPU-backed member computed from
// Core-visible, provider-agnostic descriptor metadata (width/height/stride)
// -- never inspects the opaque retained_gpu_backing handle itself, which
// would violate the Core/provider seam. A member may legitimately have both
// (GPU-primary-with-CPU-sidecar posture), in which case both contribute.
uint64_t effective_member_bytes(const CoreCaptureResultData::ImageMemberData& member) noexcept {
  uint64_t bytes = member.payload.size_bytes();
  if (member.retained_gpu_backing_descriptor.valid) {
    bytes += static_cast<uint64_t>(member.retained_gpu_backing_descriptor.stride_bytes) *
             static_cast<uint64_t>(member.retained_gpu_backing_descriptor.height);
  }
  return bytes;
}

uint64_t compute_capture_result_bytes(const CoreCaptureResultData& data) noexcept {
  uint64_t bytes = effective_member_bytes(data.default_image);
  for (const auto& member : data.additional_images) {
    bytes += effective_member_bytes(member);
  }
  return bytes;
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
  const bool has_cpu_payload = CoreResultStore::has_cpu_payload(frame);
  const std::optional<CoreRetainedBackingPlan> stream_backing_plan =
      frame.stream_id != 0 ? std::make_optional(build_retained_backing_plan_from_requested(stream_requested_retained_plan, frame, has_cpu_payload)) : std::nullopt;
  const std::optional<CoreRetainedBackingPlan> capture_backing_plan =
      frame.capture_id != 0 ? std::make_optional(build_retained_backing_plan_from_requested(capture_requested_retained_plan, frame, has_cpu_payload)) : std::nullopt;
  CoreResultPayloadCpu payload{};
  CoreImageFactBundle facts{};
  if (has_cpu_payload) {
    if (!CoreResultStore::try_copy_cpu_payload(frame, payload)) {
      return false;
    }
  }

  SharedStreamResultData replaced_stream_result;
  std::lock_guard<std::mutex> lock(mutex_);
  std::shared_ptr<CoreStreamResultData> stream_result;
  MutableCaptureResultData capture_result;

  if (frame.stream_id != 0) {
    if (!has_valid_result_image_description(frame)) {
      return false;
    }
    const CoreRetainedBackingPlan& plan = *stream_backing_plan;
    const bool gpu_primary = plan.primary_kind == ResultPayloadKind::GPU_SURFACE;
    if (!frame_matches_requested_retained_plan(frame, plan, stream_requested_retained_plan, has_cpu_payload)) {
      return false;
    }
    if (is_cpu_primary_kind(plan.primary_kind) && !has_cpu_payload) {
      return false;
    }
    std::shared_ptr<void> retained_gpu_backing =
        plan.retain_gpu_display ? frame.primary_backing_artifact : nullptr;
    RetainedGpuBackingDescriptor retained_gpu_backing_descriptor =
        build_retained_gpu_backing_descriptor(frame, gpu_primary);

    auto mutable_stream_result = std::make_shared<CoreStreamResultData>();
    mutable_stream_result->stream_id = frame.stream_id;
    mutable_stream_result->device_instance_id = frame.device_instance_id;
    mutable_stream_result->intent = stream_intent.value_or(StreamIntent::PREVIEW);
    mutable_stream_result->image_width = frame.width;
    mutable_stream_result->image_height = frame.height;
    mutable_stream_result->image_format_fourcc = frame.format_fourcc;
    mutable_stream_result->payload_kind = plan.primary_kind;
    mutable_stream_result->retained_gpu_backing = std::move(retained_gpu_backing);
    mutable_stream_result->retained_gpu_backing_descriptor = retained_gpu_backing_descriptor;
    if (is_cpu_primary_kind(plan.primary_kind) || plan.retain_cpu_sidecar) {
      if (frame.capture_id == 0) {
        mutable_stream_result->payload = std::move(payload);
      } else {
        // Defensive fallback for any future dual-routed FrameView. Current
        // stream and capture paths are distinct, but preserving capture payload
        // truth is more important than saving this copy in an unexpected mixed
        // route.
        mutable_stream_result->payload = payload;
      }
    }
    mutable_stream_result->retained_access_truth = build_stream_retained_access_truth(*mutable_stream_result);
    mutable_stream_result->access_classification =
        std::make_shared<CoreResultAccessClassificationRecord>();
    const bool stream_has_current_cpu_payload =
        mutable_stream_result->payload.uses_retained_bytes() || !mutable_stream_result->payload.empty();
    mutable_stream_result->access_posture = build_stream_access_posture_key(
        *mutable_stream_result,
        stream_has_current_cpu_payload,
        resolve_stream_access_posture_id(
            *mutable_stream_result, stream_has_current_cpu_payload, stream_applied_access_posture_epoch));
    // Derive from the already-assigned top-level fields (not frame.* again)
    // so image_properties cannot structurally drift from get_width()/
    // get_height()/get_format().
    facts = build_default_facts(
        mutable_stream_result->image_width,
        mutable_stream_result->image_height,
        mutable_stream_result->image_format_fourcc);
    mutable_stream_result->image_facts.acquisition_timing = frame.acquisition_timing;
    mutable_stream_result->facts = facts;
    stream_result = std::move(mutable_stream_result);
  }

  if (frame.capture_id != 0) {
    const CoreRetainedBackingPlan& plan = *capture_backing_plan;
    if (!frame_matches_requested_retained_plan(frame, plan, capture_requested_retained_plan, has_cpu_payload)) {
      return false;
    }
    if (is_cpu_primary_kind(plan.primary_kind) && !has_cpu_payload) {
      return false;
    }
    if (plan.primary_kind == ResultPayloadKind::GPU_SURFACE && !frame.primary_backing_artifact) {
      return false;
    }
    std::shared_ptr<void> retained_gpu_backing =
        plan.retain_gpu_display ? frame.primary_backing_artifact : nullptr;
    RetainedGpuBackingDescriptor retained_gpu_backing_descriptor =
        build_retained_gpu_backing_descriptor(frame, plan.primary_kind == ResultPayloadKind::GPU_SURFACE);
    // Waiver -- infeasible use-after-move path: the only prior move of
    // `payload` (stream branch above) requires frame.capture_id == 0, while
    // this branch requires capture_id != 0, and the dual-routed case
    // deliberately copies (not moves) there so this move stays safe. frame is
    // const and core-thread-local, so the two guards cannot disagree within
    // one call. Verified 2026-07-19; see
    // docs/dev/audit_baselines/cpp_static_analysis_baseline_2026_07_19.md.
    capture_result = build_default_image_capture_result(
            frame,
            plan,
            // NOLINTNEXTLINE(bugprone-use-after-move)
            std::move(payload),
            std::move(retained_gpu_backing),
            retained_gpu_backing_descriptor);
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
    if (!capture_result) return false;
  }
  // Prepare every container slot before issuing an identity. Afterwards the
  // pointer assignments are the non-throwing retained-truth commit.
  SharedStreamResultData* stream_slot = nullptr;
  MutableCaptureResultData* capture_slot = nullptr;
  bool inserted_stream_slot = false;
  bool inserted_capture_bucket = false;
  bool inserted_capture_slot = false;
  const auto rollback_prepared_slots = [&]() noexcept {
    if (inserted_stream_slot) {
      latest_stream_results_.erase(frame.stream_id);
    }
    if (inserted_capture_slot) {
      auto capture_it = capture_results_by_capture_id_.find(frame.capture_id);
      if (capture_it != capture_results_by_capture_id_.end()) {
        capture_it->second.erase(frame.device_instance_id);
      }
    }
    if (inserted_capture_bucket) {
      auto capture_it = capture_results_by_capture_id_.find(frame.capture_id);
      if (capture_it != capture_results_by_capture_id_.end() && capture_it->second.empty()) {
        capture_results_by_capture_id_.erase(capture_it);
      }
    }
  };
  try {
    if (stream_result) {
      auto [it, inserted] = latest_stream_results_.try_emplace(frame.stream_id);
      stream_slot = &it->second;
      inserted_stream_slot = inserted;
    }
    if (capture_result) {
      auto [capture_it, bucket_inserted] =
          capture_results_by_capture_id_.try_emplace(frame.capture_id);
      inserted_capture_bucket = bucket_inserted;
      auto [device_it, inserted] =
          capture_it->second.try_emplace(frame.device_instance_id);
      capture_slot = &device_it->second;
      inserted_capture_slot = inserted;
    }
  } catch (...) {
    rollback_prepared_slots();
    return false;
  }
  uint64_t retained_frame_id = 0;
  if (!try_issue_retained_frame_id(retained_frame_id)) {
    rollback_prepared_slots();
    return false;
  }
  if (stream_result) {
    stream_result->retained_frame_id = retained_frame_id;
    if (!stream_result->payload.empty()) {
      stream_result->payload_retained_frame_id = retained_frame_id;
    }
    stream_result->retained_access_truth = build_stream_retained_access_truth(*stream_result);
    replaced_stream_result = std::move(*stream_slot);
    *stream_slot = std::move(stream_result);
  }
  if (capture_result) {
    capture_result->default_image.retained_frame_id = retained_frame_id;
    const uint64_t old_capture_bytes =
        *capture_slot ? compute_capture_result_bytes(**capture_slot) : 0;
    const uint64_t new_capture_bytes = compute_capture_result_bytes(*capture_result);
    *capture_slot = std::move(capture_result);
    total_estimated_capture_bytes_ =
        total_estimated_capture_bytes_ - old_capture_bytes + new_capture_bytes;
  }
  const bool retained = frame.stream_id != 0 || frame.capture_id != 0;
  return retained;
}

bool CoreResultStore::try_issue_retained_frame_id(uint64_t& out_id) noexcept {
  if (next_retained_frame_id_ == 0) {
    return false;
  }
  out_id = next_retained_frame_id_;
  if (next_retained_frame_id_ == std::numeric_limits<uint64_t>::max()) {
    next_retained_frame_id_ = 0;
  } else {
    ++next_retained_frame_id_;
  }
  return true;
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
  result->additional_images.reserve(result->additional_images.size() + 1);
  if (!try_issue_retained_frame_id(image_member.retained_frame_id)) return false;
  const uint64_t added_member_bytes = effective_member_bytes(image_member);
  result->additional_images.push_back(std::move(image_member));
  total_estimated_capture_bytes_ += added_member_bytes;
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
  result->default_image.resolved_image_facts.image.acquisition_timing =
      result->default_image.acquisition_timing;
  for (CoreCaptureResultData::ImageMemberData& member : result->additional_images) {
    member.resolved_image_facts = resolve_image_facts(member.image_member_index);
    member.resolved_image_facts.image.acquisition_timing = member.acquisition_timing;
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
  const bool has_cpu_payload = CoreResultStore::has_cpu_payload(frame);
  const CoreRetainedBackingPlan plan = build_retained_backing_plan_from_requested(requested_retained_plan, frame, has_cpu_payload);
  if (!frame_matches_requested_retained_plan(frame, plan, requested_retained_plan, has_cpu_payload)) {
    return false;
  }
  if (is_cpu_primary_kind(plan.primary_kind)) {
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
        build_retained_gpu_backing_descriptor(frame, true);
  } else {
    return false;
  }
  out_member.acquisition_timing = frame.acquisition_timing;
  out_member.payload_kind = plan.primary_kind;
  out_member.retained_access_truth = build_capture_image_member_retained_access_truth(out_member);
  out_member.access_classification =
      std::make_shared<CoreResultAccessClassificationRecord>();
  // The store assigns the live posture id when the member is accepted into a
  // concrete capture result; this helper only builds provider-retained member truth.
  return true;
}


bool CoreResultStore::try_build_capture_image_member_data_from_frame(const FrameView& frame,
                                                                     CoreResultPayloadCpu& out_payload) {
  if (!has_cpu_payload(frame)) {
    return false;
  }
  if (!try_copy_cpu_payload(frame, out_payload)) {
    return false;
  }
  const bool valid = has_valid_capture_image_member_payload(out_payload);
  return valid;
}

MutableCaptureResultData CoreResultStore::build_default_image_capture_result(
    const FrameView& frame,
    CoreRetainedBackingPlan plan,
    CoreResultPayloadCpu payload,
    std::shared_ptr<void> retained_gpu_backing,
    RetainedGpuBackingDescriptor retained_gpu_backing_descriptor) {
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
  capture_result->default_image.acquisition_timing = frame.acquisition_timing;

  // Derive from the already-assigned top-level fields (not frame.* again) so
  // image_properties cannot structurally drift from get_width()/get_height()/
  // get_format().
  CoreImageFactBundle facts = build_default_facts(
      capture_result->image_width,
      capture_result->image_height,
      capture_result->image_format_fourcc);
  capture_result->default_image.payload_kind = plan.primary_kind;
  if (is_cpu_primary_kind(plan.primary_kind) || plan.retain_cpu_sidecar) {
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
  capture_result->facts = facts;
  return capture_result;
}

bool CoreResultStore::has_valid_capture_image_member_payload(const CoreResultPayloadCpu& payload) {
  if (payload.width == 0 || payload.height == 0) {
    return false;
  }
  // Retention validity only: can CamBANG reason about these bytes and account
  // for them truthfully. Whether a CPU access path can *use* them is a
  // separate, stricter question answered by
  // build_capture_image_member_retained_access_truth(), which gates
  // to_image_member() on is_packed_rgb_format().
  //
  // These were one check until planar retention landed. Conflated, a planar
  // capture member was copied in full and then rejected here, so capture
  // failed outright where the stream path retains the frame and reports
  // UNSUPPORTED access. Keep the two questions separate.
  if (!is_known_pixel_format(payload.format_fourcc)) {
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

void CoreResultStore::remove_capture_result(uint64_t capture_id, uint64_t device_instance_id) {
  if (capture_id == 0 || device_instance_id == 0) {
    return;
  }
  MutableCaptureResultData removed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto capture_it = capture_results_by_capture_id_.find(capture_id);
    if (capture_it == capture_results_by_capture_id_.end()) {
      return;
    }
    auto device_it = capture_it->second.find(device_instance_id);
    if (device_it == capture_it->second.end()) {
      return;
    }
    removed = std::move(device_it->second);
    if (removed) {
      const uint64_t removed_bytes = compute_capture_result_bytes(*removed);
      total_estimated_capture_bytes_ =
          removed_bytes <= total_estimated_capture_bytes_ ? total_estimated_capture_bytes_ - removed_bytes : 0;
    }
    capture_it->second.erase(device_it);
    if (capture_it->second.empty()) {
      capture_results_by_capture_id_.erase(capture_it);
    }
  }
  // removed's destructor (releasing any retained CPU/GPU payload) runs here,
  // outside the lock.
}

std::vector<CoreResultStore::EvictedCaptureResult> CoreResultStore::evict_over_byte_budget(
    uint64_t byte_budget,
    const std::function<bool(uint64_t capture_id, uint64_t device_instance_id)>& is_evictable) {
  std::vector<MutableCaptureResultData> released;
  std::vector<EvictedCaptureResult> evicted;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto capture_it = capture_results_by_capture_id_.begin();
    while (total_estimated_capture_bytes_ > byte_budget &&
           capture_it != capture_results_by_capture_id_.end()) {
      auto device_it = capture_it->second.begin();
      while (total_estimated_capture_bytes_ > byte_budget &&
             device_it != capture_it->second.end()) {
        if (!is_evictable || !is_evictable(capture_it->first, device_it->first)) {
          ++device_it;
          continue;
        }
        MutableCaptureResultData& entry = device_it->second;
        if (entry) {
          const uint64_t entry_bytes = compute_capture_result_bytes(*entry);
          total_estimated_capture_bytes_ =
              entry_bytes <= total_estimated_capture_bytes_ ? total_estimated_capture_bytes_ - entry_bytes : 0;
        }
        evicted.push_back(EvictedCaptureResult{capture_it->first, device_it->first});
        released.push_back(std::move(entry));
        device_it = capture_it->second.erase(device_it);
      }
      if (capture_it->second.empty()) {
        capture_it = capture_results_by_capture_id_.erase(capture_it);
      } else {
        ++capture_it;
      }
    }
  }
  // released's destructor (freeing every evicted entry's CPU/GPU payload)
  // runs here, outside the lock, so a concurrent get_latest_stream_result()/
  // get_capture_result() reader is never blocked behind deallocation work.
  return evicted;
}

uint64_t CoreResultStore::total_estimated_capture_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_estimated_capture_bytes_;
}


void CoreResultStore::clear() {
  std::map<uint64_t, SharedStreamResultData> old_stream_results;
  std::map<uint64_t, std::map<uint64_t, MutableCaptureResultData>> old_capture_results;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    old_stream_results.swap(latest_stream_results_);
    old_capture_results.swap(capture_results_by_capture_id_);
    total_estimated_capture_bytes_ = 0;
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

bool CoreResultStore::try_copy_cpu_packed_payload(const FrameView& frame, CoreResultPayloadCpu& out) {
  if (!has_cpu_payload(frame)) {
    return false;
  }

  // This is the single-plane packed retention path. Planar and semi-planar
  // payloads belong to try_copy_cpu_planar_payload(); rejecting them here
  // keeps a misrouted frame from being copied as if plane 0 were the whole
  // image.
  const PixelFormatDescriptor desc = describe_pixel_format(frame.format_fourcc);
  if (!desc.valid || desc.layout_class != PixelLayoutClass::Packed) {
    return false;
  }
  const uint32_t bytes_per_pixel = desc.plane0_bytes_per_sample;
  if (bytes_per_pixel == 0 ||
      frame.width > (std::numeric_limits<uint32_t>::max() / bytes_per_pixel)) {
    return false;
  }

  size_t row_bytes = 0;
  if (!checked_mul_size_t(static_cast<size_t>(frame.width), bytes_per_pixel, row_bytes)) {
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
  // A packed payload is described entirely by the scalar stride above.
  out.plane_count = 0;

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

bool CoreResultStore::has_cpu_payload(const FrameView& frame) {
  if (frame.width == 0 || frame.height == 0) {
    return false;
  }
  // effective_payload_layout() resolves both a populated multi-plane layout and
  // the legacy single-plane scalars, and reports absence for a multi-plane
  // format delivered through the scalars alone.
  return frame.effective_payload_layout().present();
}

bool CoreResultStore::try_copy_cpu_payload(const FrameView& frame, CoreResultPayloadCpu& out) {
  const PixelFormatDescriptor desc = describe_pixel_format(frame.format_fourcc);
  if (!desc.valid) {
    return false;
  }
  return (desc.layout_class == PixelLayoutClass::Packed)
      ? try_copy_cpu_packed_payload(frame, out)
      : try_copy_cpu_planar_payload(frame, out);
}

// Retains a planar or semi-planar payload as one tightly packed contiguous
// buffer, recording each plane's offset/stride/rows.
//
// Provider padding is removed on the way in: retaining tight means every later
// consumer (GPU upload, conversion) can rely on a single stride rule per plane
// rather than re-deriving the provider's arbitrary alignment. The copy is the
// price of that, and it replaces the full-frame YUV->RGBA conversion the
// provider would otherwise have done, so it is not a new cost on the frame
// path.
bool CoreResultStore::try_copy_cpu_planar_payload(const FrameView& frame, CoreResultPayloadCpu& out) {
  const PayloadLayout layout = frame.effective_payload_layout();
  if (!layout.present() || !validate_payload_layout(layout)) {
    return false;
  }
  const PixelFormatDescriptor desc = describe_pixel_format(layout.format_fourcc);
  if (!desc.valid || desc.layout_class == PixelLayoutClass::Packed) {
    return false;
  }
  const size_t total = min_tight_size_bytes(desc, layout.width, layout.height);
  if (total == 0 || total > out.bytes.max_size()) {
    return false;
  }

  out.format_fourcc = layout.format_fourcc;
  out.width = layout.width;
  out.height = layout.height;
  out.plane_count = layout.plane_count;
  // Carried through retention: a consumer converting these bytes later cannot
  // infer range or matrix from them, and guessing produces a plausible-looking
  // wrong image rather than a visible failure.
  out.colorimetry = layout.colorimetry;
  out.retained_bytes.reset();
  out.bytes.assign(total, 0u);

  size_t offset = 0;
  for (uint32_t plane = 0; plane < layout.plane_count; ++plane) {
    const PayloadPlaneView& pv = layout.planes[plane];
    const size_t row_bytes = static_cast<size_t>(plane_row_bytes(desc, plane, layout.width));
    const size_t rows = static_cast<size_t>(plane_rows(desc, plane, layout.height));
    const size_t src_stride = (pv.stride_bytes != 0) ? static_cast<size_t>(pv.stride_bytes) : row_bytes;
    if (row_bytes == 0 || rows == 0 || src_stride < row_bytes) {
      return false;
    }

    out.planes[plane].offset_bytes = offset;
    out.planes[plane].stride_bytes = static_cast<uint32_t>(row_bytes);
    out.planes[plane].rows = static_cast<uint32_t>(rows);

    const uint8_t* src = pv.data;
    uint8_t* dst = out.bytes.data() + offset;
    for (size_t y = 0; y < rows; ++y) {
      std::memcpy(dst, src, row_bytes);
      src += src_stride;
      dst += row_bytes;
    }
    offset += row_bytes * rows;
  }

  // Scalar stride describes plane 0, matching the packed path's meaning.
  out.stride_bytes = out.planes[0].stride_bytes;
  return offset == total;
}

} // namespace cambang
