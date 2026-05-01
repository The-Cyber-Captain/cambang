#include "imaging/synthetic/provider.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

#include "imaging/synthetic/scenario_loader.h"
#include "imaging/api/timeline_teardown_trace.h"
#include "imaging/synthetic/gpu_backing_runtime.h"
#include "pixels/pattern/pattern_render_target.h"
#if __has_include(<godot_cpp/classes/utility_functions.hpp>)
#include <godot_cpp/classes/utility_functions.hpp>
#define CAMBANG_SYNTH_TRIAGE_HAS_GODOT_UTILITY_PRINT 1
#else
#define CAMBANG_SYNTH_TRIAGE_HAS_GODOT_UTILITY_PRINT 0
#endif

namespace cambang {

namespace {

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
  triage_catchup_cap_per_tick_ = env_u32_or_default("CAMBANG_DEV_SYNTH_CATCHUP_CAP", 0);
}

StreamTemplate SyntheticProvider::stream_template() const {
  StreamTemplate t{};
  t.profile.width = cfg_.nominal.width;
  t.profile.height = cfg_.nominal.height;
  t.profile.format_fourcc = cfg_.nominal.format_fourcc;
  t.profile.target_fps_min = cfg_.nominal.fps_num / (cfg_.nominal.fps_den ? cfg_.nominal.fps_den : 1);
  t.profile.target_fps_max = t.profile.target_fps_min;

  // Canonical default per tranche: noise_animated.
  t.picture.preset = PatternPreset::NoiseAnimated;
  t.picture.seed = static_cast<uint32_t>(cfg_.pattern.seed);
  t.picture.generator_fps_num = 30;
  t.picture.generator_fps_den = 1;
  t.picture.overlay_frame_index_offsets = cfg_.pattern.overlay_frame_index;
  t.picture.overlay_moving_bar = true;
  return t;
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
  const ProducerBackingCapabilities runtime_truth = query_stream_producer_capabilities_(profile, picture);
  return apply_verification_backing_override_(runtime_truth);
}

ProducerBackingCapabilities SyntheticProvider::capture_backing_capabilities(
    const CaptureRequest& req) const noexcept {
  const ProducerBackingCapabilities runtime_truth = query_capture_producer_capabilities_(req);
  return apply_verification_backing_override_(runtime_truth);
}

bool SyntheticProvider::has_runtime_gpu_backing_path_() noexcept {
  return synthetic_gpu_backing_runtime_available();
}

ProducerBackingCapabilities SyntheticProvider::query_stream_producer_capabilities_(
    const CaptureProfile& profile,
    const PictureConfig& picture) const noexcept {
  (void)profile;
  (void)picture;
  return ProducerBackingCapabilities{
      true,
      has_runtime_gpu_backing_path_(),
  };
}

ProducerBackingCapabilities SyntheticProvider::query_capture_producer_capabilities_(
    const CaptureRequest& req) const noexcept {
  (void)req;
  return ProducerBackingCapabilities{
      true,
      has_runtime_gpu_backing_path_(),
  };
}

ProducerBackingCapabilities SyntheticProvider::apply_verification_backing_override_(
    ProducerBackingCapabilities runtime_truth) const noexcept {
  // Verification-only advertisement override. Non-release behavior layered on
  // top of producer/runtime truth.
  switch (cfg_.verification_backing_advertisement_override) {
    case SyntheticVerificationBackingAdvertisementOverride::RuntimeTruth:
      return runtime_truth;
    case SyntheticVerificationBackingAdvertisementOverride::ForceCpuOnly:
      return ProducerBackingCapabilities{true, false};
    case SyntheticVerificationBackingAdvertisementOverride::ForceCpuAndGpu:
      return ProducerBackingCapabilities{true, true};
    case SyntheticVerificationBackingAdvertisementOverride::ForceGpuOnly:
      return ProducerBackingCapabilities{false, true};
    default:
      return runtime_truth;
  }
}

bool SyntheticProvider::choose_stream_gpu_preference_(
    ProducerBackingCapabilities capabilities) const noexcept {
  return capabilities.gpu_backed_available;
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
  callbacks_ = callbacks;
  strand_.start(callbacks_, "synthetic_provider");
  initialized_ = true;
  shutting_down_ = false;
  triage_next_log_ns_ = 0;
  synthetic_triage_printf("[CamBANG][SyntheticTriage] enabled=%s catchup_cap=%u",
                          triage_trace_enabled_ ? "true" : "false",
                          triage_catchup_cap_per_tick_);

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
    std::string& error) const {
  out = {};
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

void SyntheticProvider::timeline_pump_() {
  if (!timeline_running_ || timeline_paused_) {
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

    switch (ev.type) {
      case SyntheticEventType::EmitFrame: {
        auto it = streams_.find(ev.stream_id);
        if (it == streams_.end()) {
          break;
        }
        StreamState& s = it->second;
        if (!s.created || !s.started) {
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
  return static_cast<uint32_t>(idx) < cfg_.endpoint_count;
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

uint64_t SyntheticProvider::ensure_native_acquisition_session_(DeviceState& d) {
  if (d.acquisition_session_native_id != 0) {
    return d.acquisition_session_native_id;
  }
  if (!callbacks_) {
    return 0;
  }

  d.acquisition_session_native_id = alloc_native_id_(NativeObjectType::AcquisitionSession);
  if (d.acquisition_session_native_id == 0) {
    return 0;
  }

  NativeObjectCreateInfo info{};
  info.native_id = d.acquisition_session_native_id;
  info.type = static_cast<uint32_t>(NativeObjectType::AcquisitionSession);
  info.root_id = d.root_id;
  info.owner_device_instance_id = d.device_instance_id;
  info.owner_provider_native_id = provider_native_id_;
  info.owner_rig_id = 0;
  info.has_created_ns = true;
  info.created_ns = clock_.now_ns();
  strand_.post_native_object_created(info);
  return d.acquisition_session_native_id;
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

void SyntheticProvider::release_native_acquisition_session_if_unheld_(DeviceState& d) {
  if (d.acquisition_session_stream_refs != 0 || d.acquisition_session_capture_refs != 0) {
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
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::close_device(uint64_t device_instance_id) {
  if (!initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
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
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (it->second.started || it->second.producing) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // Do not retire pool storage while any frame buffer slot is still in use by
  // core/dispatcher ownership. release_frame_ clears in_use when ownership ends.
  for (const auto& slot : it->second.pool) {
    if (slot && slot->in_use.load(std::memory_order_acquire)) {
      return ProviderResult::failure(ProviderError::ERR_BUSY);
    }
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
  const ProducerBackingCapabilities runtime_truth = query_stream_producer_capabilities_(profile, picture);
  s.prefer_gpu_backing = choose_stream_gpu_preference_(runtime_truth);
  s.gpu_staging.resize(size_bytes);

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

ProviderResult SyntheticProvider::set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto it = streams_.find(stream_id);
  if (it == streams_.end() || !it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (!find_preset_info(picture.preset)) {
    invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
  }
  it->second.picture = picture;
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::set_capture_picture_config(uint64_t device_instance_id, const PictureConfig& picture) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
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

ProviderResult SyntheticProvider::trigger_capture(const CaptureRequest& req) {
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (req.capture_id == 0 || req.device_instance_id == 0 || req.width == 0 || req.height == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  auto dev_it = devices_.find(req.device_instance_id);
  if (dev_it == devices_.end() || !dev_it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  const uint32_t fmt = req.format_fourcc == 0 ? FOURCC_RGBA : req.format_fourcc;
  if (!(fmt == FOURCC_RGBA || fmt == FOURCC_BGRA)) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  if (req.width > (std::numeric_limits<uint32_t>::max() / 4u)) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  const uint32_t stride = req.width * 4u;
  const size_t frame_size = static_cast<size_t>(stride) * static_cast<size_t>(req.height);
  auto bytes = std::make_shared<std::vector<std::uint8_t>>();
  bytes->resize(frame_size);
  std::vector<std::uint8_t> gpu_staging;
  gpu_staging.resize(frame_size);

  bool preset_valid = true;
  PatternSpec spec = to_pattern_spec(req.picture, req.width, req.height, PatternSpec::PackedFormat::RGBA8, &preset_valid);
  if (!preset_valid) {
    invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
  }

  PatternRenderTarget dst{};
  dst.data = gpu_staging.data();
  dst.size_bytes = gpu_staging.size();
  dst.width = req.width;
  dst.height = req.height;
  dst.stride_bytes = stride;
  dst.format = PatternSpec::PackedFormat::RGBA8;

  const uint64_t capture_ts_ns = clock_.now_ns();
  PatternOverlayData ov{};
  ov.frame_index = generator_frame_ordinal_from_ns_(capture_ts_ns, req.picture);
  ov.timestamp_ns = capture_ts_ns;
  ov.stream_id = 0;

  CpuPackedPatternRenderer renderer{};
  renderer.render_into(spec, dst, ov);
  const bool gpu_ok = synthetic_gpu_backing_realize_rgba8_via_global_gpu(
      gpu_staging.data(),
      req.width,
      req.height,
      stride,
      *bytes);
  if (!gpu_ok) {
    std::memcpy(bytes->data(), gpu_staging.data(), frame_size);
  }

  if (fmt == FOURCC_BGRA) {
    for (size_t i = 0; i + 3 < bytes->size(); i += 4) {
      std::swap((*bytes)[i], (*bytes)[i + 2]);
    }
  }

  FrameView fv{};
  fv.device_instance_id = req.device_instance_id;
  fv.stream_id = 0;
  fv.capture_id = req.capture_id;
  fv.width = req.width;
  fv.height = req.height;
  fv.format_fourcc = fmt;
  fv.capture_timestamp.value = capture_ts_ns;
  fv.capture_timestamp.tick_ns = 1;
  fv.capture_timestamp.domain = CaptureTimestampDomain::PROVIDER_MONOTONIC;
  fv.data = bytes->data();
  fv.size_bytes = bytes->size();
  fv.stride_bytes = stride;

  auto* lease = new FrameReleaseLease();
  lease->bytes = bytes;
  fv.release = &SyntheticProvider::release_frame_;
  fv.release_user = lease;

  retain_native_acquisition_session_for_capture_(dev_it->second);
  if (dev_it->second.acquisition_session_capture_refs == 0) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  strand_.post_capture_started(req.capture_id, req.device_instance_id);
  strand_.post_frame(fv);
  strand_.post_capture_completed(req.capture_id, req.device_instance_id);
  release_native_acquisition_session_for_capture_(req.device_instance_id);
  return ProviderResult::success();
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
  if (it->second.acquisition_session_native_id != 0) {
    emit_native_destroy_(it->second.acquisition_session_native_id);
    it->second.acquisition_session_native_id = 0;
    it->second.acquisition_session_stream_refs = 0;
    it->second.acquisition_session_capture_refs = 0;
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
    if (!materialize_staged_canonical_scenario_(timeline_scenario_, error)) {
      if (!error.empty()) {
        std::fprintf(stderr, "[Synthetic] canonical scenario materialization failed: %s\n", error.c_str());
      }
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
  }
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
  advance(dt_ns);
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

  // Deterministic teardown order:
  // stop streams, destroy streams, close devices.
  for (auto& kv : streams_) {
    if (kv.second.started) {
      (void)stop_stream(kv.first);
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
  timeline_running_ = false;
  timeline_paused_ = false;

  // Provider native object (BOUND -> ABSENT).
  if (provider_native_id_ != 0) {
    emit_native_destroy_(provider_native_id_);
    provider_native_id_ = 0;
  }

  strand_.flush();
  strand_.stop();

  initialized_ = false;
  callbacks_ = nullptr;
  shutting_down_ = false;
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
  s.live_gpu_backing = synthetic_gpu_backing_create_stream_live_gpu_backing_rgba8(width, height, stride);
  if (!s.live_gpu_backing) {
    return false;
  }
  const uint64_t native_id = alloc_native_id_(NativeObjectType::GpuBacking);
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

  bool preset_valid = true;
  PatternSpec spec = to_pattern_spec(s.picture, w, h, PatternSpec::PackedFormat::RGBA8, &preset_valid);
  if (!preset_valid) {
    invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
  }

  PatternRenderTarget dst{};
  dst.data = s.gpu_staging.data();
  dst.size_bytes = s.gpu_staging.size();
  dst.width = w;
  dst.height = h;
  dst.stride_bytes = stride;
  dst.format = PatternSpec::PackedFormat::RGBA8;

  PatternOverlayData ov{};
  ov.frame_index = generator_frame_ordinal_from_ns_(scheduled_capture_ns, s.picture);
  ov.timestamp_ns = scheduled_capture_ns;
  ov.stream_id = s.req.stream_id;

  s.renderer.render_into(spec, dst, ov);
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
      if (!gpu_ok) {
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
  if (!gpu_ok) {
    // Intentional current-slice behavior: renderer output stages in CPU memory
    // each frame, then uploads to the stream-owned live GPU backing when used.
    std::memcpy(slot->bytes.data(), s.gpu_staging.data(), slot->bytes.size());
  }

  FrameView fv{};
  fv.device_instance_id = s.req.device_instance_id;
  fv.stream_id = s.req.stream_id;
  fv.capture_id = 0;
  fv.width = w;
  fv.height = h;
  fv.format_fourcc = FOURCC_RGBA;
  fv.primary_backing_kind = gpu_ok ? ProducerBackingKind::GPU : ProducerBackingKind::CPU;
  fv.primary_backing_artifact = std::move(gpu_backing);
  fv.capture_timestamp.value = scheduled_capture_ns;
  fv.capture_timestamp.tick_ns = 1;
  fv.capture_timestamp.domain = CaptureTimestampDomain::PROVIDER_MONOTONIC;
  fv.data = gpu_ok ? nullptr : slot->bytes.data();
  fv.size_bytes = gpu_ok ? 0u : slot->bytes.size();
  fv.stride_bytes = gpu_ok ? 0u : stride;
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

  strand_.post_frame(fv);
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
    const bool behind = (s.next_due_ns <= now);
    if (behind) {
      ++s.consecutive_behind_ticks;
      if (s.consecutive_behind_ticks > 1) {
        ++triage_falling_behind_repeat_total_;
      }
    } else {
      s.consecutive_behind_ticks = 0;
    }

    // Emit as many frames as are due (catch-up) in virtual time.
    uint32_t emitted_this_tick = 0;
    while (s.next_due_ns <= now) {
      if (triage_catchup_cap_per_tick_ > 0 && emitted_this_tick >= triage_catchup_cap_per_tick_) {
        ++triage_catchup_ticks_capped_total_;
        while (s.next_due_ns <= now) {
          s.next_due_ns += period;
          ++triage_catchup_frames_dropped_total_;
        }
        break;
      }
      const uint64_t scheduled = s.next_due_ns;
      emit_one_frame_(s, scheduled);
      s.next_due_ns += period;
      ++emitted_this_tick;
      ++triage_frames_emitted_total_;
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
  const bool has_gpu_subbucket_stats = synthetic_gpu_backing_take_update_timing_stats(
      gpu_upload_copy_calls,
      gpu_upload_copy_total_ns,
      gpu_upload_copy_max_ns,
      gpu_texture_update_calls,
      gpu_texture_update_total_ns,
      gpu_texture_update_max_ns);
  synthetic_triage_printf(
      "[cambang][synth-triage] total_emitted_frames=%llu catchup_bursts=%llu catchup_max_per_tick=%u "
      "falling_behind_repeats=%llu catchup_cap=%u catchup_ticks_capped=%llu catchup_frames_dropped=%llu "
      "gpu_update_attempts=%llu gpu_update_failures=%llu gpu_update_retries=%llu "
      "gpu_backing_recreates=%llu gpu_backing_releases=%llu "
      "gpu_ensure_backing_calls=%llu gpu_ensure_backing_total_ms=%.3f gpu_ensure_backing_max_ms=%.3f "
      "gpu_update_total_calls=%llu gpu_update_total_total_ms=%.3f gpu_update_total_max_ms=%.3f "
      "gpu_upload_copy_calls=%llu gpu_upload_copy_total_ms=%.3f gpu_upload_copy_max_ms=%.3f "
      "gpu_texture_update_calls=%llu gpu_texture_update_total_ms=%.3f gpu_texture_update_max_ms=%.3f",
      static_cast<unsigned long long>(triage_frames_emitted_total_),
      static_cast<unsigned long long>(triage_catchup_bursts_total_),
      triage_catchup_max_frames_in_tick_,
      static_cast<unsigned long long>(triage_falling_behind_repeat_total_),
      triage_catchup_cap_per_tick_,
      static_cast<unsigned long long>(triage_catchup_ticks_capped_total_),
      static_cast<unsigned long long>(triage_catchup_frames_dropped_total_),
      static_cast<unsigned long long>(triage_gpu_update_attempts_total_),
      static_cast<unsigned long long>(triage_gpu_update_failures_total_),
      static_cast<unsigned long long>(triage_gpu_update_retries_total_),
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
      ns_to_ms(has_gpu_subbucket_stats ? gpu_texture_update_max_ns : 0));
}

void SyntheticProvider::advance(uint64_t dt_ns) {
  if (!initialized_ || shutting_down_) {
    return;
  }

  // For timeline-role synthetic operation, a host pause must be a true
  // scenario-time pause. Advancing the virtual clock while paused causes due
  // events to accumulate and then burst on unpause, which breaks checkpointed
  // host/timeline synchronization.
  if (cfg_.synthetic_role == SyntheticRole::Timeline && timeline_running_ && timeline_paused_) {
    return;
  }

  // v1: only VirtualTime is implemented.
  clock_.advance(dt_ns);
  if (cfg_.synthetic_role == SyntheticRole::Timeline) {
    timeline_pump_();
    if (!triage_timeline_path_banner_emitted_) {
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
  strand_.flush();
}

} // namespace cambang
