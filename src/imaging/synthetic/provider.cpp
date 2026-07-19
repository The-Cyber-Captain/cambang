#include "imaging/synthetic/provider.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <exception>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "imaging/synthetic/scenario_loader.h"
#include "imaging/synthetic/gpu_update_policy_resolver.h"
#include "imaging/api/timeline_teardown_trace.h"
#include "imaging/synthetic/gpu_backing_runtime.h"
#include "pixels/pattern/pattern_render_target.h"
#if __has_include(<godot_cpp/variant/utility_functions.hpp>)
#include <godot_cpp/variant/utility_functions.hpp>
#define CAMBANG_SYNTH_TRIAGE_HAS_GODOT_UTILITY_PRINT 1
#else
#define CAMBANG_SYNTH_TRIAGE_HAS_GODOT_UTILITY_PRINT 0
#endif

namespace cambang {

ProviderAccessStatus SyntheticProvider::check_access_readiness() noexcept {
  return ProviderAccessStatus::ready("synthetic_provider_ready");
}

namespace {

const TickPeriod& synthetic_acquisition_tick_period() {
  static const TickPeriod period = *TickPeriod::create(1, 1);
  return period;
}

std::optional<ImageAcquisitionTiming> synthetic_acquisition_timing_from_unsigned(
    uint64_t acquisition_mark,
    ImageAcquisitionReferenceEvent reference_event,
    ImageAcquisitionComparability comparability) {
  const auto checked_mark = ImageAcquisitionTiming::checked_mark_from_unsigned(acquisition_mark);
  if (!checked_mark) {
    return std::nullopt;
  }
  return ImageAcquisitionTiming::create(
      *checked_mark,
      synthetic_acquisition_tick_period(),
      ImageAcquisitionClockDomain::PROVIDER_MONOTONIC,
      reference_event,
      comparability);
}

constexpr const char* kHardwareIdPrefix = "synthetic:";
constexpr uint64_t kTriageLogIntervalNs = 1'000'000'000ull;

void synthetic_triage_print_line(const std::string& line) {
#if CAMBANG_SYNTH_TRIAGE_HAS_GODOT_UTILITY_PRINT
  godot::UtilityFunctions::print(line.c_str());
#else
  std::fprintf(stdout, "%s\n", line.c_str());
#endif
}

void synthetic_triage_printf(const char* format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  synthetic_triage_print_line(buffer);
}

bool env_flag_enabled(const char* name) {
  const char* value = std::getenv(name);
  if (!value) {
    return false;
  }
  return value[0] == '1' || value[0] == 't' || value[0] == 'T' || value[0] == 'y' || value[0] == 'Y';
}

uint32_t env_u32_or_default(const char* name, uint32_t fallback) {
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value || (end && *end != '\0')) {
    return fallback;
  }
  if (parsed > std::numeric_limits<uint32_t>::max()) {
    return std::numeric_limits<uint32_t>::max();
  }
  return static_cast<uint32_t>(parsed);
}


// Monotonic timestamp source for SyntheticProvider's "triage" capture-ready
// timing telemetry (triage_capture_ready_stage_*, CaptureReadyTimingRecord).
// Not diagnostic scaffolding -- these records are real, permanent telemetry.
uint64_t provider_monotonic_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t fps_period_ns(uint32_t fps_num, uint32_t fps_den) {
  if (fps_num == 0 || fps_den == 0) {
    return 0;
  }
  // period = 1s * den / num
  const uint64_t one_sec = 1'000'000'000ull;
  return (one_sec * static_cast<uint64_t>(fps_den)) / static_cast<uint64_t>(fps_num);
}

double ns_to_ms(uint64_t ns) {
  return static_cast<double>(ns) / 1'000'000.0;
}

void record_timing_sample(uint64_t sample_ns, uint64_t& calls, uint64_t& total_ns, uint64_t& max_ns) {
  ++calls;
  total_ns += sample_ns;
  max_ns = std::max(max_ns, sample_ns);
}

void copy_rgba8_with_optional_adjustments(
    uint8_t* dst,
    uint32_t dst_stride_bytes,
    const uint8_t* src,
    uint32_t src_stride_bytes,
    uint32_t width,
    uint32_t height,
    bool dst_bgra,
    int32_t applied_exposure_compensation_milli_ev) noexcept {
  // SyntheticProvider owns synthetic still-image generation, including
  // bracket-member exposure-variant synthesis from a deterministic CPU base
  // frame. Platform-backed providers are not expected to recreate that model:
  // they adapt backend-produced frames into CamBANG truth instead.
  //
  // The optional BGRA write path is different in kind. CamBANG's provider
  // contract allows packed RGBA/BGRA delivery, so platform-backed providers
  // may still need format mapping to satisfy the negotiated FourCC. What is
  // synthetic-specific here is fusing that mapping into synthetic frame
  // generation, not the existence of RGBA/BGRA adaptation in general.
  std::array<uint8_t, 256> lut{};
  const uint8_t* lut_ptr = nullptr;
  if (applied_exposure_compensation_milli_ev != 0) {
    const double gain = std::pow(
        2.0,
        static_cast<double>(applied_exposure_compensation_milli_ev) / 1000.0);
    for (size_t i = 0; i < lut.size(); ++i) {
      const double adjusted = static_cast<double>(i) * gain;
      lut[i] = static_cast<uint8_t>(std::clamp(adjusted, 0.0, 255.0));
    }
    lut_ptr = lut.data();
  }

  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src_row =
        src + static_cast<size_t>(y) * static_cast<size_t>(src_stride_bytes);
    uint8_t* dst_row =
        dst + static_cast<size_t>(y) * static_cast<size_t>(dst_stride_bytes);
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* src_px = src_row + static_cast<size_t>(x) * 4u;
      uint8_t* dst_px = dst_row + static_cast<size_t>(x) * 4u;
      uint8_t r = src_px[0];
      uint8_t g = src_px[1];
      uint8_t b = src_px[2];
      const uint8_t a = src_px[3];
      if (lut_ptr != nullptr) {
        r = lut_ptr[r];
        g = lut_ptr[g];
        b = lut_ptr[b];
      }
      if (dst_bgra) {
        dst_px[0] = b;
        dst_px[1] = g;
        dst_px[2] = r;
      } else {
        dst_px[0] = r;
        dst_px[1] = g;
        dst_px[2] = b;
      }
      dst_px[3] = a;
    }
  }
}

} // namespace

SyntheticProvider::SyntheticProvider(const SyntheticProviderConfig& cfg) : cfg_(cfg) {
  // Normalize defaults for v1.
  if (cfg_.nominal.format_fourcc == 0) {
    cfg_.nominal.format_fourcc = FOURCC_RGBA;
  }
  if (cfg_.endpoint_count == 0) {
    cfg_.endpoint_count = 1;
  }
  completion_gated_destructive_sequencing_enabled_ =
      (cfg_.timeline_reconciliation == TimelineReconciliation::CompletionGated);
  triage_trace_enabled_ = env_flag_enabled("CAMBANG_DEV_SYNTH_TRIAGE_TRACE");
  display_demand_trace_enabled_ = env_flag_enabled("CAMBANG_DEV_DISPLAY_DEMAND_TRACE");
  triage_catchup_cap_per_tick_ = env_u32_or_default("CAMBANG_DEV_SYNTH_CATCHUP_CAP", 0);
}

SyntheticProvider::~SyntheticProvider() {
  if (initialized_) {
    (void)shutdown();
  } else {
    stop_and_join_capture_executor_();
  }
}

StreamTemplate SyntheticProvider::stream_template() const {
  StreamTemplate t{};
  t.profile.width = cfg_.nominal.width;
  t.profile.height = cfg_.nominal.height;
  t.profile.format_fourcc = cfg_.nominal.format_fourcc;
  t.profile.target_fps_min = cfg_.nominal.fps_num / (cfg_.nominal.fps_den ? cfg_.nominal.fps_den : 1);
  t.profile.target_fps_max = t.profile.target_fps_min;

  t.picture.preset = PatternPreset::Noise;
  t.picture.seed = static_cast<uint32_t>(cfg_.pattern.seed);
  t.picture.generator_fps_num = 30;
  t.picture.generator_fps_den = 1;
  t.picture.overlay_frame_index_offsets = false;
  t.picture.overlay_moving_bar = true;
  return t;
}

static PatternSpec build_stream_render_spec(
    const PictureConfig& picture,
    uint32_t width,
    uint32_t height,
    bool* preset_valid) {
  return to_pattern_spec(picture, width, height, PatternSpec::PackedFormat::RGBA8, preset_valid);
}

CaptureTemplate SyntheticProvider::capture_template() const {
  CaptureTemplate t{};
  t.profile.width = cfg_.nominal.width;
  t.profile.height = cfg_.nominal.height;
  t.profile.format_fourcc = cfg_.nominal.format_fourcc;
  t.profile.target_fps_min = cfg_.nominal.fps_num / (cfg_.nominal.fps_den ? cfg_.nominal.fps_den : 1);
  t.profile.target_fps_max = t.profile.target_fps_min;

  t.picture.preset = PatternPreset::NoiseAnimated;
  t.picture.seed = static_cast<uint32_t>(cfg_.pattern.seed ^ 0xA5A5u);
  t.picture.generator_fps_num = 30;
  t.picture.generator_fps_den = 1;
  t.picture.overlay_frame_index_offsets = true;
  t.picture.overlay_moving_bar = false;
  return t;
}

ProducerBackingCapabilities SyntheticProvider::stream_backing_capabilities(
    const CaptureProfile& profile,
    const PictureConfig& picture) const noexcept {
  return query_stream_producer_capabilities_(profile, picture);
}

ProducerBackingCapabilities SyntheticProvider::capture_backing_capabilities(
    const CaptureRequest& req) const noexcept {
  return query_capture_producer_capabilities_(req);
}

ProducerBackingCapabilities SyntheticProvider::stream_parent_context_backing_capabilities(
    uint64_t device_instance_id,
    uint64_t stream_id,
    StreamIntent intent,
    const CaptureProfile& profile,
    const PictureConfig& picture) noexcept {
  (void)stream_id;
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  return stream_parent_context_backing_capabilities_locked_(
      device_instance_id, intent, profile, picture);
}

ProducerBackingCapabilities SyntheticProvider::capture_parent_context_backing_capabilities(
    uint64_t device_instance_id,
    const CaptureRequest& req) noexcept {
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  return capture_parent_context_backing_capabilities_locked_(
      device_instance_id, req);
}

bool SyntheticProvider::has_runtime_gpu_backing_path_() noexcept {
  return synthetic_gpu_backing_runtime_available();
}

ProducerBackingCapabilities SyntheticProvider::query_stream_producer_capabilities_(
    const CaptureProfile& profile,
    const PictureConfig& picture) const noexcept {
  (void)profile;
  (void)picture;
  const bool gpu_available = has_runtime_gpu_backing_path_();
  switch (cfg_.producer_output_form_mode) {
    case SyntheticProducerOutputFormMode::CpuOnly:
      return ProducerBackingCapabilities{true, false, false};
    case SyntheticProducerOutputFormMode::GpuOnly:
      return gpu_available ? ProducerBackingCapabilities{false, true, false}
                           : ProducerBackingCapabilities{false, false, false};
    case SyntheticProducerOutputFormMode::CpuAndGpu:
      return gpu_available ? ProducerBackingCapabilities{true, true, true}
                           : ProducerBackingCapabilities{true, false, false};
    case SyntheticProducerOutputFormMode::Auto:
    default:
      return ProducerBackingCapabilities{true, gpu_available, gpu_available};
  }
}

ProducerBackingCapabilities SyntheticProvider::query_capture_producer_capabilities_(
    const CaptureRequest& req) const noexcept {
  (void)req;
  const bool gpu_available = has_runtime_gpu_backing_path_();
  switch (cfg_.producer_output_form_mode) {
    case SyntheticProducerOutputFormMode::CpuOnly:
      return ProducerBackingCapabilities{true, false, false};
    case SyntheticProducerOutputFormMode::GpuOnly:
      return gpu_available ? ProducerBackingCapabilities{false, true, false}
                           : ProducerBackingCapabilities{false, false, false};
    case SyntheticProducerOutputFormMode::CpuAndGpu:
      return gpu_available ? ProducerBackingCapabilities{true, true, true}
                           : ProducerBackingCapabilities{true, false, false};
    case SyntheticProducerOutputFormMode::Auto:
    default:
      return ProducerBackingCapabilities{true, gpu_available, gpu_available};
  }
}

const char* SyntheticProvider::resolve_hardware_id_for_device_locked_(
    uint64_t device_instance_id) const noexcept {
  const auto it = devices_.find(device_instance_id);
  if (it == devices_.end() || !it->second.open ||
      it->second.hardware_id.empty()) {
    return nullptr;
  }
  return it->second.hardware_id.c_str();
}

ProducerBackingCapabilities SyntheticProvider::stream_parent_context_backing_capabilities_locked_(
    uint64_t device_instance_id,
    StreamIntent intent,
    const CaptureProfile& profile,
    const PictureConfig& picture) const noexcept {
  const ProducerBackingCapabilities outer_caps =
      query_stream_producer_capabilities_(profile, picture);
  const char* hardware_id =
      resolve_hardware_id_for_device_locked_(device_instance_id);
  return apply_stream_capability_downgrade_conditions_locked_(
      hardware_id, intent, profile, outer_caps);
}

ProducerBackingCapabilities SyntheticProvider::capture_parent_context_backing_capabilities_locked_(
    uint64_t device_instance_id,
    const CaptureRequest& req) const noexcept {
  const ProducerBackingCapabilities outer_caps =
      query_capture_producer_capabilities_(req);
  const char* hardware_id =
      resolve_hardware_id_for_device_locked_(device_instance_id);
  return apply_capture_capability_downgrade_conditions_locked_(
      hardware_id, req, outer_caps);
}

ProducerBackingCapabilities SyntheticProvider::apply_stream_capability_downgrade_conditions_locked_(
    const char* hardware_id,
    StreamIntent intent,
    const CaptureProfile& profile,
    ProducerBackingCapabilities outer_caps) const noexcept {
  if (!outer_caps.cpu_backed_available ||
      !outer_caps.gpu_backed_available) {
    return outer_caps;
  }
  if (!hardware_id) {
    return outer_caps;
  }
  for (const SyntheticStreamCapabilityDowngradeCondition& condition :
       cfg_.verification_stream_capability_downgrade_conditions) {
    if (condition.device_hardware_id != hardware_id) {
      continue;
    }
    if (condition.has_stream_intent &&
        condition.stream_intent != intent) {
      continue;
    }
    if (condition.width != 0 && condition.width != profile.width) {
      continue;
    }
    if (condition.height != 0 && condition.height != profile.height) {
      continue;
    }
    if (condition.format_fourcc != 0 &&
        condition.format_fourcc != profile.format_fourcc) {
      continue;
    }
    if (condition.target_fps != 0 &&
        (condition.target_fps != profile.target_fps_min ||
         condition.target_fps != profile.target_fps_max)) {
      continue;
    }
    return ProducerBackingCapabilities{true, false, false};
  }
  return outer_caps;
}

ProducerBackingCapabilities SyntheticProvider::apply_capture_capability_downgrade_conditions_locked_(
    const char* hardware_id,
    const CaptureRequest& req,
    ProducerBackingCapabilities outer_caps) const noexcept {
  if (!outer_caps.cpu_backed_available ||
      !outer_caps.gpu_backed_available) {
    return outer_caps;
  }
  if (!hardware_id) {
    return outer_caps;
  }
  for (const SyntheticCaptureCapabilityDowngradeCondition& condition :
       cfg_.verification_capture_capability_downgrade_conditions) {
    if (condition.device_hardware_id != hardware_id) {
      continue;
    }
    if (condition.width != 0 && condition.width != req.width) {
      continue;
    }
    if (condition.height != 0 && condition.height != req.height) {
      continue;
    }
    if (condition.format_fourcc != 0 &&
        condition.format_fourcc != req.format_fourcc) {
      continue;
    }
    if (condition.still_image_bundle !=
            SyntheticStillImageBundleDiscriminator::Any) {
      const bool multi_image = req.still_image_bundle.members.size() > 1u;
      if (condition.still_image_bundle ==
              SyntheticStillImageBundleDiscriminator::DefaultMeteredOnly &&
          multi_image) {
        continue;
      }
      if (condition.still_image_bundle ==
              SyntheticStillImageBundleDiscriminator::MultiImage &&
          !multi_image) {
        continue;
      }
    }
    return ProducerBackingCapabilities{true, false, false};
  }
  return outer_caps;
}

bool SyntheticProvider::choose_stream_gpu_preference_(
    ProducerBackingCapabilities capabilities) const noexcept {
  return capabilities.gpu_backed_available;
}

SyntheticProducerOutputFormMode SyntheticProvider::resolve_producer_output_form_mode_(
    ProducerBackingCapabilities capabilities) const noexcept {
  switch (cfg_.producer_output_form_mode) {
    case SyntheticProducerOutputFormMode::CpuOnly:
      return SyntheticProducerOutputFormMode::CpuOnly;
    case SyntheticProducerOutputFormMode::GpuOnly:
      return SyntheticProducerOutputFormMode::GpuOnly;
    case SyntheticProducerOutputFormMode::CpuAndGpu:
      return capabilities.gpu_backed_available ? SyntheticProducerOutputFormMode::CpuAndGpu
                                               : SyntheticProducerOutputFormMode::CpuOnly;
    case SyntheticProducerOutputFormMode::Auto:
    default:
      return choose_stream_gpu_preference_(capabilities) ? SyntheticProducerOutputFormMode::CpuAndGpu
                                                         : SyntheticProducerOutputFormMode::CpuOnly;
  }
}


uint64_t SyntheticProvider::generator_frame_ordinal_from_ns_(
    uint64_t timestamp_ns,
    const PictureConfig& picture) noexcept {
  if (picture.generator_fps_num == 0 || picture.generator_fps_den == 0) {
    return 0;
  }
  const __uint128_t numerator =
      static_cast<__uint128_t>(timestamp_ns) * static_cast<__uint128_t>(picture.generator_fps_num);
  const __uint128_t denominator =
      static_cast<__uint128_t>(1'000'000'000ull) * static_cast<__uint128_t>(picture.generator_fps_den);
  if (denominator == 0) {
    return 0;
  }
  const __uint128_t q = numerator / denominator;
  if (q > static_cast<__uint128_t>(std::numeric_limits<uint64_t>::max())) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(q);
}

ProviderResult SyntheticProvider::initialize(IProviderCallbacks* callbacks) {
  if (initialized_) {
    return ProviderResult::success();
  }
  if (!callbacks) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  const StreamGpuUpdatePolicyResolution gpu_policy_resolution = resolve_synthetic_stream_gpu_update_policy();
  if (gpu_policy_resolution.has_conflict) {
    synthetic_triage_printf("[CamBANG][SyntheticProvider][ERROR] %s", gpu_policy_resolution.error_message.c_str());
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (cfg_.producer_output_form_mode == SyntheticProducerOutputFormMode::GpuOnly &&
      (!cfg_.verification_stream_capability_downgrade_conditions.empty() ||
       !cfg_.verification_capture_capability_downgrade_conditions.empty())) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  callbacks_ = callbacks;
  if (!strand_.start(callbacks_, "synthetic_provider")) {
    callbacks_ = nullptr;
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }
  shutting_down_ = false;
  if (!start_capture_executor_()) {
    strand_.stop();
    callbacks_ = nullptr;
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }
  initialized_ = true;
  {
    std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
    capture_pause_depth_by_device_.clear();
  }
  triage_next_log_ns_ = 0;
  if (triage_trace_enabled_) {
    synthetic_triage_printf("[CamBANG][SyntheticTriage] enabled=true catchup_cap=%u",
                            triage_catchup_cap_per_tick_);
  }

  if (cfg_.synthetic_role == SyntheticRole::Timeline) {
    // Backward-compatibility baseline for Timeline-role synthetic operation:
    // advance(dt_ns) should pump timeline-driven emission immediately for
    // verifier-owned flows (including direct open/create/start flows that do
    // not stage host scenarios).
    // This is an execution-arming distinction only; scenario semantics remain
    // provider-owned for both init-seeded and host-submitted paths.
    timeline_running_ = true;
    timeline_paused_ = false;
    timeline_scenario_ = cfg_.timeline_scenario;
    if (!timeline_scenario_.events.empty()) {
      // Config-seeded scenarios are provider-owned and auto-run at initialize.
      for (const auto& ev : timeline_scenario_.events) {
        timeline_schedule_(ev);
      }
    }
  }

  // Report provider native object (BOUND). Root/owners are 0 at provider scope.
  provider_native_id_ = alloc_native_id_(NativeObjectType::Provider);
  if (callbacks_) {
    NativeObjectCreateInfo info{};
    info.native_id = provider_native_id_;
    info.type = static_cast<uint32_t>(NativeObjectType::Provider);
    info.root_id = 0;
    info.owner_provider_native_id = 0;
    info.owner_rig_id = 0;
    info.has_created_ns = true;
    info.created_ns = clock_.now_ns();
    strand_.post_native_object_created(info);
  }

  return ProviderResult::success();
}

void SyntheticProvider::timeline_schedule_(uint64_t at_ns, SyntheticEventType type, uint64_t stream_id) {
  SyntheticScheduledEvent ev{};
  ev.at_ns = at_ns;
  ev.type = type;
  ev.stream_id = stream_id;
  timeline_schedule_(ev);
}

void SyntheticProvider::timeline_schedule_(const SyntheticScheduledEvent& src) {
  SyntheticScheduledEvent ev = src;
  ev.seq = ++timeline_seq_;
  timeline_q_.push(ev);
}

void SyntheticProvider::timeline_dispatch_request_(const SyntheticScheduledEvent& ev) {
  if (timeline_request_dispatch_hook_) {
    timeline_request_dispatch_hook_(ev);
  }
}

bool SyntheticProvider::timeline_is_destructive_primitive_(SyntheticEventType type) const {
  return type == SyntheticEventType::StopStream ||
         type == SyntheticEventType::DestroyStream ||
         type == SyntheticEventType::CloseDevice;
}

bool SyntheticProvider::timeline_destructive_prereq_ready_(
    const SyntheticScheduledEvent& ev,
    const char*& reason) const {
  // Completion authority for this gate is provider-integrated runtime fact:
  // the relevant stream/device state must already be reflected here.
  // Submit rc==OK from the host binding is only admission, not completion.
  reason = nullptr;
  switch (ev.type) {
    case SyntheticEventType::DestroyStream: {
      auto it = streams_.find(ev.stream_id);
      if (it != streams_.end() && it->second.created && it->second.started) {
        reason = "await_stream_stopped";
        return false;
      }
      if (it != streams_.end() && it->second.created) {
        for (const auto& slot : it->second.pool) {
          if (slot && slot->in_use.load(std::memory_order_acquire)) {
            reason = "await_stream_buffers_released";
            return false;
          }
        }
      }
      return true;
    }
    case SyntheticEventType::CloseDevice: {
      for (const auto& kv : streams_) {
        if (kv.second.created && kv.second.req.device_instance_id == ev.device_instance_id) {
          reason = "await_device_stream_absence";
          return false;
        }
      }
      return true;
    }
    case SyntheticEventType::StopStream:
      return true;
    default:
      return true;
  }
}

void SyntheticProvider::timeline_activate_or_dispatch_(const SyntheticScheduledEvent& ev, bool allow_pending) {
  if (completion_gated_destructive_sequencing_enabled_ && timeline_is_destructive_primitive_(ev.type)) {
    if (ev.type == SyntheticEventType::StopStream) {
      timeline_teardown_trace_emit("activate StopStream stream_id=%llu",
                                   static_cast<unsigned long long>(ev.stream_id));
    } else if (ev.type == SyntheticEventType::DestroyStream) {
      timeline_teardown_trace_emit("activate DestroyStream stream_id=%llu",
                                   static_cast<unsigned long long>(ev.stream_id));
    } else if (ev.type == SyntheticEventType::CloseDevice) {
      timeline_teardown_trace_emit("activate CloseDevice device_instance_id=%llu",
                                   static_cast<unsigned long long>(ev.device_instance_id));
    }

    const char* reason = nullptr;
    if (!timeline_destructive_prereq_ready_(ev, reason) && allow_pending) {
      if (ev.type == SyntheticEventType::DestroyStream) {
        timeline_teardown_trace_emit("pending DestroyStream stream_id=%llu reason=%s",
                                     static_cast<unsigned long long>(ev.stream_id),
                                     reason ? reason : "await_prereq");
      } else if (ev.type == SyntheticEventType::CloseDevice) {
        timeline_teardown_trace_emit("pending CloseDevice device_instance_id=%llu reason=%s",
                                     static_cast<unsigned long long>(ev.device_instance_id),
                                     reason ? reason : "await_prereq");
      }
      timeline_pending_destructive_.push_back(ev);
      return;
    }
  }

  if (ev.type == SyntheticEventType::StopStream) {
    timeline_teardown_trace_emit("dispatch StopStream stream_id=%llu",
                                 static_cast<unsigned long long>(ev.stream_id));
  } else if (ev.type == SyntheticEventType::DestroyStream) {
    timeline_teardown_trace_emit("dispatch DestroyStream stream_id=%llu",
                                 static_cast<unsigned long long>(ev.stream_id));
  } else if (ev.type == SyntheticEventType::CloseDevice) {
    timeline_teardown_trace_emit("dispatch CloseDevice device_instance_id=%llu",
                                 static_cast<unsigned long long>(ev.device_instance_id));
  }
  timeline_dispatch_request_(ev);
}

bool SyntheticProvider::materialize_staged_canonical_scenario_(
    SyntheticTimelineScenario& out,
    std::vector<SyntheticStagedRigTopology>& rigs_out,
    std::string& error) const {
  out = {};
  rigs_out.clear();
  SyntheticScenarioMaterializationOptions opts{};
  SyntheticScenarioMaterializationResult materialized{};
  if (!materialize_synthetic_canonical_scenario(
          timeline_canonical_scenario_,
          opts,
          materialized,
          &error)) {
    return false;
  }

  // Provider-owned projection boundary:
  // canonical schedule is lowered to executable request-like timeline events.
  // Lifecycle actions must be explicit in the canonical timed actions.
  // EmitFrame remains provider-internal and is not emitted here.
  rigs_out.reserve(materialized.rigs.size());
  for (const auto& r : materialized.rigs) { SyntheticStagedRigTopology t{}; t.rig_id = r.rig_id; t.member_hardware_ids = r.member_hardware_ids; rigs_out.push_back(std::move(t)); }
  uint32_t required_endpoint_count = 0;
  for (const auto& d : materialized.devices) {
    required_endpoint_count = std::max(required_endpoint_count, d.endpoint_index + 1u);
  }
  const_cast<SyntheticProvider*>(this)->staged_required_endpoint_count_ = required_endpoint_count;

  std::vector<SyntheticScheduledEvent> events;
  events.reserve(materialized.executable_schedule.events.size());

  for (const auto& ev : materialized.executable_schedule.events) {
    events.push_back(ev);
  }

  std::stable_sort(events.begin(), events.end(), [](const SyntheticScheduledEvent& a,
                                                     const SyntheticScheduledEvent& b) {
    return a.at_ns < b.at_ns;
  });
  for (size_t i = 0; i < events.size(); ++i) {
    events[i].seq = static_cast<uint64_t>(i + 1);
  }
  out.events = std::move(events);
  return true;
}

void SyntheticProvider::timeline_pump_(bool allow_paused_timeline_step) {
  // Pump all events due at the current provider virtual time. A caller may reach
  // this through advance(0), which is intentionally meaningful for deterministic
  // deferred startup: already-due at_ns=0 events can dispatch without wall-clock
  // advancement or a later host frame.
  const auto pump_t0 = std::chrono::steady_clock::now();
  struct PumpTimingScope final {
    SyntheticProvider* self = nullptr;
    std::chrono::steady_clock::time_point t0{};
    ~PumpTimingScope() {
      if (!self) {
        return;
      }
      const auto t1 = std::chrono::steady_clock::now();
      const uint64_t pump_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
      record_timing_sample(
          pump_ns,
          self->triage_timeline_pump_calls_,
          self->triage_timeline_pump_total_ns_,
          self->triage_timeline_pump_max_ns_);
    }
  } pump_timing_scope{this, pump_t0};
  if (!timeline_running_ || (timeline_paused_ && !allow_paused_timeline_step)) {
    return;
  }
  const uint64_t now = clock_.now_ns();
  const uint64_t period = fps_period_ns(cfg_.nominal.fps_num, cfg_.nominal.fps_den);
  if (period == 0) {
    return;
  }

  uint32_t emitted_this_pump = 0;
  bool catchup_tick_capped = false;
  while (!timeline_q_.empty()) {
    const SyntheticScheduledEvent ev = timeline_q_.top();
    if (ev.at_ns > now) {
      break;
    }
    timeline_q_.pop();
    const auto event_t0 = std::chrono::steady_clock::now();

    switch (ev.type) {
      case SyntheticEventType::EmitFrame: {
        const auto emit_event_t0 = std::chrono::steady_clock::now();
        auto it = streams_.find(ev.stream_id);
        if (it == streams_.end()) {
          break;
        }
        StreamState& s = it->second;
        if (!s.created || !s.started) {
          break;
        }
        if (is_stream_capture_paused_locked_(s)) {
          s.consecutive_behind_ticks = 0;
          s.next_due_ns = snap_repeating_due_after_(ev.at_ns, now, period);
          timeline_schedule_(s.next_due_ns, SyntheticEventType::EmitFrame, ev.stream_id);
          break;
        }
        if (triage_catchup_cap_per_tick_ > 0 && emitted_this_pump >= triage_catchup_cap_per_tick_) {
          if (!catchup_tick_capped) {
            ++triage_catchup_ticks_capped_total_;
            catchup_tick_capped = true;
          }
          ++triage_catchup_frames_dropped_total_;
          s.next_due_ns = ev.at_ns + period;
          timeline_schedule_(s.next_due_ns, SyntheticEventType::EmitFrame, ev.stream_id);
          break;
        }
        // Execute the same frame emission path as nominal, but driven by explicit
        // scheduled event timestamps.
        emit_one_frame_(s, ev.at_ns);
        ++triage_frames_emitted_total_;
        ++emitted_this_pump;
        if (ev.at_ns < now) {
          ++triage_falling_behind_repeat_total_;
        }
        s.next_due_ns = ev.at_ns + period;
        // Deterministic continuation: schedule the next frame.
        timeline_schedule_(s.next_due_ns, SyntheticEventType::EmitFrame, ev.stream_id);
        const auto emit_event_t1 = std::chrono::steady_clock::now();
        const uint64_t emit_event_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(emit_event_t1 - emit_event_t0).count());
        record_timing_sample(
            emit_event_ns,
            triage_timeline_emit_event_calls_,
            triage_timeline_emit_event_total_ns_,
            triage_timeline_emit_event_max_ns_);
        break;
      }

      case SyntheticEventType::StartStream: {
        timeline_dispatch_request_(ev);
        break;
      }

      case SyntheticEventType::StopStream: {
        timeline_activate_or_dispatch_(ev, /*allow_pending=*/false);
        break;
      }

      case SyntheticEventType::OpenDevice: {
        timeline_dispatch_request_(ev);
        break;
      }

      case SyntheticEventType::CloseDevice: {
        timeline_activate_or_dispatch_(ev, /*allow_pending=*/true);
        break;
      }

      case SyntheticEventType::CreateStream: {
        timeline_dispatch_request_(ev);
        break;
      }

      case SyntheticEventType::DestroyStream: {
        timeline_activate_or_dispatch_(ev, /*allow_pending=*/true);
        break;
      }

      case SyntheticEventType::UpdateStreamPicture: {
        timeline_dispatch_request_(ev);
        break;
      }
      case SyntheticEventType::UpdateCapturePicture: {
        timeline_dispatch_request_(ev);
        break;
      }
    }
    const auto event_t1 = std::chrono::steady_clock::now();
    const uint64_t event_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(event_t1 - event_t0).count());
    record_timing_sample(
        event_ns,
        triage_timeline_event_exec_calls_,
        triage_timeline_event_exec_total_ns_,
        triage_timeline_event_exec_max_ns_);
  }

  if (emitted_this_pump > 0) {
    ++triage_catchup_bursts_total_;
    triage_catchup_max_frames_in_tick_ = std::max(triage_catchup_max_frames_in_tick_, emitted_this_pump);
  }

  if (!completion_gated_destructive_sequencing_enabled_ || timeline_pending_destructive_.empty()) {
    return;
  }

  std::vector<SyntheticScheduledEvent> still_pending;
  still_pending.reserve(timeline_pending_destructive_.size());
  for (const auto& ev : timeline_pending_destructive_) {
    const char* reason = nullptr;
    if (!timeline_destructive_prereq_ready_(ev, reason)) {
      still_pending.push_back(ev);
      continue;
    }
    timeline_activate_or_dispatch_(ev, /*allow_pending=*/false);
  }
  timeline_pending_destructive_.swap(still_pending);
}

void SyntheticProvider::set_timeline_request_dispatch_hook_for_host(TimelineRequestDispatchHook hook) {
  timeline_request_dispatch_hook_ = std::move(hook);
}

ProviderResult SyntheticProvider::enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  out_endpoints.clear();
  out_endpoints.reserve(cfg_.endpoint_count);
  for (uint32_t i = 0; i < cfg_.endpoint_count; ++i) {
    CameraEndpoint ep;
    ep.hardware_id = std::string(kHardwareIdPrefix) + std::to_string(i);
    ep.name = std::string("Synthetic Camera ") + std::to_string(i);
    out_endpoints.push_back(std::move(ep));
  }
  return ProviderResult::success();
}

bool SyntheticProvider::is_known_hardware_id_(const std::string& hardware_id) const {
  if (hardware_id.rfind(kHardwareIdPrefix, 0) != 0) {
    return false;
  }
  const char* p = hardware_id.c_str() + std::strlen(kHardwareIdPrefix);
  if (*p == '\0') {
    return false;
  }
  char* end = nullptr;
  const long idx = std::strtol(p, &end, 10);
  if (!end || *end != '\0' || idx < 0) {
    return false;
  }
  return static_cast<uint32_t>(idx) < effective_endpoint_count_();
}

uint32_t SyntheticProvider::effective_endpoint_count_() const noexcept {
  return std::max(cfg_.endpoint_count, staged_required_endpoint_count_);
}

uint64_t SyntheticProvider::alloc_native_id_(NativeObjectType type) {
  if (!callbacks_) {
    return 0;
  }
  return callbacks_->allocate_native_id(type);
}

void SyntheticProvider::emit_native_create_device_(const DeviceState& d) {
  if (!callbacks_) {
    return;
  }
  NativeObjectCreateInfo info{};
  info.native_id = d.native_id;
  info.type = static_cast<uint32_t>(NativeObjectType::Device);
  info.root_id = d.root_id;
  info.owner_device_instance_id = d.device_instance_id;
  info.owner_provider_native_id = provider_native_id_;
  info.owner_rig_id = 0;
  info.has_created_ns = true;
  info.created_ns = clock_.now_ns();
  strand_.post_native_object_created(info);
}

void SyntheticProvider::emit_native_destroy_(uint64_t native_id) {
  if (!callbacks_) {
    return;
  }
  NativeObjectDestroyInfo info{};
  info.native_id = native_id;
  info.has_destroyed_ns = true;
  info.destroyed_ns = clock_.now_ns();
  strand_.post_native_object_destroyed(info);
}

void SyntheticProvider::emit_camera_static_facts_(const DeviceState& d) {
  if (!callbacks_) return;
  const uint32_t device_index = static_cast<uint32_t>(
      std::strtoul(d.hardware_id.c_str() + std::strlen(kHardwareIdPrefix), nullptr, 10));
  const uint32_t width = cfg_.nominal.width;
  const uint32_t height = cfg_.nominal.height;
  const auto intrinsics = Intrinsics::create(
      static_cast<double>(width) * (1.0 + (0.01 * device_index)),
      static_cast<double>(height) * (1.0 + (0.01 * device_index)),
      static_cast<double>(width) / 2.0, static_cast<double>(height) / 2.0,
      std::nullopt, width, height, CoordinateDomain{CoordinateDomainDeliveredImage{}});
  const auto pose = CameraPose::create(
      PoseReference{PoseReferencePrimaryCamera{}},
      PoseConvention{PoseConventionCameraOpticalFrame{}},
      Vec3Meters{static_cast<double>(device_index), 0.0, 0.0},
      QuaternionXyzw{0.0, 0.0, 0.0, 1.0});
  if (!intrinsics || !pose) return;

  ProviderCameraFacts facts{};
  facts.static_facts.facing = SourcedFact<CameraFacing>{
      CameraFacing::EXTERNAL, FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  facts.static_facts.nature = SourcedFact<CameraNature>{
      CameraNature::VIRTUAL, FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  facts.static_facts.sensor_orientation = SourcedFact<SensorOrientationDegrees>{
      static_cast<SensorOrientationDegrees>((device_index % 4u) * 90u),
      FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  facts.static_facts.intrinsics = SourcedFact<Intrinsics>{
      *intrinsics, FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  facts.static_facts.distortion = SourcedFact<Distortion>{
      Distortion{NoDistortion{DistortionImageState::RECTIFIED}},
      FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  facts.static_facts.pose = SourcedFact<CameraPose>{
      *pose, FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  strand_.post_camera_static_facts(d.device_instance_id, std::move(facts));
}

void SyntheticProvider::emit_capture_image_facts_(
    const CaptureRequest& request,
    uint32_t image_member_index) {
  if (!callbacks_) return;
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  const char* hardware_id = resolve_hardware_id_for_device_locked_(request.device_instance_id);
  if (!hardware_id) return;
  const uint32_t device_index = static_cast<uint32_t>(
      std::strtoul(hardware_id + std::strlen(kHardwareIdPrefix), nullptr, 10));
  const auto intrinsics = Intrinsics::create(
      static_cast<double>(request.width) * (1.0 + (0.01 * device_index)),
      static_cast<double>(request.height) * (1.0 + (0.01 * device_index)),
      static_cast<double>(request.width) / 2.0, static_cast<double>(request.height) / 2.0,
      std::nullopt,
      request.width,
      request.height,
      CoordinateDomain{CoordinateDomainDeliveredImage{}});
  if (!intrinsics) return;
  ProviderCaptureImageFacts facts{};
  facts.intrinsics = SourcedFact<Intrinsics>{
      *intrinsics, FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  facts.distortion = SourcedFact<Distortion>{
      Distortion{NoDistortion{DistortionImageState::RECTIFIED}},
      FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  facts.focus_state = SourcedFact<FocusState>{
      FocusState{FocusAtInfinity{}}, FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  facts.realized_image_transform = SourcedFact<RealizedImageTransform>{
      RealizedImageTransform{
          ImageRotationDegrees::DEGREES_0,
          false,
          true},
      FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  strand_.post_capture_image_facts(
      request.capture_id, request.device_instance_id, image_member_index, std::move(facts));
}

uint64_t SyntheticProvider::ensure_native_acquisition_session_(DeviceState& d) {
  if (d.acquisition_session_native_id != 0) {
    return d.acquisition_session_native_id;
  }
  if (!callbacks_) {
    return 0;
  }

  const uint64_t native_id =
      alloc_native_id_(NativeObjectType::AcquisitionSession);
  if (native_id == 0) {
    return 0;
  }

  d.acquisition_session_native_id = native_id;

  NativeObjectCreateInfo info{};
  info.native_id = native_id;
  info.type = static_cast<uint32_t>(NativeObjectType::AcquisitionSession);
  info.root_id = d.root_id;
  info.owner_device_instance_id = d.device_instance_id;
  info.owner_provider_native_id = provider_native_id_;
  info.owner_rig_id = 0;
  info.has_created_ns = true;
  info.created_ns = clock_.now_ns();
  try {
    strand_.post_native_object_created(info);
  } catch (...) {
    // Admission has not retained this session yet. Do not leave an unpublished
    // native object installed in device state if the callback strand cannot
    // accept its create fact.
    d.acquisition_session_native_id = 0;
    throw;
  }
  return native_id;
}

void SyntheticProvider::retain_native_acquisition_session_for_stream_(DeviceState& d) {
  if (ensure_native_acquisition_session_(d) == 0) {
    return;
  }
  ++d.acquisition_session_stream_refs;
}

void SyntheticProvider::retain_native_acquisition_session_for_capture_(DeviceState& d) {
  if (ensure_native_acquisition_session_(d) == 0) {
    return;
  }
  ++d.acquisition_session_capture_refs;
}

void SyntheticProvider::retain_native_acquisition_session_for_priming_(DeviceState& d) {
  if (ensure_native_acquisition_session_(d) == 0) {
    return;
  }
  if (d.acquisition_session_priming_refs == 0) {
    d.acquisition_session_priming_refs = 1;
  }
}

void SyntheticProvider::release_native_acquisition_session_if_unheld_(DeviceState& d) {
  if (d.acquisition_session_stream_refs != 0 ||
      d.acquisition_session_capture_refs != 0 ||
      d.acquisition_session_priming_refs != 0) {
    return;
  }
  if (d.acquisition_session_native_id != 0) {
    emit_native_destroy_(d.acquisition_session_native_id);
    d.acquisition_session_native_id = 0;
  }
}

void SyntheticProvider::release_native_acquisition_session_for_stream_(uint64_t device_instance_id) {
  auto dit = devices_.find(device_instance_id);
  if (dit == devices_.end()) {
    return;
  }
  DeviceState& d = dit->second;
  if (d.acquisition_session_stream_refs == 0) {
    return;
  }
  --d.acquisition_session_stream_refs;
  release_native_acquisition_session_if_unheld_(d);
}

void SyntheticProvider::release_native_acquisition_session_for_capture_(uint64_t device_instance_id) {
  auto dit = devices_.find(device_instance_id);
  if (dit == devices_.end()) {
    return;
  }
  DeviceState& d = dit->second;
  if (d.acquisition_session_capture_refs == 0) {
    return;
  }
  --d.acquisition_session_capture_refs;
  release_native_acquisition_session_if_unheld_(d);
}

void SyntheticProvider::release_native_acquisition_session_for_priming_(uint64_t device_instance_id) {
  auto dit = devices_.find(device_instance_id);
  if (dit == devices_.end()) {
    return;
  }
  DeviceState& d = dit->second;
  if (d.acquisition_session_priming_refs == 0) {
    return;
  }
  d.acquisition_session_priming_refs = 0;
  release_native_acquisition_session_if_unheld_(d);
}

ProviderResult SyntheticProvider::open_device(
    const std::string& hardware_id,
    uint64_t device_instance_id,
    uint64_t root_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_SHUTTING_DOWN);
  }
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  if (!is_known_hardware_id_(hardware_id) || device_instance_id == 0 || root_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  auto& d = devices_[device_instance_id];
  if (d.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  d.hardware_id = hardware_id;
  d.device_instance_id = device_instance_id;
  d.root_id = root_id;
  d.open = true;
  d.native_id = alloc_native_id_(NativeObjectType::Device);
  d.capture_picture = capture_template().picture;

  emit_native_create_device_(d);
  strand_.post_device_opened(device_instance_id);
  emit_camera_static_facts_(d);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::close_device(uint64_t device_instance_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  {
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    if (has_in_flight_capture_for_device_locked_(device_instance_id)) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
  }
  // Known check-then-act window: the in-flight check above releases
  // capture_mutex_ before the close below takes provider_state_mutex_ (the
  // sequential order is required -- capture_mutex_ before provider_state_
  // mutex_, never nested the other way). A trigger_capture racing in from a
  // *different* thread could be admitted in that gap and produce a capture for
  // a device this call is about to erase. This is currently unreachable under
  // the intended-use contract: every mutating ICameraProvider entry point is
  // invoked from the single core thread (public commands, rig submission, and
  // the warm-expiry close all execute there), so close and trigger cannot
  // overlap. If a future caller drives provider mutations from more than one
  // thread, this window must be closed (e.g. re-check in-flight captures under
  // provider_state_mutex_ after admission also takes it) rather than relied on.
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  auto it = devices_.find(device_instance_id);
  if (it == devices_.end() || !it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // Normal operation is strict: stream lifecycle must be resolved explicitly
  // by the caller before device close.
  for (const auto& kv : streams_) {
    if (kv.second.created && kv.second.req.device_instance_id == device_instance_id) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
  }

  it->second.open = false;
  if (it->second.acquisition_session_native_id != 0) {
    emit_native_destroy_(it->second.acquisition_session_native_id);
    it->second.acquisition_session_native_id = 0;
    it->second.acquisition_session_stream_refs = 0;
    it->second.acquisition_session_capture_refs = 0;
    it->second.acquisition_session_priming_refs = 0;
  }
  strand_.post_device_closed(device_instance_id);
  emit_native_destroy_(it->second.native_id);
  devices_.erase(it);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::create_stream(const StreamRequest& req) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_SHUTTING_DOWN);
  }
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  if (req.stream_id == 0 || req.device_instance_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  auto dit = devices_.find(req.device_instance_id);
  if (dit == devices_.end() || !dit->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto& s = streams_[req.stream_id];
  if (s.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  s.req = req;
  s.picture = req.picture;
  s.pool.clear();
  s.pool_cursor = 0;

  // Provider must not apply implicit defaults; validate required fields.
  if (s.req.profile.width == 0 || s.req.profile.height == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (s.req.profile.format_fourcc == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  retain_native_acquisition_session_for_stream_(dit->second);
  s.acquisition_session_native_id = dit->second.acquisition_session_native_id;

  s.created = true;
  s.started = false;
  s.next_due_ns = 0;
  s.native_id = alloc_native_id_(NativeObjectType::Stream);

  // Report stream native object.
  if (callbacks_) {
    NativeObjectCreateInfo info{};
    info.native_id = s.native_id;
    info.type = static_cast<uint32_t>(NativeObjectType::Stream);
    info.root_id = dit->second.root_id;
    info.owner_device_instance_id = req.device_instance_id;
    info.owner_acquisition_session_id = s.acquisition_session_native_id;
    info.owner_stream_id = req.stream_id;
    info.owner_provider_native_id = provider_native_id_;
    info.owner_rig_id = 0;
    info.has_created_ns = true;
    info.created_ns = clock_.now_ns();
    strand_.post_native_object_created(info);
  }

  strand_.post_stream_created(req.stream_id);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::destroy_stream(uint64_t stream_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (it->second.started || it->second.producing) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  emit_native_destroy_(it->second.native_id);
  release_native_acquisition_session_for_stream_(it->second.req.device_instance_id);
  strand_.post_stream_destroyed(stream_id);
  streams_.erase(it);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::start_stream(
    uint64_t stream_id,
    const CaptureProfile& profile,
    const PictureConfig& picture) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_SHUTTING_DOWN);
  }
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  auto& s = it->second;
  s.req.profile = profile;
  s.picture = picture;
  release_stream_live_gpu_backing_(s);

  const uint32_t w = profile.width;
  const uint32_t h = profile.height;
  const uint32_t fmt = profile.format_fourcc;
  if (w == 0 || h == 0 || fmt == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (fmt != FOURCC_RGBA) {
    // v1 synthetic only.
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }

  constexpr size_t kPoolSize = 8;
  const uint32_t stride = w * 4u;
  const size_t size_bytes = static_cast<size_t>(stride) * static_cast<size_t>(h);
  const size_t existing0 = (s.pool.empty() || !s.pool[0]) ? 0 : s.pool[0]->bytes.size();
  if (s.pool.size() != kPoolSize || existing0 != size_bytes) {
    s.pool.clear();
    s.pool.reserve(kPoolSize);
    for (size_t i = 0; i < kPoolSize; ++i) {
      auto slot = std::make_shared<SyntheticProvider::StreamState::BufferSlot>();
      slot->stream_id = stream_id;
      slot->bytes.resize(size_bytes);
      slot->in_use.store(false, std::memory_order_relaxed);
      s.pool.emplace_back(std::move(slot));
    }
    s.pool_cursor = 0;
  }
  const ProducerBackingCapabilities runtime_truth =
      stream_parent_context_backing_capabilities_locked_(
          s.req.device_instance_id, s.req.intent, profile, picture);
  const bool selected_mode_requires_gpu =
      cfg_.producer_output_form_mode == SyntheticProducerOutputFormMode::GpuOnly;
  if (selected_mode_requires_gpu && !runtime_truth.gpu_backed_available) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  s.resolved_output_form_mode = resolve_producer_output_form_mode_(runtime_truth);
  if (s.req.requested_retained_plan.valid) {
    s.resolved_output_form_mode = s.req.requested_retained_plan.primary_cpu()
        ? SyntheticProducerOutputFormMode::CpuOnly
        : (s.req.requested_retained_plan.retain_cpu_sidecar()
            ? SyntheticProducerOutputFormMode::CpuAndGpu
            : SyntheticProducerOutputFormMode::GpuOnly);
  }
  s.prefer_gpu_backing = s.resolved_output_form_mode == SyntheticProducerOutputFormMode::GpuOnly ||
                         s.resolved_output_form_mode == SyntheticProducerOutputFormMode::CpuAndGpu;
  s.gpu_staging.resize(size_bytes);
  {
    bool preset_valid = true;
    s.render_spec = build_stream_render_spec(s.picture, w, h, &preset_valid);
    s.render_spec_valid = true;
    s.renderer.configure(s.render_spec);
    if (!preset_valid) {
      invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  s.started = true;
  s.producing = true;

  // First capture timestamp is scheduled (not wall-clock).
  s.next_due_ns = clock_.now_ns() + cfg_.nominal.start_stream_warmup_ns;
  if (cfg_.synthetic_role == SyntheticRole::Timeline) {
    // Timeline role: drive emission via explicit scheduled events.
    timeline_schedule_(s.next_due_ns, SyntheticEventType::EmitFrame, stream_id);
  }
  strand_.post_stream_started(stream_id);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::stop_stream(uint64_t stream_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto& s = it->second;
  s.started = false;
  if (s.producing) {
    // Production has stopped immediately in this provider.
    s.producing = false;
  }
  strand_.post_stream_stopped(stream_id, ProviderError::OK);
  release_stream_live_gpu_backing_(s);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::update_stream_retained_production_plan(
    uint64_t stream_id,
    CoreRetainedProductionPlan requested_retained_plan) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!requested_retained_plan.valid) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  StreamState& s = it->second;
  const ProducerBackingCapabilities runtime_truth =
      stream_parent_context_backing_capabilities_locked_(
          s.req.device_instance_id,
          s.req.intent,
          s.req.profile,
          s.picture);
  if (!runtime_truth.viable(requested_retained_plan.posture)) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  s.req.requested_retained_plan = requested_retained_plan;
  s.resolved_output_form_mode = requested_retained_plan.primary_cpu()
      ? SyntheticProducerOutputFormMode::CpuOnly
      : (requested_retained_plan.retain_cpu_sidecar()
          ? SyntheticProducerOutputFormMode::CpuAndGpu
          : SyntheticProducerOutputFormMode::GpuOnly);
  s.prefer_gpu_backing =
      s.resolved_output_form_mode == SyntheticProducerOutputFormMode::GpuOnly ||
      s.resolved_output_form_mode == SyntheticProducerOutputFormMode::CpuAndGpu;
  if (!s.prefer_gpu_backing) {
    release_stream_live_gpu_backing_(s);
  }
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (!find_preset_info(picture.preset)) {
    invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
  }
  StreamState& s = it->second;
  s.picture = picture;
  if (s.started) {
    bool preset_valid = true;
    s.render_spec = build_stream_render_spec(
        s.picture,
        s.req.profile.width,
        s.req.profile.height,
        &preset_valid);
    s.render_spec_valid = true;
    s.renderer.configure(s.render_spec);
    if (!preset_valid) {
      invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
    }
  } else {
    s.render_spec_valid = false;
  }
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::set_capture_picture_config(uint64_t device_instance_id, const PictureConfig& picture) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  auto it = devices_.find(device_instance_id);
  if (it == devices_.end() || !it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (!find_preset_info(picture.preset)) {
    invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
  }
  it->second.capture_picture = picture;
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::sync_capture_parent_priming(const CaptureRequest& req) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (req.device_instance_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  auto it = devices_.find(req.device_instance_id);
  if (it == devices_.end() || !it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  retain_native_acquisition_session_for_priming_(it->second);
  return (it->second.acquisition_session_native_id != 0)
      ? ProviderResult::success()
      : ProviderResult::failure(ProviderError::ERR_BAD_STATE);
}

ProviderResult SyntheticProvider::release_capture_parent_priming(uint64_t device_instance_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (device_instance_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  auto it = devices_.find(device_instance_id);
  if (it == devices_.end() || !it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  release_native_acquisition_session_for_priming_(device_instance_id);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::trigger_capture(const CaptureRequest& req) {
  CaptureSubmission submission{};
  submission.capture_id = req.capture_id;
  submission.origin = req.rig_id == 0 ? CaptureSubmissionOrigin::DEVICE_CAPTURE : CaptureSubmissionOrigin::RIG_CAPTURE;
  submission.rig_id = req.rig_id;
  submission.device_requests.push_back(req);
  return trigger_capture_submission(submission);
}

ProviderResult SyntheticProvider::trigger_capture_submission(const CaptureSubmission& submission) {
  CaptureSubmissionJob job{};
  ProviderResult admission_result = ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  CaptureAdmissionFailureInfo failure_info{};
  {
    std::unique_lock<std::mutex> capture_lock(capture_mutex_);
    std::unique_lock<std::mutex> state_lock(provider_state_mutex_);
    try {
      admission_result =
          validate_and_admit_capture_submission_locked_(submission, job, &failure_info);
    } catch (...) {
      rollback_capture_submission_locked_(job);
      return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
    }
    if (!admission_result.ok()) {
      return admission_result;
    }
    enqueue_capture_submission_locked_(job);
  }
  capture_cv_.notify_all();
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::validate_and_admit_capture_submission_locked_(
    const CaptureSubmission& submission,
    CaptureSubmissionJob& out_job,
    CaptureAdmissionFailureInfo* failure_info) {
  auto set_failure_info =
      [&](const char* reason,
          uint64_t device_instance_id,
          const DeviceState* device_state) {
        if (!failure_info) {
          return;
        }
        failure_info->reason = reason;
        failure_info->device_instance_id = device_instance_id;
        failure_info->device_present = device_state != nullptr;
        failure_info->device_open = device_state != nullptr && device_state->open;
        failure_info->acquisition_session_native_id =
            device_state ? device_state->acquisition_session_native_id : 0;
        failure_info->acquisition_session_stream_refs =
            device_state ? device_state->acquisition_session_stream_refs : 0;
        failure_info->acquisition_session_capture_refs =
            device_state ? device_state->acquisition_session_capture_refs : 0;
        failure_info->acquisition_session_priming_refs =
            device_state ? device_state->acquisition_session_priming_refs : 0;
      };
  if (!initialized_ || shutting_down_ || capture_admission_closed_) {
    set_failure_info("provider_not_accepting_captures", 0, nullptr);
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (submission.capture_id == 0 || submission.device_requests.empty()) {
    set_failure_info("invalid_submission_shape", 0, nullptr);
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (submission.origin == CaptureSubmissionOrigin::RIG_CAPTURE && submission.rig_id == 0) {
    set_failure_info("rig_submission_missing_rig_id", 0, nullptr);
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  CaptureSubmissionJob job{};
  job.capture_id = submission.capture_id;
  job.origin = submission.origin;
  job.rig_id = submission.rig_id;
  job.generation = capture_generation_;
  job.device_jobs.reserve(submission.device_requests.size());

  std::map<uint64_t, bool> seen_devices;
  for (const CaptureRequest& req : submission.device_requests) {
    if (req.capture_id != submission.capture_id || req.device_instance_id == 0 || req.width == 0 || req.height == 0) {
      set_failure_info("invalid_request_shape", req.device_instance_id, nullptr);
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    if (submission.origin == CaptureSubmissionOrigin::RIG_CAPTURE) {
      if (req.rig_id != submission.rig_id || req.rig_id == 0) {
        set_failure_info("rig_request_mismatch", req.device_instance_id, nullptr);
        return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
      }
    } else if (req.rig_id != 0 || submission.rig_id != 0) {
      set_failure_info("device_request_has_rig_fields", req.device_instance_id, nullptr);
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    if (seen_devices.find(req.device_instance_id) != seen_devices.end()) {
      set_failure_info("duplicate_device_in_submission", req.device_instance_id, nullptr);
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    seen_devices.emplace(req.device_instance_id, true);

    auto dev_it = devices_.find(req.device_instance_id);
    if (dev_it == devices_.end() || !dev_it->second.open) {
      const DeviceState* device_state =
          dev_it == devices_.end() ? nullptr : &dev_it->second;
      set_failure_info("device_not_open_during_precheck", req.device_instance_id, device_state);
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }

    const uint32_t fmt = req.format_fourcc == 0 ? FOURCC_RGBA : req.format_fourcc;
    if (!(fmt == FOURCC_RGBA || fmt == FOURCC_BGRA)) {
      set_failure_info("unsupported_capture_format", req.device_instance_id, &dev_it->second);
      return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
    }
    if (req.width > (std::numeric_limits<uint32_t>::max() / 4u)) {
      set_failure_info("capture_dimensions_overflow", req.device_instance_id, &dev_it->second);
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    const uint32_t stride_bytes = req.width * 4u;
    if (static_cast<size_t>(req.height) >
        (std::numeric_limits<size_t>::max() /
         static_cast<size_t>(stride_bytes))) {
      set_failure_info(
          "capture_frame_size_overflow",
          req.device_instance_id,
          &dev_it->second);
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    if (!is_valid_capture_still_image_bundle(req.still_image_bundle, supports_multi_image_still_sequence())) {
      set_failure_info("invalid_still_image_bundle", req.device_instance_id, &dev_it->second);
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    const ProducerBackingCapabilities capture_truth =
        capture_parent_context_backing_capabilities_locked_(
            req.device_instance_id, req);
    const bool selected_mode_requires_gpu =
        cfg_.producer_output_form_mode == SyntheticProducerOutputFormMode::GpuOnly;
    if (selected_mode_requires_gpu && !capture_truth.gpu_backed_available) {
      set_failure_info("gpu_only_capture_not_supported", req.device_instance_id, &dev_it->second);
      return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
    }

    DeviceCaptureJob device_job{};
    device_job.request = req;
    device_job.format_fourcc = fmt;
    device_job.stride_bytes = stride_bytes;
    device_job.frame_size_bytes = static_cast<size_t>(device_job.stride_bytes) * static_cast<size_t>(req.height);
    device_job.capture_timestamp_ns = clock_.now_ns();
    device_job.output_form_mode = resolve_producer_output_form_mode_(capture_truth);
    if (req.requested_retained_plan.valid) {
      device_job.output_form_mode = req.requested_retained_plan.primary_cpu()
          ? SyntheticProducerOutputFormMode::CpuOnly
          : (req.requested_retained_plan.retain_cpu_sidecar()
              ? SyntheticProducerOutputFormMode::CpuAndGpu
              : SyntheticProducerOutputFormMode::GpuOnly);
    }
    job.device_jobs.push_back(std::move(device_job));
  }

  if (job.device_jobs.size() >
      (kCaptureQueueCapacity - capture_queue_count_)) {
    set_failure_info("capture_executor_saturated", 0, nullptr);
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }

  for (const DeviceCaptureJob& device_job : job.device_jobs) {
    const InFlightCaptureKey key{
        device_job.request.capture_id,
        device_job.request.device_instance_id};
    if (in_flight_captures_.find(key) != in_flight_captures_.end()) {
      auto dev_it = devices_.find(device_job.request.device_instance_id);
      const DeviceState* device_state =
          dev_it == devices_.end() ? nullptr : &dev_it->second;
      set_failure_info(
          "duplicate_inflight_capture_key",
          device_job.request.device_instance_id,
          device_state);
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
  }

  try {
    for (DeviceCaptureJob& device_job : job.device_jobs) {
      auto dev_it = devices_.find(device_job.request.device_instance_id);
      if (dev_it == devices_.end() || !dev_it->second.open) {
        const DeviceState* device_state =
            dev_it == devices_.end() ? nullptr : &dev_it->second;
        rollback_capture_submission_locked_(job);
        set_failure_info(
            "device_not_open_during_retain",
            device_job.request.device_instance_id,
            device_state);
        return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
      }
      retain_native_acquisition_session_for_capture_(dev_it->second);
      if (dev_it->second.acquisition_session_capture_refs == 0 ||
          dev_it->second.acquisition_session_native_id == 0) {
        rollback_capture_submission_locked_(job);
        set_failure_info(
            "capture_session_retain_failed",
            device_job.request.device_instance_id,
            &dev_it->second);
        return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
      }
      device_job.acquisition_session_id =
          dev_it->second.acquisition_session_native_id;
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
      if (next_capture_admission_fault_for_test_ ==
          CaptureAdmissionFaultForTest::AfterFirstRetain) {
        next_capture_admission_fault_for_test_ =
            CaptureAdmissionFaultForTest::None;
        throw std::bad_alloc{};
      }
#endif
    }
  } catch (...) {
    rollback_capture_submission_locked_(job);
    set_failure_info("capture_session_retain_exception", 0, nullptr);
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  try {
    for (const DeviceCaptureJob& device_job : job.device_jobs) {
      const InFlightCaptureKey key{
          device_job.request.capture_id,
          device_job.request.device_instance_id};
      InFlightCaptureDevice in_flight{};
      in_flight.capture_id = device_job.request.capture_id;
      in_flight.device_instance_id = device_job.request.device_instance_id;
      in_flight.acquisition_session_id = device_job.acquisition_session_id;
      in_flight.generation = job.generation;
      const auto inserted = in_flight_captures_.emplace(key, in_flight);
      if (!inserted.second) {
        rollback_capture_submission_locked_(job);
        set_failure_info(
            "duplicate_inflight_capture_key",
            device_job.request.device_instance_id,
            nullptr);
        return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
      }
      ++capture_pause_depth_by_device_[device_job.request.device_instance_id];
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
      if (next_capture_admission_fault_for_test_ ==
          CaptureAdmissionFaultForTest::AfterFirstRegistration) {
        next_capture_admission_fault_for_test_ =
            CaptureAdmissionFaultForTest::None;
        throw std::bad_alloc{};
      }
#endif
    }
  } catch (...) {
    rollback_capture_submission_locked_(job);
    set_failure_info("capture_admission_allocation_failed", 0, nullptr);
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  out_job = std::move(job);
  return ProviderResult::success();
}

void SyntheticProvider::rollback_capture_submission_locked_(
    CaptureSubmissionJob& job) noexcept {
  for (DeviceCaptureJob& device_job : job.device_jobs) {
    const InFlightCaptureKey key{
        device_job.request.capture_id,
        device_job.request.device_instance_id};
    const bool had_in_flight = in_flight_captures_.erase(key) != 0;
    if (had_in_flight) {
      auto pause_it = capture_pause_depth_by_device_.find(
          device_job.request.device_instance_id);
      if (pause_it != capture_pause_depth_by_device_.end()) {
        if (pause_it->second > 1) {
          --pause_it->second;
        } else {
          capture_pause_depth_by_device_.erase(pause_it);
        }
      }
    }
    if (device_job.acquisition_session_id != 0) {
      try {
        release_native_acquisition_session_for_capture_(
            device_job.request.device_instance_id);
      } catch (...) {
        std::fprintf(
            stderr,
            "[CamBANG][SyntheticProvider] exception while rolling back capture-session retain\n");
      }
      device_job.acquisition_session_id = 0;
    }
  }
}

bool SyntheticProvider::should_stop_capture_job_(uint64_t generation) const noexcept {
  std::lock_guard<std::mutex> capture_lock(capture_mutex_);
  return capture_admission_closed_ || generation != capture_generation_;
}

void SyntheticProvider::enqueue_capture_submission_locked_(
    CaptureSubmissionJob& job) noexcept {
  static_assert(
      std::is_nothrow_move_constructible_v<CaptureWorkItem>,
      "capture queue admission must not fail after provider state commits");
  for (DeviceCaptureJob& device_job : job.device_jobs) {
    capture_queue_[capture_queue_tail_].emplace(
        CaptureWorkItem{std::move(device_job), job.generation});
    capture_queue_tail_ =
        (capture_queue_tail_ + 1) % kCaptureQueueCapacity;
    ++capture_queue_count_;
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
    capture_max_queued_jobs_for_test_ =
        std::max(capture_max_queued_jobs_for_test_, capture_queue_count_);
#endif
  }
}

SyntheticProvider::CaptureWorkItem
SyntheticProvider::dequeue_capture_work_locked_() noexcept {
  std::optional<CaptureWorkItem>& slot = capture_queue_[capture_queue_head_];
  CaptureWorkItem item = std::move(*slot);
  slot.reset();
  capture_queue_head_ =
      (capture_queue_head_ + 1) % kCaptureQueueCapacity;
  --capture_queue_count_;
  return item;
}

bool SyntheticProvider::start_capture_executor_() noexcept {
  std::vector<std::thread> workers;
  try {
    {
      std::lock_guard<std::mutex> capture_lock(capture_mutex_);
      if (!capture_workers_.empty() || capture_queue_count_ != 0 ||
          capture_active_jobs_ != 0 || !in_flight_captures_.empty()) {
        return false;
      }
      capture_admission_closed_ = true;
      capture_executor_stop_requested_ = false;
      capture_queue_head_ = 0;
      capture_queue_tail_ = 0;
      ++capture_generation_;
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
      capture_workers_paused_for_test_ = false;
      capture_jobs_paused_after_dequeue_for_test_ = false;
      next_capture_admission_fault_for_test_ =
          CaptureAdmissionFaultForTest::None;
      next_capture_worker_fault_for_test_ =
          CaptureWorkerFaultForTest::None;
      capture_max_queued_jobs_for_test_ = 0;
      capture_max_active_jobs_for_test_ = 0;
#endif
    }

    workers.reserve(kCaptureWorkerCount);
    for (size_t i = 0; i < kCaptureWorkerCount; ++i) {
      workers.emplace_back([this]() { capture_worker_main_(); });
    }

    {
      std::lock_guard<std::mutex> capture_lock(capture_mutex_);
      capture_workers_ = std::move(workers);
      capture_admission_closed_ = false;
    }
    return true;
  } catch (...) {
    {
      std::lock_guard<std::mutex> capture_lock(capture_mutex_);
      capture_admission_closed_ = true;
      capture_executor_stop_requested_ = true;
    }
    capture_cv_.notify_all();
    for (std::thread& worker : workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    return false;
  }
}

void SyntheticProvider::capture_worker_main_() noexcept {
  while (true) {
    CaptureWorkItem item{};
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
    CaptureWorkerFaultForTest fault = CaptureWorkerFaultForTest::None;
#endif
    {
      std::unique_lock<std::mutex> capture_lock(capture_mutex_);
      capture_cv_.wait(capture_lock, [&]() {
        return capture_executor_stop_requested_ ||
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
               (!capture_workers_paused_for_test_ &&
                capture_queue_count_ != 0);
#else
               capture_queue_count_ != 0;
#endif
      });
      if (capture_executor_stop_requested_ && capture_queue_count_ == 0) {
        return;
      }
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
      if (capture_workers_paused_for_test_ &&
          !capture_executor_stop_requested_) {
        continue;
      }
#endif
      item = dequeue_capture_work_locked_();
      ++capture_active_jobs_;
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
      capture_max_active_jobs_for_test_ =
          std::max(capture_max_active_jobs_for_test_, capture_active_jobs_);
      capture_cv_.wait(capture_lock, [&]() {
        return capture_executor_stop_requested_ ||
               !capture_jobs_paused_after_dequeue_for_test_;
      });
      fault = next_capture_worker_fault_for_test_;
      next_capture_worker_fault_for_test_ =
          CaptureWorkerFaultForTest::None;
#endif
    }

    try {
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
      if (fault == CaptureWorkerFaultForTest::StandardException) {
        throw std::runtime_error("injected capture worker failure");
      }
      if (fault == CaptureWorkerFaultForTest::NonStandardException) {
        throw 1;
      }
#endif
      run_device_capture_job_(item.job, item.generation);
    } catch (const std::exception& e) {
      std::fprintf(
          stderr,
          "[CamBANG][SyntheticProvider] capture worker exception: %s\n",
          e.what());
      try {
        finish_device_capture_job_(
            item.job,
            item.generation,
            CaptureTerminalKind::Failed,
            ProviderError::ERR_PROVIDER_FAILED);
      } catch (...) {
        std::fprintf(
            stderr,
            "[CamBANG][SyntheticProvider] exception while terminalizing failed capture worker job\n");
      }
    } catch (...) {
      std::fprintf(
          stderr,
          "[CamBANG][SyntheticProvider] non-standard capture worker exception\n");
      try {
        finish_device_capture_job_(
            item.job,
            item.generation,
            CaptureTerminalKind::Failed,
            ProviderError::ERR_PROVIDER_FAILED);
      } catch (...) {
        std::fprintf(
            stderr,
            "[CamBANG][SyntheticProvider] exception while terminalizing failed capture worker job\n");
      }
    }

    {
      std::lock_guard<std::mutex> capture_lock(capture_mutex_);
      if (capture_active_jobs_ > 0) {
        --capture_active_jobs_;
      }
    }
    capture_cv_.notify_all();
  }
}

void SyntheticProvider::run_device_capture_job_(const DeviceCaptureJob& job, uint64_t generation) {
  if (should_stop_capture_job_(generation)) {
    finish_device_capture_job_(job, generation, CaptureTerminalKind::Failed, ProviderError::ERR_SHUTTING_DOWN);
    return;
  }

  std::shared_ptr<std::vector<std::uint8_t>> deferred_cpu_staging_bytes{};
  try {
    const bool ok = generate_device_capture_payloads_(
        job, generation, &deferred_cpu_staging_bytes);
    if (ok) {
      finish_device_capture_job_(job,
                                 generation,
                                 CaptureTerminalKind::Completed,
                                 ProviderError::OK,
                                 std::move(deferred_cpu_staging_bytes));
    } else {
      finish_device_capture_job_(job,
                                 generation,
                                 CaptureTerminalKind::Failed,
                                 ProviderError::ERR_SHUTTING_DOWN,
                                 std::move(deferred_cpu_staging_bytes));
    }
  } catch (const std::exception&) {
    finish_device_capture_job_(job,
                               generation,
                               CaptureTerminalKind::Failed,
                               ProviderError::ERR_PROVIDER_FAILED,
                               std::move(deferred_cpu_staging_bytes));
  }
}

bool SyntheticProvider::generate_device_capture_payloads_(
    const DeviceCaptureJob& job,
    uint64_t generation,
    std::shared_ptr<std::vector<std::uint8_t>>* deferred_cpu_staging_bytes) {
  uint64_t staging_alloc_ns = 0;
  uint64_t before_first_member_ns = 0;
  uint64_t member_iteration_gap_ns = 0;
  uint64_t spec_setup_ns = 0;
  uint64_t timestamp_lock_wait_ns = 0;
  uint64_t base_render_ns = 0;
  uint64_t member_alloc_ns = 0;
  uint64_t member_copy_ns = 0;
  uint64_t member_ev_bgra_ns = 0;
  uint64_t capture_gpu_backing_retain_ns = 0;
  uint64_t member_frame_assembly_ns = 0;
  uint64_t member_post_ns = 0;
  const CaptureRequest& req = job.request;
  if (should_stop_capture_job_(generation)) {
    return false;
  }

  const uint64_t staging_begin_ns = provider_monotonic_now_ns();
  auto base_bytes = std::make_shared<std::vector<std::uint8_t>>();
  base_bytes->resize(job.frame_size_bytes);
  staging_alloc_ns = provider_monotonic_now_ns() - staging_begin_ns;

  const uint64_t spec_begin_ns = provider_monotonic_now_ns();
  bool preset_valid = true;
  PatternSpec spec = to_pattern_spec(req.picture, req.width, req.height, PatternSpec::PackedFormat::RGBA8, &preset_valid);
  if (!preset_valid) {
    invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
  }

  PatternRenderTarget dst{};
  dst.data = base_bytes->data();
  dst.size_bytes = base_bytes->size();
  dst.width = req.width;
  dst.height = req.height;
  dst.stride_bytes = job.stride_bytes;
  dst.format = PatternSpec::PackedFormat::RGBA8;
  spec_setup_ns = provider_monotonic_now_ns() - spec_begin_ns;

  const uint64_t capture_ts_ns = job.capture_timestamp_ns;
  // The virtual-clock sample is captured transactionally at admission while
  // provider_state_mutex_ is held; workers consume only this immutable value.
  timestamp_lock_wait_ns = 0;
  PatternOverlayData ov{};
  ov.frame_index = generator_frame_ordinal_from_ns_(capture_ts_ns, req.picture);
  ov.timestamp_ns = capture_ts_ns;
  ov.stream_id = 0;

  CpuPackedPatternRenderer renderer{};
  const uint64_t base_render_begin_ns = provider_monotonic_now_ns();
  renderer.render_into(spec, dst, ov);
  base_render_ns = provider_monotonic_now_ns() - base_render_begin_ns;
  if (should_stop_capture_job_(generation)) {
    return false;
  }

  const SyntheticProducerOutputFormMode output_form_mode =
      job.output_form_mode;
  auto* ready_stage_metrics = &triage_capture_ready_stage_cpu_primary_;
  if (output_form_mode == SyntheticProducerOutputFormMode::GpuOnly) {
    ready_stage_metrics =
        &triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_;
  } else if (output_form_mode == SyntheticProducerOutputFormMode::CpuAndGpu ||
             output_form_mode == SyntheticProducerOutputFormMode::Auto) {
    ready_stage_metrics =
        &triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_;
  }

  const uint64_t provider_post_capture_started_steady_ns =
      provider_monotonic_now_ns();
  strand_.post_capture_started(req.capture_id, req.device_instance_id);
  {
    std::lock_guard<std::mutex> triage_lock(
        triage_capture_ready_metrics_mutex_);
    ++ready_stage_metrics->calls;
    ready_stage_metrics->pre_capture_started_total_ns +=
        staging_alloc_ns + spec_setup_ns + timestamp_lock_wait_ns +
        base_render_ns;
    CaptureReadyTimingRecord& timing_record =
        capture_ready_timing_record_(req.capture_id, req.device_instance_id);
    timing_record.capture_id = req.capture_id;
    timing_record.device_instance_id = req.device_instance_id;
    timing_record.acquisition_session_id = job.acquisition_session_id;
    timing_record.primary_cpu =
        output_form_mode == SyntheticProducerOutputFormMode::CpuOnly;
    timing_record.retain_cpu_sidecar =
        output_form_mode == SyntheticProducerOutputFormMode::CpuAndGpu ||
        output_form_mode == SyntheticProducerOutputFormMode::Auto;
    timing_record.has_provider_post_capture_started_steady_ns = true;
    timing_record.provider_post_capture_started_steady_ns =
        provider_post_capture_started_steady_ns;
    timing_record.provider_pre_capture_started_total_ns =
        staging_alloc_ns + spec_setup_ns + timestamp_lock_wait_ns +
        base_render_ns;
  }
  const auto& members = req.still_image_bundle.members;
  uint64_t last_member_post_end_ns = provider_post_capture_started_steady_ns;
  for (size_t i = 0; i < members.size(); ++i) {
    const uint64_t member_span_begin_ns = provider_monotonic_now_ns();
    if (i == 0) {
      before_first_member_ns +=
          member_span_begin_ns - provider_post_capture_started_steady_ns;
    } else if (member_span_begin_ns >= last_member_post_end_ns) {
      member_iteration_gap_ns += member_span_begin_ns - last_member_post_end_ns;
    }
    if (should_stop_capture_job_(generation)) {
      return false;
    }
    uint64_t member_cpu_prep_ns = 0;
    uint64_t member_gpu_retain_ns = 0;
    uint64_t member_frame_assembly_sample_ns = 0;
    uint64_t member_post_sample_ns = 0;
    const auto& member = members[i];
    const bool can_reuse_base_for_default =
        i == 0 &&
        member.intended_exposure_compensation_milli_ev == 0 &&
        job.format_fourcc == FOURCC_RGBA;
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
    if (can_reuse_base_for_default) {
      bytes = base_bytes;
    } else {
      const bool needs_exposure_adjustment =
          member.intended_exposure_compensation_milli_ev != 0;
      const bool needs_bgra_swizzle = job.format_fourcc == FOURCC_BGRA;
      if (!needs_exposure_adjustment && !needs_bgra_swizzle) {
        // Copy-construct from the immutable base render: one pass into
        // uninitialized storage. The previous resize()-then-memcpy shape
        // value-initialized (zero-filled) the whole multi-MB buffer first and
        // then overwrote every byte, doubling memory traffic per plain bracket
        // member on the capture-latency path.
        const uint64_t member_copy_begin_ns = provider_monotonic_now_ns();
        bytes = std::make_shared<std::vector<std::uint8_t>>(*base_bytes);
        const uint64_t member_copy_sample_ns =
            provider_monotonic_now_ns() - member_copy_begin_ns;
        member_copy_ns += member_copy_sample_ns;
        member_cpu_prep_ns += member_copy_sample_ns;
      } else {
        const uint64_t member_alloc_begin_ns = provider_monotonic_now_ns();
        bytes = std::make_shared<std::vector<std::uint8_t>>();
        bytes->resize(job.frame_size_bytes);
        const uint64_t member_alloc_sample_ns =
            provider_monotonic_now_ns() - member_alloc_begin_ns;
        member_alloc_ns += member_alloc_sample_ns;
        member_cpu_prep_ns += member_alloc_sample_ns;
        // Synthetic still generation can fold exposure-variant synthesis and
        // optional FourCC mapping into one pass because this provider owns the
        // source pixels. That is an implementation detail of SyntheticProvider,
        // not a rule that platform-backed providers must synthesize frames the
        // same way.
        const uint64_t member_adjust_begin_ns = provider_monotonic_now_ns();
        copy_rgba8_with_optional_adjustments(
            bytes->data(),
            job.stride_bytes,
            base_bytes->data(),
            job.stride_bytes,
            req.width,
            req.height,
            needs_bgra_swizzle,
            member.intended_exposure_compensation_milli_ev);
        const uint64_t member_adjust_sample_ns =
            provider_monotonic_now_ns() - member_adjust_begin_ns;
        member_ev_bgra_ns += member_adjust_sample_ns;
        member_cpu_prep_ns += member_adjust_sample_ns;
      }
    }

    FrameView fv{};
    fv.device_instance_id = req.device_instance_id;
    fv.stream_id = 0;
    fv.acquisition_session_id = job.acquisition_session_id;
    fv.capture_id = req.capture_id;
    fv.width = req.width;
    fv.height = req.height;
    fv.format_fourcc = job.format_fourcc;
    if (const auto timing = synthetic_acquisition_timing_from_unsigned(
            capture_ts_ns,
            ImageAcquisitionReferenceEvent::PROVIDER_OBSERVED,
            ImageAcquisitionComparability::SAME_PROVIDER)) {
      fv.acquisition_timing =
          SourcedFact<ImageAcquisitionTiming>{*timing, FactOrigin::VIRTUAL_CAMERA_AUTHORED};
    }
    fv.capture_image.routing = (i == 0)
        ? CaptureImageRouting::DEFAULT_METERED
        : CaptureImageRouting::ADDITIONAL_BRACKET;
    fv.capture_image.image_member_index = member.image_member_index;
    fv.capture_image.applied_exposure_compensation_milli_ev = member.intended_exposure_compensation_milli_ev;
    fv.capture_image.has_realized_exposure_compensation_milli_ev = true;
    {
      const auto realized_known_override_it =
          cfg_.verification_has_realized_exposure_compensation_override_by_member_index.find(member.image_member_index);
      if (realized_known_override_it !=
              cfg_.verification_has_realized_exposure_compensation_override_by_member_index.end() &&
          !realized_known_override_it->second) {
        fv.capture_image.has_realized_exposure_compensation_milli_ev = false;
      }
    }
    {
      const auto realized_override_it =
          cfg_.verification_realized_exposure_compensation_override_by_member_index.find(member.image_member_index);
      if (fv.capture_image.has_realized_exposure_compensation_milli_ev &&
          realized_override_it != cfg_.verification_realized_exposure_compensation_override_by_member_index.end()) {
        fv.capture_image.realized_exposure_compensation_milli_ev = realized_override_it->second;
      } else {
        fv.capture_image.realized_exposure_compensation_milli_ev = fv.capture_image.applied_exposure_compensation_milli_ev;
      }
    }
    const bool retain_cpu_payload = output_form_mode != SyntheticProducerOutputFormMode::GpuOnly;
    std::shared_ptr<void> gpu_backing{};
    if (output_form_mode == SyntheticProducerOutputFormMode::GpuOnly ||
        output_form_mode == SyntheticProducerOutputFormMode::CpuAndGpu) {
      auto* posture_metrics = &triage_capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar_;
      if (output_form_mode == SyntheticProducerOutputFormMode::GpuOnly) {
        posture_metrics = &triage_capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar_;
      }
      const uint64_t gpu_backing_retain_begin_ns = provider_monotonic_now_ns();
      gpu_backing = synthetic_gpu_backing_retain_primary_gpu_backing_rgba8(
          bytes->data(), req.width, req.height, job.stride_bytes);
      const uint64_t gpu_backing_retain_sample_ns =
          provider_monotonic_now_ns() - gpu_backing_retain_begin_ns;
      capture_gpu_backing_retain_ns += gpu_backing_retain_sample_ns;
      member_gpu_retain_ns += gpu_backing_retain_sample_ns;
      record_timing_sample(
          gpu_backing_retain_sample_ns,
          triage_capture_gpu_backing_retain_calls_,
          triage_capture_gpu_backing_retain_total_ns_,
          triage_capture_gpu_backing_retain_max_ns_);
      ++posture_metrics->calls;
      posture_metrics->total_ns += gpu_backing_retain_sample_ns;
      posture_metrics->max_ns =
          std::max(posture_metrics->max_ns, gpu_backing_retain_sample_ns);
      if (!posture_metrics->has_first_call) {
        posture_metrics->has_first_call = true;
        posture_metrics->first_call_ns = gpu_backing_retain_sample_ns;
      } else {
        ++posture_metrics->later_calls;
        posture_metrics->later_total_ns += gpu_backing_retain_sample_ns;
        posture_metrics->later_max_ns =
            std::max(posture_metrics->later_max_ns,
                     gpu_backing_retain_sample_ns);
      }
      if (!gpu_backing) {
        return false;
      }
      fv.primary_backing_kind = ProducerBackingKind::GPU;
      fv.primary_backing_artifact = gpu_backing;
      fv.retained_gpu_backing_descriptor.valid = true;
      fv.retained_gpu_backing_descriptor.stream_id = 0;
      fv.retained_gpu_backing_descriptor.backing_id = 0;
      fv.retained_gpu_backing_descriptor.width = req.width;
      fv.retained_gpu_backing_descriptor.height = req.height;
      fv.retained_gpu_backing_descriptor.stride_bytes = job.stride_bytes;
      fv.retained_gpu_backing_descriptor.format_fourcc = job.format_fourcc;
      fv.retained_gpu_backing_descriptor.display_available = true;
      fv.retained_gpu_backing_descriptor.materialization_available =
          synthetic_gpu_backing_can_materialize_to_image(gpu_backing);
      fv.retained_gpu_backing_descriptor.materialization_requires_gpu_readback = false;
    } else {
      fv.primary_backing_kind = ProducerBackingKind::CPU;
    }
    const uint64_t member_frame_assembly_begin_ns =
        provider_monotonic_now_ns();
    fv.retain_cpu_sidecar = retain_cpu_payload;
    fv.requested_retained_plan = req.requested_retained_plan;
    if (retain_cpu_payload) {
      fv.data = bytes->data();
      fv.size_bytes = bytes->size();
      fv.cpu_payload_owner = bytes;
    }
    fv.stride_bytes = job.stride_bytes;
    auto* lease = new FrameReleaseLease();
    lease->bytes = bytes;
    fv.release = &SyntheticProvider::release_frame_;
    fv.release_user = lease;
    member_frame_assembly_sample_ns =
        provider_monotonic_now_ns() - member_frame_assembly_begin_ns;
    member_frame_assembly_ns += member_frame_assembly_sample_ns;
    const uint64_t member_post_begin_ns = provider_monotonic_now_ns();
    emit_capture_image_facts_(req, member.image_member_index);
    strand_.post_frame(fv);
    const uint64_t member_post_end_ns = provider_monotonic_now_ns();
    member_post_sample_ns = member_post_end_ns - member_post_begin_ns;
    member_post_ns += member_post_sample_ns;
    last_member_post_end_ns = member_post_end_ns;
    const uint64_t member_span_total_ns =
        member_post_end_ns - member_span_begin_ns;
    const uint64_t member_measured_total_ns =
        member_cpu_prep_ns + member_gpu_retain_ns +
        member_frame_assembly_sample_ns + member_post_sample_ns;
    if (member_span_total_ns >= member_measured_total_ns) {
      member_iteration_gap_ns +=
          member_span_total_ns - member_measured_total_ns;
    }
  }
  const uint64_t production_return_ns = provider_monotonic_now_ns();
  {
    std::lock_guard<std::mutex> triage_lock(
        triage_capture_ready_metrics_mutex_);
    const uint64_t post_return_timing_record_lock_acquired_ns =
        provider_monotonic_now_ns();
    CaptureReadyTimingRecord& timing_record =
        capture_ready_timing_record_(req.capture_id, req.device_instance_id);
    timing_record.provider_post_capture_started_before_first_member_total_ns =
        before_first_member_ns;
    timing_record.provider_post_capture_started_member_iteration_gap_total_ns =
        member_iteration_gap_ns;
    timing_record.has_provider_last_frame_posted_steady_ns = !members.empty();
    timing_record.provider_last_frame_posted_steady_ns = last_member_post_end_ns;
    ready_stage_metrics->post_capture_started_before_first_member_total_ns +=
        before_first_member_ns;
    ready_stage_metrics->post_capture_started_member_iteration_gap_total_ns +=
        member_iteration_gap_ns;
    ready_stage_metrics->post_capture_started_cpu_prep_total_ns +=
        member_alloc_ns + member_copy_ns + member_ev_bgra_ns;
    ready_stage_metrics->post_capture_started_gpu_retain_total_ns +=
        capture_gpu_backing_retain_ns;
    ready_stage_metrics->post_capture_started_frame_assembly_total_ns +=
        member_frame_assembly_ns;
    ready_stage_metrics->post_capture_started_post_frame_total_ns +=
        member_post_ns;
    ready_stage_metrics->capture_ready_provider_window_total_ns +=
        before_first_member_ns + member_iteration_gap_ns +
        member_alloc_ns + member_copy_ns + member_ev_bgra_ns +
        member_frame_assembly_ns + capture_gpu_backing_retain_ns +
        member_post_ns;
    timing_record.provider_post_capture_started_cpu_prep_total_ns =
        member_alloc_ns + member_copy_ns + member_ev_bgra_ns;
    timing_record.provider_post_capture_started_gpu_retain_total_ns =
        capture_gpu_backing_retain_ns;
    timing_record.provider_post_capture_started_frame_assembly_total_ns =
        member_frame_assembly_ns;
    timing_record.provider_post_capture_started_post_frame_total_ns =
        member_post_ns;
    timing_record.provider_capture_ready_provider_window_total_ns =
        before_first_member_ns + member_iteration_gap_ns +
        member_alloc_ns + member_copy_ns + member_ev_bgra_ns +
        member_frame_assembly_ns + capture_gpu_backing_retain_ns +
        member_post_ns;
    timing_record.has_provider_capture_generation_return_steady_ns = true;
    timing_record.provider_capture_generation_return_steady_ns =
        production_return_ns;
    timing_record
        .has_provider_capture_generation_timing_record_lock_acquired_steady_ns =
        true;
    timing_record
        .provider_capture_generation_timing_record_lock_acquired_steady_ns =
        post_return_timing_record_lock_acquired_ns;
    if (timing_record.has_provider_last_frame_posted_steady_ns &&
        production_return_ns >=
            timing_record.provider_last_frame_posted_steady_ns) {
      const uint64_t generation_tail_ns =
          production_return_ns -
          timing_record.provider_last_frame_posted_steady_ns;
      timing_record
          .provider_post_capture_started_after_last_frame_generation_tail_total_ns =
          generation_tail_ns;
      ready_stage_metrics
          ->post_capture_started_after_last_frame_generation_tail_total_ns +=
          generation_tail_ns;
    }
    timing_record.has_provider_capture_generation_post_timing_record_steady_ns =
        true;
    timing_record.provider_capture_generation_post_timing_record_steady_ns =
        provider_monotonic_now_ns();
    timing_record.provider_base_bytes_use_count_after_timing_record =
        static_cast<uint64_t>(base_bytes.use_count());
  }
  if (output_form_mode == SyntheticProducerOutputFormMode::GpuOnly &&
      deferred_cpu_staging_bytes != nullptr) {
    *deferred_cpu_staging_bytes = base_bytes;
  }
  const uint64_t pre_return_ns = provider_monotonic_now_ns();
  {
    std::lock_guard<std::mutex> triage_lock(
        triage_capture_ready_metrics_mutex_);
    CaptureReadyTimingRecord& timing_record =
        capture_ready_timing_record_(req.capture_id, req.device_instance_id);
    timing_record.has_provider_capture_generation_pre_return_steady_ns = true;
    timing_record.provider_capture_generation_pre_return_steady_ns =
        pre_return_ns;
    timing_record.provider_base_bytes_use_count_pre_return =
        static_cast<uint64_t>(base_bytes.use_count());
  }
  return true;
}

void SyntheticProvider::finish_device_capture_job_(const DeviceCaptureJob& job,
                                                   uint64_t generation,
                                                   CaptureTerminalKind terminal,
                                                   ProviderError error,
                                                   std::shared_ptr<std::vector<std::uint8_t>>
                                                       deferred_cpu_staging_bytes) {
  const uint64_t finish_begin_ns = provider_monotonic_now_ns();
  uint64_t terminal_post_ns = 0;
  bool should_post_terminal = false;
  bool should_release = false;
  std::exception_ptr terminal_post_exception{};
  uint64_t capture_lock_acquired_ns = finish_begin_ns;
  uint64_t capture_lock_wait_begin_ns = finish_begin_ns;
  uint64_t state_wait_begin_ns = 0;
  uint64_t state_lock_released_ns = 0;
  auto* ready_stage_metrics = &triage_capture_ready_stage_cpu_primary_;
  if (job.request.requested_retained_plan.valid) {
    if (job.request.requested_retained_plan.primary_cpu()) {
      ready_stage_metrics = &triage_capture_ready_stage_cpu_primary_;
    } else if (job.request.requested_retained_plan.retain_cpu_sidecar()) {
      ready_stage_metrics =
          &triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_;
    } else {
      ready_stage_metrics =
          &triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_;
    }
  }
  {
    capture_lock_wait_begin_ns = provider_monotonic_now_ns();
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    capture_lock_acquired_ns = provider_monotonic_now_ns();
    const InFlightCaptureKey key{job.request.capture_id, job.request.device_instance_id};
    auto it = in_flight_captures_.find(key);
    if (it == in_flight_captures_.end()) {
      return;
    }
    InFlightCaptureDevice& in_flight = it->second;
    if (in_flight.generation != generation) {
      terminal = CaptureTerminalKind::Failed;
      error = ProviderError::ERR_SHUTTING_DOWN;
    }
    if (!in_flight.terminal_posted) {
      in_flight.terminal_posted = true;
      should_post_terminal = true;
    }
    if (!in_flight.release_done) {
      in_flight.release_done = true;
      should_release = true;
    }
    {
      state_wait_begin_ns = provider_monotonic_now_ns();
      std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
      auto pause_it = capture_pause_depth_by_device_.find(job.request.device_instance_id);
      if (pause_it != capture_pause_depth_by_device_.end()) {
        if (pause_it->second > 1) {
          --pause_it->second;
        } else {
          capture_pause_depth_by_device_.erase(pause_it);
        }
      }
      state_lock_released_ns = provider_monotonic_now_ns();
    }
    in_flight_captures_.erase(it);
  }

  if (should_post_terminal) {
    try {
      const uint64_t terminal_post_begin_ns = provider_monotonic_now_ns();
      {
        std::lock_guard<std::mutex> triage_lock(
            triage_capture_ready_metrics_mutex_);
        CaptureReadyTimingRecord& timing_record =
            capture_ready_timing_record_(
                job.request.capture_id, job.request.device_instance_id);
        timing_record.capture_id = job.request.capture_id;
        timing_record.device_instance_id = job.request.device_instance_id;
        timing_record.acquisition_session_id = job.acquisition_session_id;
        if (timing_record.has_provider_last_frame_posted_steady_ns &&
            terminal_post_begin_ns >=
                timing_record.provider_last_frame_posted_steady_ns) {
        const uint64_t after_last_frame_before_terminal_ns =
            terminal_post_begin_ns -
            timing_record.provider_last_frame_posted_steady_ns;
        uint64_t generation_tail_ns = 0;
        if (timing_record.has_provider_capture_generation_return_steady_ns &&
            timing_record.provider_capture_generation_return_steady_ns >=
                timing_record.provider_last_frame_posted_steady_ns) {
          generation_tail_ns =
              timing_record.provider_capture_generation_return_steady_ns -
              timing_record.provider_last_frame_posted_steady_ns;
        }
        uint64_t return_to_capture_lock_ns = 0;
        if (timing_record.has_provider_capture_generation_return_steady_ns &&
            capture_lock_acquired_ns >=
                timing_record.provider_capture_generation_return_steady_ns) {
          return_to_capture_lock_ns =
              capture_lock_acquired_ns -
              timing_record.provider_capture_generation_return_steady_ns;
        }
        uint64_t return_to_finish_begin_ns = 0;
        if (timing_record.has_provider_capture_generation_return_steady_ns &&
            finish_begin_ns >=
                timing_record.provider_capture_generation_return_steady_ns) {
          return_to_finish_begin_ns =
              finish_begin_ns -
              timing_record.provider_capture_generation_return_steady_ns;
        }
        uint64_t post_return_timing_record_lock_wait_ns = 0;
        if (timing_record.has_provider_capture_generation_return_steady_ns &&
            timing_record
                .has_provider_capture_generation_timing_record_lock_acquired_steady_ns &&
            timing_record
                .provider_capture_generation_timing_record_lock_acquired_steady_ns >=
                timing_record.provider_capture_generation_return_steady_ns) {
          post_return_timing_record_lock_wait_ns =
              timing_record
                  .provider_capture_generation_timing_record_lock_acquired_steady_ns -
              timing_record.provider_capture_generation_return_steady_ns;
        }
        uint64_t post_return_timing_record_update_ns = 0;
        if (timing_record.has_provider_capture_generation_timing_record_lock_acquired_steady_ns &&
            timing_record.has_provider_capture_generation_post_timing_record_steady_ns &&
            timing_record.provider_capture_generation_post_timing_record_steady_ns >=
                timing_record
                    .provider_capture_generation_timing_record_lock_acquired_steady_ns) {
          post_return_timing_record_update_ns =
              timing_record.provider_capture_generation_post_timing_record_steady_ns -
              timing_record
                  .provider_capture_generation_timing_record_lock_acquired_steady_ns;
        }
        uint64_t post_return_after_timing_record_to_finish_begin_ns = 0;
        if (timing_record.has_provider_capture_generation_post_timing_record_steady_ns &&
            finish_begin_ns >=
                timing_record.provider_capture_generation_post_timing_record_steady_ns) {
          post_return_after_timing_record_to_finish_begin_ns =
              finish_begin_ns -
              timing_record.provider_capture_generation_post_timing_record_steady_ns;
        }
        uint64_t post_return_after_timing_record_to_pre_return_ns = 0;
        if (timing_record.has_provider_capture_generation_post_timing_record_steady_ns &&
            timing_record.has_provider_capture_generation_pre_return_steady_ns &&
            timing_record.provider_capture_generation_pre_return_steady_ns >=
                timing_record.provider_capture_generation_post_timing_record_steady_ns) {
          post_return_after_timing_record_to_pre_return_ns =
              timing_record.provider_capture_generation_pre_return_steady_ns -
              timing_record.provider_capture_generation_post_timing_record_steady_ns;
        }
        uint64_t pre_return_to_finish_begin_ns = 0;
        if (timing_record.has_provider_capture_generation_pre_return_steady_ns &&
            finish_begin_ns >=
                timing_record.provider_capture_generation_pre_return_steady_ns) {
          pre_return_to_finish_begin_ns =
              finish_begin_ns -
              timing_record.provider_capture_generation_pre_return_steady_ns;
        }
        uint64_t finish_begin_to_capture_lock_wait_begin_ns = 0;
        if (capture_lock_wait_begin_ns >= finish_begin_ns) {
          finish_begin_to_capture_lock_wait_begin_ns =
              capture_lock_wait_begin_ns - finish_begin_ns;
        }
        uint64_t capture_lock_wait_ns = 0;
        if (capture_lock_acquired_ns >= capture_lock_wait_begin_ns) {
          capture_lock_wait_ns =
              capture_lock_acquired_ns - capture_lock_wait_begin_ns;
        }
        uint64_t finish_provider_state_total_ns = 0;
        if (state_lock_released_ns >= state_wait_begin_ns) {
          finish_provider_state_total_ns =
              state_lock_released_ns - state_wait_begin_ns;
        }
        uint64_t finish_non_state_total_ns = 0;
        if (terminal_post_begin_ns >= capture_lock_acquired_ns) {
          const uint64_t capture_locked_to_terminal_ns =
              terminal_post_begin_ns - capture_lock_acquired_ns;
          if (capture_locked_to_terminal_ns >=
              finish_provider_state_total_ns) {
            finish_non_state_total_ns =
                capture_locked_to_terminal_ns -
                finish_provider_state_total_ns;
          }
        }
        ready_stage_metrics
            ->post_capture_started_after_last_frame_before_terminal_total_ns +=
            after_last_frame_before_terminal_ns;
        ready_stage_metrics
            ->post_capture_started_after_last_frame_return_to_capture_lock_total_ns +=
            return_to_capture_lock_ns;
        ready_stage_metrics
            ->post_capture_started_after_last_frame_return_to_finish_begin_total_ns +=
            return_to_finish_begin_ns;
        ready_stage_metrics
            ->post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ns +=
            post_return_timing_record_lock_wait_ns;
        ready_stage_metrics
            ->post_capture_started_after_last_frame_post_return_timing_record_update_total_ns +=
            post_return_timing_record_update_ns;
        ready_stage_metrics
            ->post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ns +=
            post_return_after_timing_record_to_finish_begin_ns;
        ready_stage_metrics
            ->post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ns +=
            post_return_after_timing_record_to_pre_return_ns;
        ready_stage_metrics
            ->post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ns +=
            pre_return_to_finish_begin_ns;
        ready_stage_metrics
            ->post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ns +=
            finish_begin_to_capture_lock_wait_begin_ns;
        ready_stage_metrics
            ->post_capture_started_after_last_frame_capture_lock_wait_total_ns +=
            capture_lock_wait_ns;
        ready_stage_metrics
            ->post_capture_started_after_last_frame_finish_provider_state_total_ns +=
            finish_provider_state_total_ns;
        ready_stage_metrics
            ->post_capture_started_after_last_frame_finish_non_state_total_ns +=
            finish_non_state_total_ns;
        ready_stage_metrics->capture_ready_provider_window_total_ns +=
            after_last_frame_before_terminal_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_before_terminal_total_ns =
            after_last_frame_before_terminal_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_generation_tail_total_ns =
            generation_tail_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_return_to_capture_lock_total_ns =
            return_to_capture_lock_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_return_to_finish_begin_total_ns =
            return_to_finish_begin_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ns =
            post_return_timing_record_lock_wait_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_post_return_timing_record_update_total_ns =
            post_return_timing_record_update_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ns =
            post_return_after_timing_record_to_finish_begin_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ns =
            post_return_after_timing_record_to_pre_return_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ns =
            pre_return_to_finish_begin_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ns =
            finish_begin_to_capture_lock_wait_begin_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_capture_lock_wait_total_ns =
            capture_lock_wait_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_finish_provider_state_total_ns =
            finish_provider_state_total_ns;
        timing_record
            .provider_post_capture_started_after_last_frame_finish_non_state_total_ns =
            finish_non_state_total_ns;
        timing_record.provider_capture_ready_provider_window_total_ns +=
            after_last_frame_before_terminal_ns;
        }
        if (terminal == CaptureTerminalKind::Completed) {
          timing_record.has_provider_post_capture_completed_steady_ns = true;
          timing_record.provider_post_capture_completed_steady_ns =
              terminal_post_begin_ns;
        }
      }
      if (terminal == CaptureTerminalKind::Completed) {
        strand_.post_capture_completed(
            job.request.capture_id, job.request.device_instance_id);
      } else {
        const ProviderError failure_error =
            error == ProviderError::OK ? ProviderError::ERR_PROVIDER_FAILED
                                       : error;
        strand_.post_capture_failed(job.request.capture_id,
                                    job.request.device_instance_id,
                                    failure_error);
      }
      terminal_post_ns = provider_monotonic_now_ns() - terminal_post_begin_ns;
      {
        std::lock_guard<std::mutex> triage_lock(
            triage_capture_ready_metrics_mutex_);
        ready_stage_metrics->capture_terminal_post_total_ns += terminal_post_ns;
        ready_stage_metrics->capture_ready_provider_window_total_ns +=
            terminal_post_ns;
        CaptureReadyTimingRecord& timing_record =
            capture_ready_timing_record_(
                job.request.capture_id, job.request.device_instance_id);
        timing_record.provider_capture_terminal_post_total_ns =
            terminal_post_ns;
        timing_record.provider_capture_ready_provider_window_total_ns +=
            terminal_post_ns;
      }
    } catch (...) {
      terminal_post_exception = std::current_exception();
    }
  }
  if (should_release) {
    std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
    release_native_acquisition_session_for_capture_(job.request.device_instance_id);
  }
  if (terminal_post_exception) {
    std::rethrow_exception(terminal_post_exception);
  }
  (void)deferred_cpu_staging_bytes;
}

SyntheticProvider::CaptureReadyTimingRecord&
SyntheticProvider::capture_ready_timing_record_(uint64_t capture_id,
                                                uint64_t device_instance_id) {
  const CaptureReadyTimingKey key{capture_id, device_instance_id};
  auto it = triage_capture_ready_timing_records_.find(key);
  if (it != triage_capture_ready_timing_records_.end()) {
    return it->second;
  }

  CaptureReadyTimingRecord record{};
  record.capture_id = capture_id;
  record.device_instance_id = device_instance_id;
  auto [inserted_it, inserted] =
      triage_capture_ready_timing_records_.emplace(key, std::move(record));
  if (inserted) {
    triage_capture_ready_timing_order_.push_back(key);
    constexpr size_t kMaxRecentCaptureReadyTimingRecords = 256;
    if (triage_capture_ready_timing_order_.size() >
        kMaxRecentCaptureReadyTimingRecords) {
      const CaptureReadyTimingKey oldest =
          triage_capture_ready_timing_order_.front();
      triage_capture_ready_timing_order_.erase(
          triage_capture_ready_timing_order_.begin());
      triage_capture_ready_timing_records_.erase(oldest);
    }
  }
  return inserted_it->second;
}

void SyntheticProvider::drain_paused_capture_descendants_for_host_() {
  constexpr auto kDrainPollSleep = std::chrono::milliseconds(1);
  constexpr uint64_t kDrainMaxWaitNs = 2'000'000'000ull;
  const uint64_t wait_begin_ns = provider_monotonic_now_ns();
  while (true) {
    advance(0, /*allow_paused_timeline_step=*/true);
    bool any_in_flight = false;
    {
      std::lock_guard<std::mutex> capture_lock(capture_mutex_);
      any_in_flight = !in_flight_captures_.empty();
    }
    if (!any_in_flight) {
      return;
    }
    if (shutting_down_ ||
        provider_monotonic_now_ns() - wait_begin_ns >= kDrainMaxWaitNs) {
      return;
    }
    std::this_thread::sleep_for(kDrainPollSleep);
  }
}

void SyntheticProvider::stop_and_join_capture_executor_() noexcept {
  std::vector<std::thread> workers;
  {
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    capture_admission_closed_ = true;
    if (!capture_executor_stop_requested_) {
      capture_executor_stop_requested_ = true;
      ++capture_generation_;
    }
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
    capture_workers_paused_for_test_ = false;
    capture_jobs_paused_after_dequeue_for_test_ = false;
#endif
    workers.swap(capture_workers_);
  }
  capture_cv_.notify_all();
  for (std::thread& worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  // Every queued item is drained by the fixed workers after the generation is
  // closed. Active items observe the same generation change at their next
  // cancellation checkpoint. Joining above therefore owns the complete
  // worker lifetime and leaves no job that can callback into a later restart.
  std::lock_guard<std::mutex> capture_lock(capture_mutex_);
  if (capture_queue_count_ != 0 || capture_active_jobs_ != 0 ||
      !in_flight_captures_.empty()) {
    std::fprintf(
        stderr,
        "[CamBANG][SyntheticProvider] capture executor failed to reach quiescence queue=%zu active=%zu in_flight=%zu\n",
        capture_queue_count_,
        capture_active_jobs_,
        in_flight_captures_.size());
  }
#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
  next_capture_worker_fault_for_test_ = CaptureWorkerFaultForTest::None;
  next_capture_admission_fault_for_test_ =
      CaptureAdmissionFaultForTest::None;
#endif
}

#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
void SyntheticProvider::set_capture_workers_paused_for_test(bool paused) {
  {
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    capture_workers_paused_for_test_ = paused;
  }
  capture_cv_.notify_all();
}

void SyntheticProvider::set_capture_jobs_paused_after_dequeue_for_test(
    bool paused) {
  {
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    capture_jobs_paused_after_dequeue_for_test_ = paused;
  }
  capture_cv_.notify_all();
}

void SyntheticProvider::inject_next_capture_admission_fault_for_test(
    CaptureAdmissionFaultForTest fault) {
  std::lock_guard<std::mutex> capture_lock(capture_mutex_);
  next_capture_admission_fault_for_test_ = fault;
}

void SyntheticProvider::inject_next_capture_worker_fault_for_test(
    CaptureWorkerFaultForTest fault) {
  std::lock_guard<std::mutex> capture_lock(capture_mutex_);
  next_capture_worker_fault_for_test_ = fault;
}

SyntheticProvider::CaptureExecutorSnapshotForTest
SyntheticProvider::capture_executor_snapshot_for_test() const {
  std::lock_guard<std::mutex> capture_lock(capture_mutex_);
  std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
  CaptureExecutorSnapshotForTest snapshot{};
  snapshot.worker_count = capture_workers_.size();
  snapshot.worker_limit = kCaptureWorkerCount;
  snapshot.queued_jobs = capture_queue_count_;
  snapshot.queue_capacity = kCaptureQueueCapacity;
  snapshot.active_jobs = capture_active_jobs_;
  snapshot.in_flight_jobs = in_flight_captures_.size();
  snapshot.paused_device_count = capture_pause_depth_by_device_.size();
  snapshot.max_queued_jobs = capture_max_queued_jobs_for_test_;
  snapshot.max_active_jobs = capture_max_active_jobs_for_test_;
  snapshot.admission_closed = capture_admission_closed_;
  snapshot.stop_requested = capture_executor_stop_requested_;
  return snapshot;
}
#endif

bool SyntheticProvider::has_in_flight_capture_for_device_locked_(uint64_t device_instance_id) const {
  for (const auto& kv : in_flight_captures_) {
    if (kv.second.device_instance_id == device_instance_id && !kv.second.release_done) {
      return true;
    }
  }
  return false;
}

bool SyntheticProvider::is_stream_capture_paused_locked_(const StreamState& s) const {
  const auto it = capture_pause_depth_by_device_.find(s.req.device_instance_id);
  return it != capture_pause_depth_by_device_.end() && it->second > 0;
}

uint64_t SyntheticProvider::snap_repeating_due_after_(uint64_t due_ns, uint64_t now_ns, uint64_t period_ns) noexcept {
  if (period_ns == 0 || due_ns > now_ns) {
    return due_ns;
  }
  const uint64_t missed = ((now_ns - due_ns) / period_ns) + 1;
  const uint64_t max_increment = std::numeric_limits<uint64_t>::max() - due_ns;
  if (missed > max_increment / period_ns) {
    return std::numeric_limits<uint64_t>::max();
  }
  return due_ns + (missed * period_ns);
}

ProviderResult SyntheticProvider::abort_capture(uint64_t capture_id) {
  (void)capture_id;
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult SyntheticProvider::apply_camera_spec_patch(
    const std::string& hardware_id,
    uint64_t new_camera_spec_version,
    SpecPatchView patch) {
  (void)hardware_id;
  (void)new_camera_spec_version;
  (void)patch;
  // Core validates patches; synthetic currently does not implement dynamic capability changes.
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::apply_imaging_spec_patch(
    uint64_t new_imaging_spec_version,
    SpecPatchView patch) {
  (void)new_imaging_spec_version;
  (void)patch;
  return ProviderResult::success();
}



void SyntheticProvider::destroy_stream_storage_(std::map<uint64_t, StreamState>::iterator it,
                                               ProviderError stop_error,
                                               bool emit_stop_event) {
  if (it == streams_.end()) {
    return;
  }

  StreamState& s = it->second;
  const bool had_started = s.started;
  const uint64_t stream_id = s.req.stream_id;
  const uint64_t device_instance_id = s.req.device_instance_id;
  if (s.producing) {
    s.producing = false;
  }
  if (emit_stop_event && had_started) {
    strand_.post_stream_stopped(stream_id, stop_error);
  }
  release_stream_live_gpu_backing_(s);
  s.started = false;
  emit_native_destroy_(s.native_id);
  release_native_acquisition_session_for_stream_(device_instance_id);
  strand_.post_stream_destroyed(stream_id);
  streams_.erase(it);
}

void SyntheticProvider::close_device_storage_(std::map<uint64_t, DeviceState>::iterator it) {
  if (it == devices_.end()) {
    return;
  }
  capture_pause_depth_by_device_.erase(it->second.device_instance_id);
  if (it->second.acquisition_session_native_id != 0) {
    emit_native_destroy_(it->second.acquisition_session_native_id);
    it->second.acquisition_session_native_id = 0;
    it->second.acquisition_session_stream_refs = 0;
    it->second.acquisition_session_capture_refs = 0;
    it->second.acquisition_session_priming_refs = 0;
  }
  it->second.open = false;
  strand_.post_device_closed(it->second.device_instance_id);
  emit_native_destroy_(it->second.native_id);
  devices_.erase(it);
}

ProviderResult SyntheticProvider::disconnect_device_for_test(uint64_t device_instance_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto dit = devices_.find(device_instance_id);
  if (dit == devices_.end() || !dit->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  std::vector<uint64_t> stream_ids;
  for (const auto& kv : streams_) {
    if (kv.second.created && kv.second.req.device_instance_id == device_instance_id) {
      stream_ids.push_back(kv.first);
    }
  }
  for (uint64_t sid : stream_ids) {
    auto it = streams_.find(sid);
    if (it != streams_.end()) {
      destroy_stream_storage_(it, ProviderError::ERR_PROVIDER_FAILED, true);
    }
  }

  close_device_storage_(dit);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::force_close_device_for_test(uint64_t device_instance_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto dit = devices_.find(device_instance_id);
  if (dit == devices_.end() || !dit->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  std::vector<uint64_t> stream_ids;
  for (const auto& kv : streams_) {
    if (kv.second.created && kv.second.req.device_instance_id == device_instance_id) {
      stream_ids.push_back(kv.first);
    }
  }
  for (uint64_t sid : stream_ids) {
    auto it = streams_.find(sid);
    if (it != streams_.end()) {
      destroy_stream_storage_(it, ProviderError::OK, true);
    }
  }

  close_device_storage_(dit);
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::fail_stream_for_test(uint64_t stream_id, ProviderError error) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (error == ProviderError::OK) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  if (it->second.producing) {
    it->second.producing = false;
  }
  strand_.post_stream_error(stream_id, error);
  if (it->second.started) {
    strand_.post_stream_stopped(stream_id, error);
  }
  it->second.started = false;
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::load_timeline_canonical_scenario_from_json_text_for_host(
    const std::string& text,
    std::string* error) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (cfg_.synthetic_role != SyntheticRole::Timeline) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }

  SyntheticCanonicalScenario canonical{};
  std::string load_error;
  if (!load_synthetic_canonical_scenario_from_json_text(text, canonical, &load_error)) {
    if (error) {
      *error = load_error;
    }
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (error) {
    error->clear();
  }
  return set_timeline_scenario_for_host(canonical);
}

ProviderResult SyntheticProvider::load_timeline_canonical_scenario_from_json_file_for_host(
    const std::string& path,
    std::string* error) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (cfg_.synthetic_role != SyntheticRole::Timeline) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }

  SyntheticCanonicalScenario canonical{};
  std::string load_error;
  if (!load_synthetic_canonical_scenario_from_json_file(path, canonical, &load_error)) {
    if (error) {
      *error = load_error;
    }
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (error) {
    error->clear();
  }
  return set_timeline_scenario_for_host(canonical);
}

ProviderResult SyntheticProvider::set_timeline_scenario_for_host(const SyntheticTimelineScenario& scenario) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (cfg_.synthetic_role != SyntheticRole::Timeline) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  // Host-submitted scenarios are staged, not auto-started.
  // This intentionally differs from config-seeded initialization semantics.
  // Semantics remain provider-owned; only arming differs (stage until start).
  while (!timeline_q_.empty()) {
    timeline_q_.pop();
  }
  timeline_seq_ = 0;
  timeline_running_ = false;
  timeline_paused_ = false;
  timeline_pending_destructive_.clear();
  timeline_scenario_ = scenario;
  timeline_canonical_scenario_ = {};
  timeline_canonical_staged_ = false;
  staged_rig_topology_.clear();
  staged_required_endpoint_count_ = 0;
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::set_timeline_scenario_for_host(const SyntheticCanonicalScenario& scenario) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (cfg_.synthetic_role != SyntheticRole::Timeline) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  // Host-submitted canonical scenarios are staged until explicit start.
  while (!timeline_q_.empty()) {
    timeline_q_.pop();
  }
  timeline_seq_ = 0;
  timeline_running_ = false;
  timeline_paused_ = false;
  timeline_pending_destructive_.clear();
  timeline_scenario_ = {};
  timeline_canonical_scenario_ = scenario;
  timeline_canonical_staged_ = true;
  staged_rig_topology_.clear();
  staged_required_endpoint_count_ = 0;
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::start_timeline_scenario_for_host() {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (cfg_.synthetic_role != SyntheticRole::Timeline) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  while (!timeline_q_.empty()) {
    timeline_q_.pop();
  }
  timeline_seq_ = 0;
  timeline_pending_destructive_.clear();
  if (timeline_canonical_staged_) {
    std::string error;
    std::vector<SyntheticStagedRigTopology> staged_rigs;
    if (!materialize_staged_canonical_scenario_(timeline_scenario_, staged_rigs, error)) {
      if (!error.empty()) {
        std::fprintf(stderr, "[Synthetic] canonical scenario materialization failed: %s\n", error.c_str());
      }
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    staged_rig_topology_ = std::move(staged_rigs);
  }
  // Host start arms the provider-owned scenario by scheduling authored events
  // and marking the timeline running/unpaused. It intentionally does not advance
  // or pump synthetic time; the host stepper is responsible for executing due
  // events via advance(dt_ns), including a meaningful dt=0 current-time pump.
  for (const auto& ev : timeline_scenario_.events) {
    timeline_schedule_(ev);
  }
  timeline_running_ = true;
  timeline_paused_ = false;
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::stop_timeline_scenario_for_host() {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (cfg_.synthetic_role != SyntheticRole::Timeline) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  while (!timeline_q_.empty()) {
    timeline_q_.pop();
  }
  timeline_seq_ = 0;
  timeline_running_ = false;
  timeline_paused_ = false;
  timeline_pending_destructive_.clear();
  staged_required_endpoint_count_ = 0;
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::set_timeline_scenario_paused_for_host(bool paused) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (cfg_.synthetic_role != SyntheticRole::Timeline) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  if (!timeline_running_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  timeline_paused_ = paused;
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::advance_timeline_for_host(uint64_t dt_ns) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (cfg_.synthetic_role != SyntheticRole::Timeline) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  if (cfg_.timing_driver != TimingDriver::VirtualTime) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  advance(dt_ns, /*allow_paused_timeline_step=*/true);
  if (timeline_paused_) {
    // In paused/manual stepping, capture result production is still real work on
    // background threads. Drain already-triggered capture descendants here so
    // host-driven virtual-time verification does not become platform-speed
    // sensitive, while leaving later evaluator-only convergence to explicit
    // caller-provided budget.
    drain_paused_capture_descendants_for_host_();
  }
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::set_timeline_reconciliation_for_host(TimelineReconciliation reconciliation) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (cfg_.synthetic_role != SyntheticRole::Timeline) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  completion_gated_destructive_sequencing_enabled_ =
      (reconciliation == TimelineReconciliation::CompletionGated);
  if (!completion_gated_destructive_sequencing_enabled_) {
    timeline_pending_destructive_.clear();
  }
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::shutdown() {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  shutting_down_ = true;

  // Close capture admission and wait for accepted capture production to post a
  // terminal fact and release its retained acquisition-session references before
  // native/device teardown below.
  stop_and_join_capture_executor_();

  {
    std::lock_guard<std::mutex> state_lock(provider_state_mutex_);

    // Deterministic teardown order:
    // stop streams, destroy streams, close devices.
    for (auto& kv : streams_) {
      if (kv.second.started) {
        kv.second.started = false;
        kv.second.producing = false;
        strand_.post_stream_stopped(kv.first, ProviderError::OK);
        release_stream_live_gpu_backing_(kv.second);
      }
    }
    while (!streams_.empty()) {
      destroy_stream_storage_(streams_.begin(), ProviderError::OK, false);
    }
    while (!devices_.empty()) {
      close_device_storage_(devices_.begin());
    }

    // Clear any pending timeline events.
    while (!timeline_q_.empty()) {
      timeline_q_.pop();
    }
    timeline_seq_ = 0;
    timeline_pending_destructive_.clear();
    timeline_scenario_ = {};
    timeline_canonical_scenario_ = {};
    timeline_canonical_staged_ = false;
    staged_rig_topology_.clear();
    staged_required_endpoint_count_ = 0;
    timeline_running_ = false;
    timeline_paused_ = false;

    // Provider native object (BOUND -> ABSENT).
    if (provider_native_id_ != 0) {
      emit_native_destroy_(provider_native_id_);
      provider_native_id_ = 0;
    }
  }

  strand_.flush();
  strand_.stop();

  initialized_ = false;
  callbacks_ = nullptr;
  shutting_down_ = false;
  {
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    capture_admission_closed_ = true;
  }
  return ProviderResult::success();
}

void SyntheticProvider::release_frame_(void* user, const FrameView* frame) {
  (void)frame;
  auto* lease = static_cast<FrameReleaseLease*>(user);
  if (!lease) {
    return;
  }
  if (lease->slot) {
    lease->slot->in_use.store(false, std::memory_order_release);
  }
  delete lease;
}

bool SyntheticProvider::ensure_stream_live_gpu_backing_(
    StreamState& s,
    uint32_t width,
    uint32_t height,
    uint32_t stride) {
  if (s.live_gpu_backing &&
      s.live_gpu_width == width &&
      s.live_gpu_height == height &&
      s.live_gpu_stride_bytes == stride) {
    return true;
  }
  if (s.live_gpu_backing) {
    ++triage_gpu_backing_recreate_total_;
  }
  release_stream_live_gpu_backing_(s);
  // Create and recreate share the same runtime helper so usage flags remain
  // identical for initial allocation and retry allocation.
  s.live_gpu_backing = synthetic_gpu_backing_create_stream_live_gpu_backing_rgba8(s.req.stream_id, width, height, stride);
  if (!s.live_gpu_backing) {
    return false;
  }
  const uint64_t native_id = alloc_native_id_(NativeObjectType::GpuBacking);
  ++s.live_gpu_backing_generation;
  if (native_id != 0 && callbacks_) {
    const auto dit = devices_.find(s.req.device_instance_id);
    const uint64_t root_id = (dit != devices_.end()) ? dit->second.root_id : 0;
    NativeObjectCreateInfo info{};
    info.native_id = native_id;
    info.type = static_cast<uint32_t>(NativeObjectType::GpuBacking);
    info.root_id = root_id;
    info.owner_device_instance_id = s.req.device_instance_id;
    info.owner_acquisition_session_id = s.acquisition_session_native_id;
    info.owner_stream_id = s.req.stream_id;
    info.owner_provider_native_id = provider_native_id_;
    info.owner_rig_id = 0;
    info.has_created_ns = true;
    info.created_ns = clock_.now_ns();
    strand_.post_native_object_created(info);
    s.live_gpu_backing_native_id = native_id;
  }
  s.live_gpu_width = width;
  s.live_gpu_height = height;
  s.live_gpu_stride_bytes = stride;
  return true;
}

void SyntheticProvider::release_stream_live_gpu_backing_(StreamState& s) {
  if (s.live_gpu_backing) {
    ++triage_gpu_backing_release_total_;
  }
  if (!s.live_gpu_backing) {
    if (s.live_gpu_backing_native_id != 0) {
      emit_native_destroy_(s.live_gpu_backing_native_id);
      s.live_gpu_backing_native_id = 0;
    }
    s.live_gpu_width = 0;
    s.live_gpu_height = 0;
    s.live_gpu_stride_bytes = 0;
    return;
  }
  synthetic_gpu_backing_release_stream_live_gpu_backing(s.live_gpu_backing);
  s.live_gpu_backing.reset();
  if (s.live_gpu_backing_native_id != 0) {
    emit_native_destroy_(s.live_gpu_backing_native_id);
    s.live_gpu_backing_native_id = 0;
  }
  s.live_gpu_width = 0;
  s.live_gpu_height = 0;
  s.live_gpu_stride_bytes = 0;
}

void SyntheticProvider::emit_one_frame_(StreamState& s, uint64_t scheduled_capture_ns) {
  const auto emit_t0 = std::chrono::steady_clock::now();
  if (!callbacks_) {
    return;
  }

  const uint32_t w = s.req.profile.width;
  const uint32_t h = s.req.profile.height;
  const uint32_t stride = w * 4u;

  // Acquire a buffer slot.
  std::shared_ptr<StreamState::BufferSlot> slot;
  const size_t n = s.pool.size();
  if (n == 0) {
    return;
  }
  for (size_t probe = 0; probe < n; ++probe) {
    const size_t idx = (s.pool_cursor + probe) % n;
    auto& cand = s.pool[idx];
    if (!cand) {
      continue;
    }
    bool expected = false;
    if (cand->in_use.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      slot = cand;
      s.pool_cursor = (idx + 1) % n;
      break;
    }
  }
  if (!slot) {
    // Drop if pool exhausted.
    return;
  }

  if (!s.render_spec_valid) {
    bool preset_valid = true;
    const auto spec_t0 = std::chrono::steady_clock::now();
    s.render_spec = build_stream_render_spec(s.picture, w, h, &preset_valid);
    const auto spec_t1 = std::chrono::steady_clock::now();
    const uint64_t spec_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(spec_t1 - spec_t0).count());
    triage_render_spec_build_total_ns_ += spec_ns;
    triage_render_spec_build_max_ns_ = std::max(triage_render_spec_build_max_ns_, spec_ns);
    s.render_spec_valid = true;
    s.renderer.configure(s.render_spec);
    if (!preset_valid) {
      invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  const PatternSpec& spec = s.render_spec;

  const auto target_t0 = std::chrono::steady_clock::now();
  const bool publish_cpu_payload =
      s.resolved_output_form_mode != SyntheticProducerOutputFormMode::GpuOnly;
  const bool render_direct_to_cpu_slot = publish_cpu_payload && !s.prefer_gpu_backing;
  PatternRenderTarget dst{};
  dst.data = render_direct_to_cpu_slot ? static_cast<void*>(slot->bytes.data()) : static_cast<void*>(s.gpu_staging.data());
  dst.size_bytes = render_direct_to_cpu_slot ? slot->bytes.size() : s.gpu_staging.size();
  dst.width = w;
  dst.height = h;
  dst.stride_bytes = stride;
  dst.format = PatternSpec::PackedFormat::RGBA8;
  const auto target_t1 = std::chrono::steady_clock::now();
  const uint64_t target_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(target_t1 - target_t0).count());
  triage_render_target_prepare_total_ns_ += target_ns;
  triage_render_target_prepare_max_ns_ = std::max(triage_render_target_prepare_max_ns_, target_ns);

  PatternOverlayData ov{};
  ov.frame_index = generator_frame_ordinal_from_ns_(scheduled_capture_ns, s.picture);
  ov.timestamp_ns = scheduled_capture_ns;
  ov.stream_id = s.req.stream_id;

  const auto render_t0 = std::chrono::steady_clock::now();
  s.renderer.render_into(spec, dst, ov);
  const auto render_t1 = std::chrono::steady_clock::now();
  const uint64_t render_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(render_t1 - render_t0).count());
  record_timing_sample(render_ns, triage_frame_render_calls_, triage_frame_render_total_ns_, triage_frame_render_max_ns_);
  bool gpu_ok = false;
  std::shared_ptr<void> gpu_backing;
  if (s.prefer_gpu_backing) {
    const auto ensure_t0 = std::chrono::steady_clock::now();
    const bool ensured_backing = ensure_stream_live_gpu_backing_(s, w, h, stride);
    const auto ensure_t1 = std::chrono::steady_clock::now();
    const uint64_t ensure_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(ensure_t1 - ensure_t0).count());
    ++triage_gpu_ensure_backing_calls_total_;
    triage_gpu_ensure_backing_total_ns_ += ensure_ns;
    triage_gpu_ensure_backing_max_ns_ = std::max(triage_gpu_ensure_backing_max_ns_, ensure_ns);
    if (ensured_backing) {
      const StreamGpuUpdatePolicy gpu_update_policy = resolve_synthetic_stream_gpu_update_policy().policy;
      const bool provider_has_display_demand_signal = true;
      const bool display_demand_active =
          callbacks_ ? callbacks_->is_stream_display_demand_active(s.req.stream_id) : false;
      const bool gpu_only_stream_output =
          s.resolved_output_form_mode == SyntheticProducerOutputFormMode::GpuOnly;
      const bool skip_gpu_update_for_demand =
          !gpu_only_stream_output &&
          gpu_update_policy == StreamGpuUpdatePolicy::DisplayDemanded &&
          (!provider_has_display_demand_signal || !display_demand_active);
      if (display_demand_trace_enabled_) {
        const bool prev_active = display_demand_last_active_by_stream_[s.req.stream_id];
        if (prev_active != display_demand_active) {
          std::printf(
              "[CamBANG][DemandTrace] provider_demand_transition stream_id=%llu active=%d policy=%s\n",
              static_cast<unsigned long long>(s.req.stream_id),
              display_demand_active ? 1 : 0,
              to_string(gpu_update_policy));
          display_demand_last_active_by_stream_[s.req.stream_id] = display_demand_active;
        }
      }
      bool attempted_gpu_update = false;
      if (skip_gpu_update_for_demand) {
        ++triage_gpu_update_demand_skipped_total_;
      } else {
        attempted_gpu_update = true;
        ++triage_gpu_update_attempts_total_;
        ++triage_gpu_update_total_calls_;
        const auto update_total_t0 = std::chrono::steady_clock::now();
        gpu_ok = synthetic_gpu_backing_update_stream_live_gpu_backing_rgba8(
            s.live_gpu_backing,
            s.gpu_staging.data(),
            w,
            h,
            stride);
        const auto update_total_t1 = std::chrono::steady_clock::now();
        const uint64_t update_total_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(update_total_t1 - update_total_t0).count());
        triage_gpu_update_total_ns_ += update_total_ns;
        triage_gpu_update_total_max_ns_ = std::max(triage_gpu_update_total_max_ns_, update_total_ns);
      }
      if (display_demand_trace_enabled_) {
        const bool prev_skip = display_demand_last_skip_by_stream_[s.req.stream_id];
        if (prev_skip != skip_gpu_update_for_demand) {
          std::printf(
              "[CamBANG][DemandTrace] provider_gpu_decision_transition stream_id=%llu demand_active=%d decision=%s\n",
              static_cast<unsigned long long>(s.req.stream_id),
              display_demand_active ? 1 : 0,
              skip_gpu_update_for_demand ? "demand_skipped" : "gpu_update_attempt");
          display_demand_last_skip_by_stream_[s.req.stream_id] = skip_gpu_update_for_demand;
        }
      }
      if (attempted_gpu_update && !gpu_ok) {
        ++triage_gpu_update_failures_total_;
        ++triage_gpu_update_retries_total_;
        // Preserve current provider hardening shape: one release/recreate and
        // one retry if the in-place live-backing update reports failure.
        ++triage_gpu_backing_recreate_total_;
        release_stream_live_gpu_backing_(s);
        const auto ensure_retry_t0 = std::chrono::steady_clock::now();
        const bool ensured_retry = ensure_stream_live_gpu_backing_(s, w, h, stride);
        const auto ensure_retry_t1 = std::chrono::steady_clock::now();
        const uint64_t ensure_retry_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ensure_retry_t1 - ensure_retry_t0).count());
        ++triage_gpu_ensure_backing_calls_total_;
        triage_gpu_ensure_backing_total_ns_ += ensure_retry_ns;
        triage_gpu_ensure_backing_max_ns_ = std::max(triage_gpu_ensure_backing_max_ns_, ensure_retry_ns);
        if (ensured_retry) {
          ++triage_gpu_update_attempts_total_;
          ++triage_gpu_update_total_calls_;
          const auto update_retry_t0 = std::chrono::steady_clock::now();
          gpu_ok = synthetic_gpu_backing_update_stream_live_gpu_backing_rgba8(
              s.live_gpu_backing,
              s.gpu_staging.data(),
              w,
              h,
              stride);
          const auto update_retry_t1 = std::chrono::steady_clock::now();
          const uint64_t update_retry_ns = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(update_retry_t1 - update_retry_t0).count());
          triage_gpu_update_total_ns_ += update_retry_ns;
          triage_gpu_update_total_max_ns_ = std::max(triage_gpu_update_total_max_ns_, update_retry_ns);
          if (!gpu_ok) {
            ++triage_gpu_update_failures_total_;
          }
        }
      }
      if (gpu_ok) {
        gpu_backing = s.live_gpu_backing;
      }
    }
  }
  const bool requires_gpu_primary =
      s.resolved_output_form_mode == SyntheticProducerOutputFormMode::GpuOnly ||
      s.resolved_output_form_mode == SyntheticProducerOutputFormMode::CpuAndGpu;
  if (requires_gpu_primary && !gpu_backing) {
    // A mode that selected a truthful GPU-backed stream must not silently emit
    // a CPU fallback frame when live GPU backing creation/update is unavailable.
    slot->in_use.store(false, std::memory_order_release);
    return;
  }

  if (publish_cpu_payload) {
    if (!render_direct_to_cpu_slot) {
      // Preserve a current CPU materialization source for the exact FrameView that
      // is about to be retained. GPU-only mode keeps CPU staging provider-local.
      const auto copy_t0 = std::chrono::steady_clock::now();
      std::memcpy(slot->bytes.data(), s.gpu_staging.data(), slot->bytes.size());
      const auto copy_t1 = std::chrono::steady_clock::now();
      const uint64_t copy_ns = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(copy_t1 - copy_t0).count());
      record_timing_sample(copy_ns, triage_frame_copy_calls_, triage_frame_copy_total_ns_, triage_frame_copy_max_ns_);
    }
  }

  FrameView fv{};
  fv.device_instance_id = s.req.device_instance_id;
  fv.stream_id = s.req.stream_id;
  fv.acquisition_session_id = s.acquisition_session_native_id;
  fv.capture_id = 0;
  fv.width = w;
  fv.height = h;
  fv.format_fourcc = FOURCC_RGBA;
  fv.primary_backing_kind = gpu_backing ? ProducerBackingKind::GPU : ProducerBackingKind::CPU;
  if (gpu_backing) {
    fv.retained_gpu_backing_descriptor.valid = true;
    fv.retained_gpu_backing_descriptor.stream_id = s.req.stream_id;
    fv.retained_gpu_backing_descriptor.backing_id =
        (s.live_gpu_backing_native_id != 0) ? s.live_gpu_backing_native_id : s.live_gpu_backing_generation;
    fv.retained_gpu_backing_descriptor.width = w;
    fv.retained_gpu_backing_descriptor.height = h;
    fv.retained_gpu_backing_descriptor.stride_bytes = stride;
    fv.retained_gpu_backing_descriptor.format_fourcc = FOURCC_RGBA;
    fv.retained_gpu_backing_descriptor.display_available = true;
    fv.retained_gpu_backing_descriptor.materialization_available =
        gpu_ok && synthetic_gpu_backing_can_materialize_to_image(gpu_backing);
    fv.retained_gpu_backing_descriptor.materialization_requires_gpu_readback = false;
  }
  fv.primary_backing_artifact = std::move(gpu_backing);
  if (const auto timing = synthetic_acquisition_timing_from_unsigned(
          scheduled_capture_ns,
          ImageAcquisitionReferenceEvent::PROVIDER_OBSERVED,
          ImageAcquisitionComparability::SAME_PROVIDER)) {
    fv.acquisition_timing =
        SourcedFact<ImageAcquisitionTiming>{*timing, FactOrigin::VIRTUAL_CAMERA_AUTHORED};
  }
  fv.retain_cpu_sidecar = publish_cpu_payload;
  fv.requested_retained_plan = s.req.requested_retained_plan;
  if (publish_cpu_payload) {
    fv.data = slot->bytes.data();
    fv.size_bytes = slot->bytes.size();
    fv.cpu_payload_owner =
        std::shared_ptr<const std::vector<uint8_t>>(slot, &slot->bytes);
  }
  fv.stride_bytes = stride;
  const bool profile_compatible =
      fv.width == s.req.profile.width &&
      fv.height == s.req.profile.height &&
      (s.req.profile.format_fourcc == 0 || s.req.profile.format_fourcc == fv.format_fourcc);
  if (!profile_compatible) {
    slot->in_use.store(false, std::memory_order_release);
    return;
  }
  auto* lease = new FrameReleaseLease();
  lease->slot = slot;
  fv.release = &SyntheticProvider::release_frame_;
  fv.release_user = lease;

  const auto post_t0 = std::chrono::steady_clock::now();
  strand_.post_frame(fv);
  const auto post_t1 = std::chrono::steady_clock::now();
  const uint64_t post_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(post_t1 - post_t0).count());
  record_timing_sample(post_ns, triage_post_frame_calls_, triage_post_frame_total_ns_, triage_post_frame_max_ns_);
  const auto emit_t1 = std::chrono::steady_clock::now();
  const uint64_t emit_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(emit_t1 - emit_t0).count());
  record_timing_sample(emit_ns, triage_emit_frame_calls_, triage_emit_frame_total_ns_, triage_emit_frame_max_ns_);
}

void SyntheticProvider::emit_due_frames_() {
  const uint64_t now = clock_.now_ns();
  const uint64_t period = fps_period_ns(cfg_.nominal.fps_num, cfg_.nominal.fps_den);
  if (period == 0) {
    return;
  }

  for (auto& kv : streams_) {
    StreamState& s = kv.second;
    if (!s.created || !s.started) {
      continue;
    }
    if (is_stream_capture_paused_locked_(s)) {
      s.consecutive_behind_ticks = 0;
      s.next_due_ns = snap_repeating_due_after_(s.next_due_ns, now, period);
      continue;
    }
    const bool behind = (s.next_due_ns <= now);
    if (behind) {
      ++s.consecutive_behind_ticks;
      if (s.consecutive_behind_ticks > 1) {
        ++triage_falling_behind_repeat_total_;
      }
    } else {
      s.consecutive_behind_ticks = 0;
    }

    // Nominal live streams should not replay every overdue frame after a slow
    // host tick. Doing so creates a positive feedback loop where a delayed tick
    // emits many stale frames, which makes the next tick even later. Emit at
    // most one frame for the current tick and snap past any missed intervals.
    uint32_t emitted_this_tick = 0;
    if (s.next_due_ns <= now) {
      const uint64_t scheduled = s.next_due_ns;
      emit_one_frame_(s, scheduled);
      ++emitted_this_tick;
      ++triage_frames_emitted_total_;

      const uint64_t next_due = scheduled + period;
      if (next_due <= now) {
        const uint64_t snapped_due = snap_repeating_due_after_(next_due, now, period);
        if (snapped_due > next_due) {
          ++triage_catchup_ticks_capped_total_;
          triage_catchup_frames_dropped_total_ +=
              static_cast<uint64_t>((snapped_due - next_due) / period);
        }
        s.next_due_ns = snapped_due;
      } else {
        s.next_due_ns = next_due;
      }
    }
    if (emitted_this_tick > 0) {
      ++triage_catchup_bursts_total_;
      triage_catchup_max_frames_in_tick_ = std::max(triage_catchup_max_frames_in_tick_, emitted_this_tick);
    }
  }
  emit_triage_trace_if_due_();
}

void SyntheticProvider::emit_triage_trace_if_due_() {
  if (!triage_trace_enabled_) {
    return;
  }
  const uint64_t now = clock_.now_ns();
  if (triage_next_log_ns_ != 0 && now < triage_next_log_ns_) {
    return;
  }
  triage_next_log_ns_ = now + kTriageLogIntervalNs;
  uint64_t gpu_upload_copy_calls = 0;
  uint64_t gpu_upload_copy_total_ns = 0;
  uint64_t gpu_upload_copy_max_ns = 0;
  uint64_t gpu_texture_update_calls = 0;
  uint64_t gpu_texture_update_total_ns = 0;
  uint64_t gpu_texture_update_max_ns = 0;
  uint64_t gpu_texture_update_skipped = 0;
  uint64_t pattern_base_cache_hit_count = 0;
  uint64_t pattern_base_cache_miss_count = 0;
  uint64_t pattern_base_render_total_ns = 0;
  uint64_t pattern_base_render_max_ns = 0;
  uint64_t pattern_base_copy_total_ns = 0;
  uint64_t pattern_base_copy_max_ns = 0;
  uint64_t pattern_base_copy_skipped_count = 0;
  uint64_t pattern_overlay_total_ns = 0;
  uint64_t pattern_overlay_max_ns = 0;
  for (const auto& kv : streams_) {
    const auto& stats = kv.second.renderer.debug_stats();
    pattern_base_cache_hit_count += stats.base_cache_hit_count;
    pattern_base_cache_miss_count += stats.base_cache_miss_count;
    pattern_base_render_total_ns += stats.base_render_total_ns;
    pattern_base_render_max_ns = std::max(pattern_base_render_max_ns, stats.base_render_max_ns);
    pattern_base_copy_total_ns += stats.base_copy_total_ns;
    pattern_base_copy_max_ns = std::max(pattern_base_copy_max_ns, stats.base_copy_max_ns);
    pattern_base_copy_skipped_count += stats.base_copy_skipped_count;
    pattern_overlay_total_ns += stats.overlay_total_ns;
    pattern_overlay_max_ns = std::max(pattern_overlay_max_ns, stats.overlay_max_ns);
  }
  const bool has_gpu_subbucket_stats = synthetic_gpu_backing_take_update_timing_stats(
      gpu_upload_copy_calls,
      gpu_upload_copy_total_ns,
      gpu_upload_copy_max_ns,
      gpu_texture_update_calls,
      gpu_texture_update_total_ns,
      gpu_texture_update_max_ns,
      gpu_texture_update_skipped);
  synthetic_triage_printf(
      "[CamBANG][SyntheticTriageMetrics] total_emitted_frames=%llu catchup_bursts=%llu catchup_max_per_tick=%u "
      "falling_behind_repeats=%llu catchup_cap=%u catchup_ticks_capped=%llu catchup_frames_dropped=%llu",
      static_cast<unsigned long long>(triage_frames_emitted_total_),
      static_cast<unsigned long long>(triage_catchup_bursts_total_),
      triage_catchup_max_frames_in_tick_,
      static_cast<unsigned long long>(triage_falling_behind_repeat_total_),
      triage_catchup_cap_per_tick_,
      static_cast<unsigned long long>(triage_catchup_ticks_capped_total_),
      static_cast<unsigned long long>(triage_catchup_frames_dropped_total_));
  synthetic_triage_printf(
      "[CamBANG][SyntheticGpuMetrics] gpu_update_attempts=%llu gpu_update_failures=%llu gpu_update_retries=%llu "
      "gpu_update_demand_skipped=%llu "
      "capture_gpu_backing_retain_calls=%llu capture_gpu_backing_retain_total_ms=%.3f capture_gpu_backing_retain_max_ms=%.3f "
      "gpu_backing_recreates=%llu gpu_backing_releases=%llu "
      "gpu_ensure_backing_calls=%llu gpu_ensure_backing_total_ms=%.3f gpu_ensure_backing_max_ms=%.3f "
      "gpu_update_total_calls=%llu gpu_update_total_total_ms=%.3f gpu_update_total_max_ms=%.3f "
      "gpu_upload_copy_calls=%llu gpu_upload_copy_total_ms=%.3f gpu_upload_copy_max_ms=%.3f "
      "gpu_texture_update_calls=%llu gpu_texture_update_total_ms=%.3f gpu_texture_update_max_ms=%.3f "
      "gpu_texture_update_skipped=%llu",
      static_cast<unsigned long long>(triage_gpu_update_attempts_total_),
      static_cast<unsigned long long>(triage_gpu_update_failures_total_),
      static_cast<unsigned long long>(triage_gpu_update_retries_total_),
      static_cast<unsigned long long>(triage_gpu_update_demand_skipped_total_),
      static_cast<unsigned long long>(triage_capture_gpu_backing_retain_calls_),
      ns_to_ms(triage_capture_gpu_backing_retain_total_ns_),
      ns_to_ms(triage_capture_gpu_backing_retain_max_ns_),
      static_cast<unsigned long long>(triage_gpu_backing_recreate_total_),
      static_cast<unsigned long long>(triage_gpu_backing_release_total_),
      static_cast<unsigned long long>(triage_gpu_ensure_backing_calls_total_),
      ns_to_ms(triage_gpu_ensure_backing_total_ns_),
      ns_to_ms(triage_gpu_ensure_backing_max_ns_),
      static_cast<unsigned long long>(triage_gpu_update_total_calls_),
      ns_to_ms(triage_gpu_update_total_ns_),
      ns_to_ms(triage_gpu_update_total_max_ns_),
      static_cast<unsigned long long>(has_gpu_subbucket_stats ? gpu_upload_copy_calls : 0),
      ns_to_ms(has_gpu_subbucket_stats ? gpu_upload_copy_total_ns : 0),
      ns_to_ms(has_gpu_subbucket_stats ? gpu_upload_copy_max_ns : 0),
      static_cast<unsigned long long>(has_gpu_subbucket_stats ? gpu_texture_update_calls : 0),
      ns_to_ms(has_gpu_subbucket_stats ? gpu_texture_update_total_ns : 0),
      ns_to_ms(has_gpu_subbucket_stats ? gpu_texture_update_max_ns : 0),
      static_cast<unsigned long long>(has_gpu_subbucket_stats ? gpu_texture_update_skipped : 0));
  synthetic_triage_printf(
      "[CamBANG][SyntheticTimelineMetrics] timeline_pump_calls=%llu timeline_pump_total_ms=%.3f timeline_pump_max_ms=%.3f "
      "timeline_event_exec_calls=%llu timeline_event_exec_total_ms=%.3f timeline_event_exec_max_ms=%.3f "
      "timeline_emit_event_calls=%llu timeline_emit_event_total_ms=%.3f timeline_emit_event_max_ms=%.3f "
      "emit_frame_calls=%llu emit_frame_total_ms=%.3f emit_frame_max_ms=%.3f "
      "frame_copy_calls=%llu frame_copy_total_ms=%.3f frame_copy_max_ms=%.3f "
      "post_frame_calls=%llu post_frame_total_ms=%.3f post_frame_max_ms=%.3f "
      "strand_flush_calls=%llu strand_flush_total_ms=%.3f strand_flush_max_ms=%.3f",
      static_cast<unsigned long long>(triage_timeline_pump_calls_),
      ns_to_ms(triage_timeline_pump_total_ns_),
      ns_to_ms(triage_timeline_pump_max_ns_),
      static_cast<unsigned long long>(triage_timeline_event_exec_calls_),
      ns_to_ms(triage_timeline_event_exec_total_ns_),
      ns_to_ms(triage_timeline_event_exec_max_ns_),
      static_cast<unsigned long long>(triage_timeline_emit_event_calls_),
      ns_to_ms(triage_timeline_emit_event_total_ns_),
      ns_to_ms(triage_timeline_emit_event_max_ns_),
      static_cast<unsigned long long>(triage_emit_frame_calls_),
      ns_to_ms(triage_emit_frame_total_ns_),
      ns_to_ms(triage_emit_frame_max_ns_),
      static_cast<unsigned long long>(triage_frame_copy_calls_),
      ns_to_ms(triage_frame_copy_total_ns_),
      ns_to_ms(triage_frame_copy_max_ns_),
      static_cast<unsigned long long>(triage_post_frame_calls_),
      ns_to_ms(triage_post_frame_total_ns_),
      ns_to_ms(triage_post_frame_max_ns_),
      static_cast<unsigned long long>(triage_strand_flush_calls_),
      ns_to_ms(triage_strand_flush_total_ns_),
      ns_to_ms(triage_strand_flush_max_ns_));
  synthetic_triage_printf(
      "[CamBANG][SyntheticRenderMetrics] frame_render_calls=%llu frame_render_total_ms=%.3f frame_render_max_ms=%.3f "
      "pattern_base_cache_hit_count=%llu pattern_base_cache_miss_count=%llu "
      "pattern_base_render_total_ms=%.3f pattern_base_render_max_ms=%.3f "
      "pattern_base_copy_total_ms=%.3f pattern_base_copy_max_ms=%.3f pattern_base_copy_skipped_count=%llu "
      "pattern_overlay_total_ms=%.3f pattern_overlay_max_ms=%.3f "
      "render_spec_build_total_ms=%.3f render_spec_build_max_ms=%.3f "
      "render_target_prepare_total_ms=%.3f render_target_prepare_max_ms=%.3f "
      "render_allocation_or_resize_count=%llu",
      static_cast<unsigned long long>(triage_frame_render_calls_),
      ns_to_ms(triage_frame_render_total_ns_),
      ns_to_ms(triage_frame_render_max_ns_),
      static_cast<unsigned long long>(pattern_base_cache_hit_count),
      static_cast<unsigned long long>(pattern_base_cache_miss_count),
      ns_to_ms(pattern_base_render_total_ns),
      ns_to_ms(pattern_base_render_max_ns),
      ns_to_ms(pattern_base_copy_total_ns),
      ns_to_ms(pattern_base_copy_max_ns),
      static_cast<unsigned long long>(pattern_base_copy_skipped_count),
      ns_to_ms(pattern_overlay_total_ns),
      ns_to_ms(pattern_overlay_max_ns),
      ns_to_ms(triage_render_spec_build_total_ns_),
      ns_to_ms(triage_render_spec_build_max_ns_),
      ns_to_ms(triage_render_target_prepare_total_ns_),
      ns_to_ms(triage_render_target_prepare_max_ns_),
      static_cast<unsigned long long>(triage_render_allocation_or_resize_count_));
}


SyntheticMetricsSnapshot SyntheticProvider::get_metrics_snapshot_for_host() const {
  SyntheticMetricsSnapshot out{};
  out.current_virtual_timeline_ns = clock_.now_ns();
  uint64_t pattern_base_copy_total_ns = 0;
  uint64_t pattern_overlay_total_ns = 0;
  uint64_t gpu_upload_copy_calls = 0;
  uint64_t gpu_upload_copy_total_ns = 0;
  uint64_t gpu_upload_copy_max_ns = 0;
  uint64_t gpu_texture_update_calls = 0;
  uint64_t gpu_texture_update_total_ns = 0;
  uint64_t gpu_texture_update_max_ns = 0;
  uint64_t gpu_texture_update_skipped = 0;
  for (const auto& kv : streams_) {
    const auto& stats = kv.second.renderer.debug_stats();
    pattern_base_copy_total_ns += stats.base_copy_total_ns;
    pattern_overlay_total_ns += stats.overlay_total_ns;
  }
  out.total_emitted_frames = triage_frames_emitted_total_;
  out.gpu_update_attempts = triage_gpu_update_attempts_total_;
  out.gpu_update_demand_skipped = triage_gpu_update_demand_skipped_total_;
  out.capture_gpu_backing_retain_calls = triage_capture_gpu_backing_retain_calls_;
  const bool has_gpu_subbucket_stats = synthetic_gpu_backing_peek_update_timing_stats(
      gpu_upload_copy_calls,
      gpu_upload_copy_total_ns,
      gpu_upload_copy_max_ns,
      gpu_texture_update_calls,
      gpu_texture_update_total_ns,
      gpu_texture_update_max_ns,
      gpu_texture_update_skipped);
  (void)gpu_upload_copy_max_ns;
  (void)gpu_texture_update_max_ns;
  (void)gpu_texture_update_skipped;
  out.gpu_texture_update_calls = has_gpu_subbucket_stats ? gpu_texture_update_calls : 0;
  out.frame_copy_calls = triage_frame_copy_calls_;
  out.capture_gpu_backing_retain_total_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_total_ns_);
  out.capture_gpu_backing_retain_max_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_max_ns_);
  out.frame_render_total_ms = ns_to_ms(triage_frame_render_total_ns_);
  out.pattern_overlay_total_ms = ns_to_ms(pattern_overlay_total_ns);
  out.pattern_base_copy_total_ms = ns_to_ms(pattern_base_copy_total_ns);
  out.gpu_update_total_total_ms = ns_to_ms(triage_gpu_update_total_ns_);
  out.gpu_upload_copy_total_ms = ns_to_ms(has_gpu_subbucket_stats ? gpu_upload_copy_total_ns : 0);
  out.gpu_texture_update_total_ms = ns_to_ms(has_gpu_subbucket_stats ? gpu_texture_update_total_ns : 0);
  out.catchup_ticks_capped = triage_catchup_ticks_capped_total_;
  out.catchup_frames_dropped = triage_catchup_frames_dropped_total_;
  out.capture_gpu_backing_retain_cpu_primary =
      SyntheticCaptureGpuBackingRetainPostureMetricsSnapshot{};
  out.capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar.calls =
      triage_capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar_.calls;
  out.capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar.total_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar_.total_ns);
  out.capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar.max_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar_.max_ns);
  out.capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar.has_first_call =
      triage_capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar_.has_first_call;
  out.capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar.first_call_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar_.first_call_ns);
  out.capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar.later_calls =
      triage_capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar_.later_calls;
  out.capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar.later_total_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar_.later_total_ns);
  out.capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar.later_max_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_gpu_primary_no_cpu_sidecar_.later_max_ns);
  out.capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar.calls =
      triage_capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar_.calls;
  out.capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar.total_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar_.total_ns);
  out.capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar.max_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar_.max_ns);
  out.capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar.has_first_call =
      triage_capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar_.has_first_call;
  out.capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar.first_call_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar_.first_call_ns);
  out.capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar.later_calls =
      triage_capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar_.later_calls;
  out.capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar.later_total_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar_.later_total_ns);
  out.capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar.later_max_ms =
      ns_to_ms(triage_capture_gpu_backing_retain_gpu_primary_with_cpu_sidecar_.later_max_ns);
  {
    std::lock_guard<std::mutex> triage_lock(
        triage_capture_ready_metrics_mutex_);
    out.capture_ready_stage_cpu_primary.calls =
        triage_capture_ready_stage_cpu_primary_.calls;
    out.capture_ready_stage_cpu_primary.pre_capture_started_total_ms =
        ns_to_ms(
            triage_capture_ready_stage_cpu_primary_.pre_capture_started_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_before_first_member_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_before_first_member_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_member_iteration_gap_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_member_iteration_gap_total_ns);
    out.capture_ready_stage_cpu_primary.post_capture_started_cpu_prep_total_ms =
        ns_to_ms(
            triage_capture_ready_stage_cpu_primary_.post_capture_started_cpu_prep_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_gpu_retain_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_gpu_retain_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_frame_assembly_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_frame_assembly_total_ns);
    out.capture_ready_stage_cpu_primary.post_capture_started_post_frame_total_ms =
        ns_to_ms(
            triage_capture_ready_stage_cpu_primary_.post_capture_started_post_frame_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_before_terminal_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_before_terminal_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_generation_tail_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_generation_tail_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_return_to_capture_lock_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_return_to_capture_lock_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_return_to_finish_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_return_to_finish_begin_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_post_return_timing_record_update_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_post_return_timing_record_update_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_capture_lock_wait_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_capture_lock_wait_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_finish_provider_state_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_finish_provider_state_total_ns);
    out.capture_ready_stage_cpu_primary
        .post_capture_started_after_last_frame_finish_non_state_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .post_capture_started_after_last_frame_finish_non_state_total_ns);
    out.capture_ready_stage_cpu_primary.capture_terminal_post_total_ms =
        ns_to_ms(
            triage_capture_ready_stage_cpu_primary_.capture_terminal_post_total_ns);
    out.capture_ready_stage_cpu_primary.capture_ready_provider_window_total_ms =
        ns_to_ms(triage_capture_ready_stage_cpu_primary_
                     .capture_ready_provider_window_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar.calls =
        triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_.calls;
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar.pre_capture_started_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .pre_capture_started_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_before_first_member_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_before_first_member_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_member_iteration_gap_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_member_iteration_gap_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_cpu_prep_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_cpu_prep_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_gpu_retain_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_gpu_retain_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_frame_assembly_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_frame_assembly_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_post_frame_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_post_frame_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_before_terminal_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_before_terminal_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_generation_tail_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_generation_tail_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_return_to_capture_lock_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_return_to_capture_lock_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_return_to_finish_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_return_to_finish_begin_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_post_return_timing_record_update_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_post_return_timing_record_update_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_capture_lock_wait_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_capture_lock_wait_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_finish_provider_state_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_finish_provider_state_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .post_capture_started_after_last_frame_finish_non_state_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .post_capture_started_after_last_frame_finish_non_state_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar.capture_terminal_post_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .capture_terminal_post_total_ns);
    out.capture_ready_stage_gpu_primary_no_cpu_sidecar
        .capture_ready_provider_window_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_no_cpu_sidecar_
                     .capture_ready_provider_window_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar.calls =
        triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_.calls;
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .pre_capture_started_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .pre_capture_started_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_before_first_member_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_before_first_member_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_member_iteration_gap_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_member_iteration_gap_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_cpu_prep_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_cpu_prep_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_gpu_retain_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_gpu_retain_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_frame_assembly_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_frame_assembly_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_post_frame_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_post_frame_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_before_terminal_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_before_terminal_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_generation_tail_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_generation_tail_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_return_to_capture_lock_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_return_to_capture_lock_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_return_to_finish_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_return_to_finish_begin_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_post_return_timing_record_update_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_post_return_timing_record_update_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_capture_lock_wait_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_capture_lock_wait_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_finish_provider_state_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_finish_provider_state_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .post_capture_started_after_last_frame_finish_non_state_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .post_capture_started_after_last_frame_finish_non_state_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar.capture_terminal_post_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .capture_terminal_post_total_ns);
    out.capture_ready_stage_gpu_primary_with_cpu_sidecar
        .capture_ready_provider_window_total_ms =
        ns_to_ms(triage_capture_ready_stage_gpu_primary_with_cpu_sidecar_
                     .capture_ready_provider_window_total_ns);
    out.capture_ready_timing_records.clear();
    out.capture_ready_timing_records.reserve(
        triage_capture_ready_timing_records_.size());
    for (const CaptureReadyTimingKey& key : triage_capture_ready_timing_order_) {
      const auto it = triage_capture_ready_timing_records_.find(key);
      if (it == triage_capture_ready_timing_records_.end()) {
        continue;
      }
      const CaptureReadyTimingRecord& record = it->second;
      SyntheticCaptureReadyTimingRecordSnapshot snap_record{};
      snap_record.capture_id = record.capture_id;
      snap_record.device_instance_id = record.device_instance_id;
      snap_record.acquisition_session_id = record.acquisition_session_id;
      snap_record.primary_cpu = record.primary_cpu;
      snap_record.retain_cpu_sidecar = record.retain_cpu_sidecar;
      snap_record.has_provider_post_capture_started_steady_ns =
          record.has_provider_post_capture_started_steady_ns;
      snap_record.provider_post_capture_started_steady_ns =
          record.provider_post_capture_started_steady_ns;
      snap_record.has_provider_post_capture_completed_steady_ns =
          record.has_provider_post_capture_completed_steady_ns;
      snap_record.provider_post_capture_completed_steady_ns =
          record.provider_post_capture_completed_steady_ns;
      snap_record.provider_pre_capture_started_total_ns =
          record.provider_pre_capture_started_total_ns;
      snap_record.provider_post_capture_started_before_first_member_total_ns =
          record.provider_post_capture_started_before_first_member_total_ns;
      snap_record.provider_post_capture_started_member_iteration_gap_total_ns =
          record.provider_post_capture_started_member_iteration_gap_total_ns;
      snap_record.provider_post_capture_started_cpu_prep_total_ns =
          record.provider_post_capture_started_cpu_prep_total_ns;
      snap_record.provider_post_capture_started_gpu_retain_total_ns =
          record.provider_post_capture_started_gpu_retain_total_ns;
      snap_record.provider_post_capture_started_frame_assembly_total_ns =
          record.provider_post_capture_started_frame_assembly_total_ns;
      snap_record.provider_post_capture_started_post_frame_total_ns =
          record.provider_post_capture_started_post_frame_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_before_terminal_total_ns =
          record.provider_post_capture_started_after_last_frame_before_terminal_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_generation_tail_total_ns =
          record.provider_post_capture_started_after_last_frame_generation_tail_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_return_to_capture_lock_total_ns =
          record.provider_post_capture_started_after_last_frame_return_to_capture_lock_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_return_to_finish_begin_total_ns =
          record.provider_post_capture_started_after_last_frame_return_to_finish_begin_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ns =
          record.provider_post_capture_started_after_last_frame_post_return_timing_record_lock_wait_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_post_return_timing_record_update_total_ns =
          record.provider_post_capture_started_after_last_frame_post_return_timing_record_update_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ns =
          record.provider_post_capture_started_after_last_frame_post_return_after_timing_record_to_finish_begin_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ns =
          record.provider_post_capture_started_after_last_frame_post_return_after_timing_record_to_pre_return_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ns =
          record.provider_post_capture_started_after_last_frame_pre_return_to_finish_begin_total_ns;
      snap_record.provider_base_bytes_use_count_after_timing_record =
          record.provider_base_bytes_use_count_after_timing_record;
      snap_record.provider_base_bytes_use_count_pre_return =
          record.provider_base_bytes_use_count_pre_return;
      snap_record
          .provider_post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ns =
          record.provider_post_capture_started_after_last_frame_finish_begin_to_capture_lock_wait_begin_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_capture_lock_wait_total_ns =
          record.provider_post_capture_started_after_last_frame_capture_lock_wait_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_finish_provider_state_total_ns =
          record.provider_post_capture_started_after_last_frame_finish_provider_state_total_ns;
      snap_record
          .provider_post_capture_started_after_last_frame_finish_non_state_total_ns =
          record.provider_post_capture_started_after_last_frame_finish_non_state_total_ns;
      snap_record.provider_capture_terminal_post_total_ns =
          record.provider_capture_terminal_post_total_ns;
      snap_record.provider_capture_ready_provider_window_total_ns =
          record.provider_capture_ready_provider_window_total_ns;
      out.capture_ready_timing_records.push_back(snap_record);
    }
  }
  return out;
}

void SyntheticProvider::advance(uint64_t dt_ns,
                                bool allow_paused_timeline_step,
                                bool flush_strand) {
  if (!initialized_ || shutting_down_) {
    return;
  }
  std::unique_lock<std::mutex> state_lock(provider_state_mutex_);

  // Ordinary runtime ticking must respect timeline pause. Explicit host-driven
  // advance_timeline() stepping is the exception: it advances scenario time
  // deterministically while leaving the paused flag in place so automatic
  // ticking remains suspended.
  if (cfg_.synthetic_role == SyntheticRole::Timeline &&
      timeline_running_ &&
      timeline_paused_ &&
      !allow_paused_timeline_step) {
    return;
  }

  // v1: only VirtualTime is implemented. Advancing by dt=0 is still a valid
  // host-stepper operation because timeline_pump_() executes events already due
  // at the current virtual time.
  clock_.advance(dt_ns);
  if (cfg_.synthetic_role == SyntheticRole::Timeline) {
    timeline_pump_(allow_paused_timeline_step);
    if (triage_trace_enabled_ && !triage_timeline_path_banner_emitted_) {
      synthetic_triage_printf("[CamBANG][SyntheticTriage] timeline-advance-path-reached");
      triage_timeline_path_banner_emitted_ = true;
    }
    emit_triage_trace_if_due_();
  } else {
    if (!triage_nominal_path_banner_emitted_) {
      synthetic_triage_printf("[CamBANG][SyntheticTriage] nominal-advance-path-reached");
      triage_nominal_path_banner_emitted_ = true;
    }
    emit_due_frames_();
  }
  // Keep virtual-time advances deterministic from the harness perspective:
  // deliver all provider->core callbacks generated by this advance before
  // returning so publish-only checks do not race queued frame delivery.
  // strand_.flush() blocks on a cross-thread wait for the strand worker; it
  // must not run while provider_state_mutex_ is held (mirrors shutdown(),
  // which already releases this lock before its own flush()/stop() calls).
  // Nothing after this point reads or writes provider_state_mutex_-protected
  // state.
  state_lock.unlock();
  // flush_strand=false is the free-running per-frame tick's path (see the
  // declaration comment in provider.h): frames queued by this advance are
  // delivered asynchronously by the strand worker and integrated by the core
  // pump; tick-bounded snapshot publication is unaffected. Host-stepped and
  // harness callers keep the flush so publish-only checks never race queued
  // delivery.
  if (!flush_strand) {
    return;
  }
  const uint64_t flush_begin_ns = provider_monotonic_now_ns();
  strand_.flush();
  const uint64_t flush_ns = provider_monotonic_now_ns() - flush_begin_ns;
  record_timing_sample(
      flush_ns,
      triage_strand_flush_calls_,
      triage_strand_flush_total_ns_,
      triage_strand_flush_max_ns_);
}

} // namespace cambang

std::vector<cambang::SyntheticStagedRigTopology> cambang::SyntheticProvider::get_staged_rig_topology_for_host() const { return staged_rig_topology_; }
