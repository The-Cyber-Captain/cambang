#include "imaging/synthetic/provider.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <chrono>
#include <exception>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>

#include "imaging/synthetic/scenario_loader.h"
#include "imaging/synthetic/gpu_update_policy_resolver.h"
#include "imaging/api/timeline_teardown_trace.h"
#include "imaging/api/capture_latency_trace_diagnostics.h"
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

struct CaptureLatencyDueFrameStats {
  uint64_t frames = 0;
  uint64_t emit_due_total_ns = 0;
  uint64_t emit_one_total_ns = 0;
  uint64_t emit_one_max_ns = 0;
};

thread_local CaptureLatencyDueFrameStats g_capture_latency_due_frame_stats;

constexpr uint64_t kCaptureLatencyAdvanceLogThresholdUs = 5000;
constexpr uint64_t kCaptureLatencyAdvanceSummaryEvery = 64;

struct CaptureLatencyAdvanceSuppressionStats {
  uint64_t calls = 0;
  uint64_t frames = 0;
  uint64_t total_ns = 0;
  uint64_t max_ns = 0;
  uint64_t emit_one_total_ns = 0;
  uint64_t emit_one_max_ns = 0;
  uint64_t flush_total_ns = 0;
  uint64_t state_lock_wait_total_ns = 0;
  uint64_t state_lock_hold_total_ns = 0;
};

thread_local CaptureLatencyAdvanceSuppressionStats g_capture_latency_suppressed_advance_stats;

void capture_latency_trace_note_suppressed_advance(uint64_t frames,
                                                   uint64_t total_ns,
                                                   uint64_t emit_one_total_ns,
                                                   uint64_t emit_one_max_ns,
                                                   uint64_t flush_ns,
                                                   uint64_t state_lock_wait_ns,
                                                   uint64_t state_lock_hold_ns) {
  auto& stats = g_capture_latency_suppressed_advance_stats;
  ++stats.calls;
  stats.frames += frames;
  stats.total_ns += total_ns;
  stats.max_ns = std::max(stats.max_ns, total_ns);
  stats.emit_one_total_ns += emit_one_total_ns;
  stats.emit_one_max_ns = std::max(stats.emit_one_max_ns, emit_one_max_ns);
  stats.flush_total_ns += flush_ns;
  stats.state_lock_wait_total_ns += state_lock_wait_ns;
  stats.state_lock_hold_total_ns += state_lock_hold_ns;
}

void capture_latency_trace_flush_suppressed_advance_summary(const char* reason) {
  auto& stats = g_capture_latency_suppressed_advance_stats;
  if (stats.calls == 0) {
    return;
  }
  capture_latency_trace_printf(
      "synthetic_advance_summary reason=%s suppressed_calls=%llu suppressed_frames=%llu total_us=%llu max_us=%llu emit_one_total_us=%llu emit_one_max_us=%llu flush_us=%llu state_lock_wait_us=%llu state_lock_hold_us=%llu capture_inflight=%u active_capture_count=%u",
      reason,
      static_cast<unsigned long long>(stats.calls),
      static_cast<unsigned long long>(stats.frames),
      static_cast<unsigned long long>(stats.total_ns / 1000ull),
      static_cast<unsigned long long>(stats.max_ns / 1000ull),
      static_cast<unsigned long long>(stats.emit_one_total_ns / 1000ull),
      static_cast<unsigned long long>(stats.emit_one_max_ns / 1000ull),
      static_cast<unsigned long long>(stats.flush_total_ns / 1000ull),
      static_cast<unsigned long long>(stats.state_lock_wait_total_ns / 1000ull),
      static_cast<unsigned long long>(stats.state_lock_hold_total_ns / 1000ull),
      capture_latency_trace_diagnostics::capture_inflight(),
      capture_latency_trace_diagnostics::active_capture_count());
  stats = CaptureLatencyAdvanceSuppressionStats{};
}

void capture_latency_trace_emit_or_suppress_advance(uint64_t dt_ns,
                                                    uint64_t state_lock_wait_ns,
                                                    uint64_t state_lock_hold_ns,
                                                    uint64_t emit_due_ns,
                                                    uint64_t frames,
                                                    uint64_t emit_one_total_ns,
                                                    uint64_t emit_one_max_ns,
                                                    uint64_t flush_ns,
                                                    uint64_t total_ns,
                                                    uint32_t paused) {
  const uint32_t active_capture_count = capture_latency_trace_diagnostics::active_capture_count();
  const bool near_capture = active_capture_count != 0;
  const bool interesting = near_capture || total_ns / 1000ull >= kCaptureLatencyAdvanceLogThresholdUs;
  if (!interesting) {
    capture_latency_trace_note_suppressed_advance(
        frames, total_ns, emit_one_total_ns, emit_one_max_ns, flush_ns, state_lock_wait_ns, state_lock_hold_ns);
    if (g_capture_latency_suppressed_advance_stats.calls >= kCaptureLatencyAdvanceSummaryEvery) {
      capture_latency_trace_flush_suppressed_advance_summary("periodic");
    }
    return;
  }
  capture_latency_trace_flush_suppressed_advance_summary("before_interesting");
  capture_latency_trace_printf(
      "synthetic_advance dt_ns=%llu state_lock_wait_us=%llu state_lock_hold_us=%llu emit_due_us=%llu frames=%llu emit_one_total_us=%llu emit_one_max_us=%llu flush_us=%llu total_us=%llu paused=%u capture_inflight=%u active_capture_count=%u",
      static_cast<unsigned long long>(dt_ns),
      static_cast<unsigned long long>(state_lock_wait_ns / 1000ull),
      static_cast<unsigned long long>(state_lock_hold_ns / 1000ull),
      static_cast<unsigned long long>(emit_due_ns / 1000ull),
      static_cast<unsigned long long>(frames),
      static_cast<unsigned long long>(emit_one_total_ns / 1000ull),
      static_cast<unsigned long long>(emit_one_max_ns / 1000ull),
      static_cast<unsigned long long>(flush_ns / 1000ull),
      static_cast<unsigned long long>(total_ns / 1000ull),
      paused,
      near_capture ? 1u : 0u,
      active_capture_count);
}
// END TEMPORARY CAPTURE LATENCY DIAGNOSTICS

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
    stop_and_join_capture_threads_();
  }
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
  return query_stream_producer_capabilities_(profile, picture);
}

ProducerBackingCapabilities SyntheticProvider::capture_backing_capabilities(
    const CaptureRequest& req) const noexcept {
  return query_capture_producer_capabilities_(req);
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
      return ProducerBackingCapabilities{true, false};
    case SyntheticProducerOutputFormMode::GpuOnly:
      return gpu_available ? ProducerBackingCapabilities{false, true}
                           : ProducerBackingCapabilities{false, false};
    case SyntheticProducerOutputFormMode::CpuAndGpu:
      return gpu_available ? ProducerBackingCapabilities{true, true}
                           : ProducerBackingCapabilities{false, false};
    case SyntheticProducerOutputFormMode::Auto:
    default:
      return ProducerBackingCapabilities{true, gpu_available};
  }
}

ProducerBackingCapabilities SyntheticProvider::query_capture_producer_capabilities_(
    const CaptureRequest& req) const noexcept {
  (void)req;
  const bool gpu_available = has_runtime_gpu_backing_path_();
  switch (cfg_.producer_output_form_mode) {
    case SyntheticProducerOutputFormMode::CpuOnly:
      return ProducerBackingCapabilities{true, false};
    case SyntheticProducerOutputFormMode::GpuOnly:
      return gpu_available ? ProducerBackingCapabilities{false, true}
                           : ProducerBackingCapabilities{false, false};
    case SyntheticProducerOutputFormMode::CpuAndGpu:
      return gpu_available ? ProducerBackingCapabilities{true, true}
                           : ProducerBackingCapabilities{false, false};
    case SyntheticProducerOutputFormMode::Auto:
    default:
      return ProducerBackingCapabilities{true, gpu_available};
  }
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
      return SyntheticProducerOutputFormMode::CpuAndGpu;
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
  callbacks_ = callbacks;
  strand_.start(callbacks_, "synthetic_provider");
  initialized_ = true;
  shutting_down_ = false;
  {
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    capture_admission_closed_ = false;
    capture_latency_trace_first_capture_after_start_.store(true, std::memory_order_relaxed);
    ++capture_generation_;
    in_flight_captures_.clear();
  }
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

void SyntheticProvider::timeline_pump_() {
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
  const ProducerBackingCapabilities runtime_truth = query_stream_producer_capabilities_(profile, picture);
  const bool selected_mode_requires_gpu =
      cfg_.producer_output_form_mode == SyntheticProducerOutputFormMode::GpuOnly ||
      cfg_.producer_output_form_mode == SyntheticProducerOutputFormMode::CpuAndGpu;
  if (selected_mode_requires_gpu && !runtime_truth.gpu_backed_available) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  s.resolved_output_form_mode = resolve_producer_output_form_mode_(runtime_truth);
  s.prefer_gpu_backing = s.resolved_output_form_mode == SyntheticProducerOutputFormMode::GpuOnly ||
                         s.resolved_output_form_mode == SyntheticProducerOutputFormMode::CpuAndGpu;
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
  it->second.picture = picture;
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

ProviderResult SyntheticProvider::trigger_capture(const CaptureRequest& req) {
  CaptureSubmission submission{};
  submission.capture_id = req.capture_id;
  submission.origin = req.rig_id == 0 ? CaptureSubmissionOrigin::DEVICE_CAPTURE : CaptureSubmissionOrigin::RIG_CAPTURE;
  submission.rig_id = req.rig_id;
  submission.device_requests.push_back(req);
  return trigger_capture_submission(submission);
}

ProviderResult SyntheticProvider::trigger_capture_submission(const CaptureSubmission& submission) {
  const uint64_t admission_begin_ns = capture_latency_trace_now_ns();
  CaptureSubmissionJob job{};
  ProviderResult admission_result = ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  uint64_t capture_lock_wait_ns = 0;
  uint64_t provider_lock_wait_ns = 0;
  uint64_t validation_ns = 0;
  {
    const uint64_t capture_lock_wait_begin_ns = capture_latency_trace_now_ns();
    std::unique_lock<std::mutex> capture_lock(capture_mutex_);
    const uint64_t capture_lock_acquired_ns = capture_latency_trace_now_ns();
    capture_lock_wait_ns = capture_lock_acquired_ns - capture_lock_wait_begin_ns;
    const uint64_t provider_lock_wait_begin_ns = capture_latency_trace_now_ns();
    std::unique_lock<std::mutex> state_lock(provider_state_mutex_);
    const uint64_t provider_lock_acquired_ns = capture_latency_trace_now_ns();
    provider_lock_wait_ns = provider_lock_acquired_ns - provider_lock_wait_begin_ns;
    const uint64_t validation_begin_ns = capture_latency_trace_now_ns();
    admission_result = validate_and_admit_capture_submission_locked_(submission, job);
    validation_ns = capture_latency_trace_now_ns() - validation_begin_ns;
    if (!admission_result.ok()) {
      capture_latency_trace_printf(
          "synthetic_admission capture_id=%llu rig_id=%llu origin=%u devices=%llu capture_lock_wait_us=%llu provider_lock_wait_us=%llu validation_us=%llu thread_create_us=0 thread_store_us=0 total_us=%llu ok=0 code=%u",
          static_cast<unsigned long long>(submission.capture_id),
          static_cast<unsigned long long>(submission.rig_id),
          static_cast<unsigned>(submission.origin),
          static_cast<unsigned long long>(submission.device_requests.size()),
          static_cast<unsigned long long>(capture_lock_wait_ns / 1000ull),
          static_cast<unsigned long long>(provider_lock_wait_ns / 1000ull),
          static_cast<unsigned long long>(validation_ns / 1000ull),
          static_cast<unsigned long long>((capture_latency_trace_now_ns() - admission_begin_ns) / 1000ull),
          static_cast<unsigned>(admission_result.code));
      return admission_result;
    }
  }

  try {
    start_capture_thread_(job);
  } catch (const std::exception&) {
    for (const DeviceCaptureJob& device_job : job.device_jobs) {
      finish_device_capture_job_(device_job,
                                 job.generation,
                                 CaptureTerminalKind::Failed,
                                 ProviderError::ERR_PROVIDER_FAILED);
    }
    capture_latency_trace_printf(
        "synthetic_admission capture_id=%llu rig_id=%llu origin=%u devices=%llu capture_lock_wait_us=%llu provider_lock_wait_us=%llu validation_us=%llu total_us=%llu ok=0 code=%u",
        static_cast<unsigned long long>(submission.capture_id),
        static_cast<unsigned long long>(submission.rig_id),
        static_cast<unsigned>(submission.origin),
        static_cast<unsigned long long>(submission.device_requests.size()),
        static_cast<unsigned long long>(capture_lock_wait_ns / 1000ull),
        static_cast<unsigned long long>(provider_lock_wait_ns / 1000ull),
        static_cast<unsigned long long>(validation_ns / 1000ull),
        static_cast<unsigned long long>((capture_latency_trace_now_ns() - admission_begin_ns) / 1000ull),
        static_cast<unsigned>(ProviderError::ERR_PROVIDER_FAILED));
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }
  capture_latency_trace_printf(
      "synthetic_admission capture_id=%llu rig_id=%llu origin=%u devices=%llu capture_lock_wait_us=%llu provider_lock_wait_us=%llu validation_us=%llu total_us=%llu ok=1 code=0",
      static_cast<unsigned long long>(submission.capture_id),
      static_cast<unsigned long long>(submission.rig_id),
      static_cast<unsigned>(submission.origin),
      static_cast<unsigned long long>(submission.device_requests.size()),
      static_cast<unsigned long long>(capture_lock_wait_ns / 1000ull),
      static_cast<unsigned long long>(provider_lock_wait_ns / 1000ull),
      static_cast<unsigned long long>(validation_ns / 1000ull),
      static_cast<unsigned long long>((capture_latency_trace_now_ns() - admission_begin_ns) / 1000ull));
  return ProviderResult::success();
}

ProviderResult SyntheticProvider::validate_and_admit_capture_submission_locked_(
    const CaptureSubmission& submission,
    CaptureSubmissionJob& out_job) {
  if (!initialized_ || shutting_down_ || capture_admission_closed_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (submission.capture_id == 0 || submission.device_requests.empty()) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (submission.origin == CaptureSubmissionOrigin::RIG_CAPTURE && submission.rig_id == 0) {
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
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    if (submission.origin == CaptureSubmissionOrigin::RIG_CAPTURE) {
      if (req.rig_id != submission.rig_id || req.rig_id == 0) {
        return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
      }
    } else if (req.rig_id != 0 || submission.rig_id != 0) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    if (seen_devices.find(req.device_instance_id) != seen_devices.end()) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    seen_devices.emplace(req.device_instance_id, true);

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
    if (!is_valid_capture_still_image_bundle(req.still_image_bundle, supports_multi_image_still_sequence())) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    const ProducerBackingCapabilities capture_truth = query_capture_producer_capabilities_(req);
    const bool selected_mode_requires_gpu =
        cfg_.producer_output_form_mode == SyntheticProducerOutputFormMode::GpuOnly ||
        cfg_.producer_output_form_mode == SyntheticProducerOutputFormMode::CpuAndGpu;
    if (selected_mode_requires_gpu && !capture_truth.gpu_backed_available) {
      return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
    }

    DeviceCaptureJob device_job{};
    device_job.request = req;
    device_job.format_fourcc = fmt;
    device_job.stride_bytes = req.width * 4u;
    device_job.frame_size_bytes = static_cast<size_t>(device_job.stride_bytes) * static_cast<size_t>(req.height);
    job.device_jobs.push_back(std::move(device_job));
  }

  uint64_t retain_total_ns = 0;
  for (DeviceCaptureJob& device_job : job.device_jobs) {
    auto dev_it = devices_.find(device_job.request.device_instance_id);
    if (dev_it == devices_.end() || !dev_it->second.open) {
      for (const DeviceCaptureJob& retained : job.device_jobs) {
        if (retained.acquisition_session_id != 0) {
          release_native_acquisition_session_for_capture_(retained.request.device_instance_id);
        }
      }
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    const uint64_t retain_begin_ns = capture_latency_trace_now_ns();
    retain_native_acquisition_session_for_capture_(dev_it->second);
    retain_total_ns += capture_latency_trace_now_ns() - retain_begin_ns;
    if (dev_it->second.acquisition_session_capture_refs == 0 || dev_it->second.acquisition_session_native_id == 0) {
      for (const DeviceCaptureJob& retained : job.device_jobs) {
        if (retained.acquisition_session_id != 0) {
          release_native_acquisition_session_for_capture_(retained.request.device_instance_id);
        }
      }
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    device_job.acquisition_session_id = dev_it->second.acquisition_session_native_id;
  }

  for (const DeviceCaptureJob& device_job : job.device_jobs) {
    const InFlightCaptureKey key{device_job.request.capture_id, device_job.request.device_instance_id};
    if (in_flight_captures_.find(key) != in_flight_captures_.end()) {
      for (const DeviceCaptureJob& retained : job.device_jobs) {
        if (retained.acquisition_session_id != 0) {
          release_native_acquisition_session_for_capture_(retained.request.device_instance_id);
        }
      }
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
  }

  uint64_t inflight_insert_total_ns = 0;
  for (const DeviceCaptureJob& device_job : job.device_jobs) {
    const uint64_t inflight_begin_ns = capture_latency_trace_now_ns();
    const InFlightCaptureKey key{device_job.request.capture_id, device_job.request.device_instance_id};
    InFlightCaptureDevice in_flight{};
    in_flight.capture_id = device_job.request.capture_id;
    in_flight.device_instance_id = device_job.request.device_instance_id;
    in_flight.acquisition_session_id = device_job.acquisition_session_id;
    in_flight.generation = job.generation;
    in_flight_captures_.emplace(key, in_flight);
    ++capture_pause_depth_by_device_[device_job.request.device_instance_id];
    inflight_insert_total_ns += capture_latency_trace_now_ns() - inflight_begin_ns;
  }

  capture_latency_trace_diagnostics::note_capture_admitted(static_cast<uint32_t>(job.device_jobs.size()));
  capture_latency_trace_printf(
      "synthetic_validate_admit capture_id=%llu rig_id=%llu origin=%u devices=%llu retain_us=%llu inflight_insert_us=%llu capture_inflight=%u active_capture_count=%u",
      static_cast<unsigned long long>(submission.capture_id),
      static_cast<unsigned long long>(submission.rig_id),
      static_cast<unsigned>(submission.origin),
      static_cast<unsigned long long>(job.device_jobs.size()),
      static_cast<unsigned long long>(retain_total_ns / 1000ull),
      static_cast<unsigned long long>(inflight_insert_total_ns / 1000ull),
      capture_latency_trace_diagnostics::capture_inflight(),
      capture_latency_trace_diagnostics::active_capture_count());

  out_job = std::move(job);
  return ProviderResult::success();
}

bool SyntheticProvider::capture_shutdown_requested_() const noexcept {
  std::lock_guard<std::mutex> capture_lock(capture_mutex_);
  return capture_admission_closed_;
}

bool SyntheticProvider::should_stop_capture_job_(uint64_t generation) const noexcept {
  std::lock_guard<std::mutex> capture_lock(capture_mutex_);
  return capture_admission_closed_ || generation != capture_generation_;
}

void SyntheticProvider::start_capture_thread_(const CaptureSubmissionJob& job) {
  const uint64_t construct_begin_ns = capture_latency_trace_now_ns();
  std::thread worker([this, job, construct_begin_ns]() mutable {
    const uint64_t first_instruction_ns = capture_latency_trace_now_ns();
    capture_latency_trace_printf(
        "synthetic_submission_thread_start capture_id=%llu rig_id=%llu origin=%u devices=%llu wake_delay_us=%llu",
        static_cast<unsigned long long>(job.capture_id),
        static_cast<unsigned long long>(job.rig_id),
        static_cast<unsigned>(job.origin),
        static_cast<unsigned long long>(job.device_jobs.size()),
        static_cast<unsigned long long>((first_instruction_ns - construct_begin_ns) / 1000ull));
    run_capture_submission_(std::move(job));
  });
  const uint64_t construct_end_ns = capture_latency_trace_now_ns();
  const uint64_t store_begin_ns = capture_latency_trace_now_ns();
  std::lock_guard<std::mutex> capture_lock(capture_mutex_);
  capture_threads_.push_back(std::move(worker));
  const uint64_t store_end_ns = capture_latency_trace_now_ns();
  capture_latency_trace_printf(
      "synthetic_submission_thread_create capture_id=%llu rig_id=%llu origin=%u devices=%llu thread_construct_us=%llu thread_store_us=%llu",
      static_cast<unsigned long long>(job.capture_id),
      static_cast<unsigned long long>(job.rig_id),
      static_cast<unsigned>(job.origin),
      static_cast<unsigned long long>(job.device_jobs.size()),
      static_cast<unsigned long long>((construct_end_ns - construct_begin_ns) / 1000ull),
      static_cast<unsigned long long>((store_end_ns - store_begin_ns) / 1000ull));
}

void SyntheticProvider::run_capture_submission_(CaptureSubmissionJob job) {
  const uint64_t submission_begin_ns = capture_latency_trace_now_ns();
  if (should_stop_capture_job_(job.generation)) {
    for (const DeviceCaptureJob& device_job : job.device_jobs) {
      finish_device_capture_job_(device_job, job.generation, CaptureTerminalKind::Failed, ProviderError::ERR_SHUTTING_DOWN);
    }
    capture_latency_trace_printf(
        "synthetic_submission_done capture_id=%llu rig_id=%llu origin=%u devices=%llu device_thread_create_us=0 join_us=0 total_us=%llu stopped=1",
        static_cast<unsigned long long>(job.capture_id),
        static_cast<unsigned long long>(job.rig_id),
        static_cast<unsigned>(job.origin),
        static_cast<unsigned long long>(job.device_jobs.size()),
        static_cast<unsigned long long>((capture_latency_trace_now_ns() - submission_begin_ns) / 1000ull));
    return;
  }

  std::vector<std::thread> device_threads;
  device_threads.reserve(job.device_jobs.size());
  uint64_t device_thread_create_total_ns = 0;
  try {
    for (const DeviceCaptureJob& device_job : job.device_jobs) {
      const uint64_t device_construct_begin_ns = capture_latency_trace_now_ns();
      device_threads.emplace_back([this, device_job, generation = job.generation, device_construct_begin_ns]() mutable {
        const uint64_t first_instruction_ns = capture_latency_trace_now_ns();
        capture_latency_trace_printf(
            "synthetic_device_thread_start capture_id=%llu device_id=%llu rig_id=%llu members=%llu wake_delay_us=%llu",
            static_cast<unsigned long long>(device_job.request.capture_id),
            static_cast<unsigned long long>(device_job.request.device_instance_id),
            static_cast<unsigned long long>(device_job.request.rig_id),
            static_cast<unsigned long long>(device_job.request.still_image_bundle.members.size()),
            static_cast<unsigned long long>((first_instruction_ns - device_construct_begin_ns) / 1000ull));
        run_device_capture_job_(std::move(device_job), generation);
      });
      const uint64_t device_construct_end_ns = capture_latency_trace_now_ns();
      device_thread_create_total_ns += device_construct_end_ns - device_construct_begin_ns;
    }
  } catch (const std::exception&) {
    for (const DeviceCaptureJob& device_job : job.device_jobs) {
      finish_device_capture_job_(device_job, job.generation, CaptureTerminalKind::Failed, ProviderError::ERR_PROVIDER_FAILED);
    }
  }
  const uint64_t join_begin_ns = capture_latency_trace_now_ns();
  for (std::thread& device_thread : device_threads) {
    if (device_thread.joinable()) {
      device_thread.join();
    }
  }
  const uint64_t join_end_ns = capture_latency_trace_now_ns();
  capture_latency_trace_printf(
      "synthetic_submission_done capture_id=%llu rig_id=%llu origin=%u devices=%llu device_thread_create_us=%llu join_us=%llu total_us=%llu stopped=0",
      static_cast<unsigned long long>(job.capture_id),
      static_cast<unsigned long long>(job.rig_id),
      static_cast<unsigned>(job.origin),
      static_cast<unsigned long long>(job.device_jobs.size()),
      static_cast<unsigned long long>(device_thread_create_total_ns / 1000ull),
      static_cast<unsigned long long>((join_end_ns - join_begin_ns) / 1000ull),
      static_cast<unsigned long long>((join_end_ns - submission_begin_ns) / 1000ull));
}

void SyntheticProvider::run_device_capture_job_(DeviceCaptureJob job, uint64_t generation) {
  const uint64_t device_job_begin_ns = capture_latency_trace_now_ns();
  if (should_stop_capture_job_(generation)) {
    finish_device_capture_job_(job, generation, CaptureTerminalKind::Failed, ProviderError::ERR_SHUTTING_DOWN);
    capture_latency_trace_printf(
        "synthetic_device_job_done capture_id=%llu device_id=%llu rig_id=%llu members=%llu total_us=%llu ok=0 stopped=1",
        static_cast<unsigned long long>(job.request.capture_id),
        static_cast<unsigned long long>(job.request.device_instance_id),
        static_cast<unsigned long long>(job.request.rig_id),
        static_cast<unsigned long long>(job.request.still_image_bundle.members.size()),
        static_cast<unsigned long long>((capture_latency_trace_now_ns() - device_job_begin_ns) / 1000ull));
    return;
  }

  bool ok = false;
  try {
    ok = generate_device_capture_payloads_(job, generation);
    if (ok) {
      finish_device_capture_job_(job, generation, CaptureTerminalKind::Completed, ProviderError::OK);
    } else {
      finish_device_capture_job_(job, generation, CaptureTerminalKind::Failed, ProviderError::ERR_SHUTTING_DOWN);
    }
  } catch (const std::exception&) {
    finish_device_capture_job_(job, generation, CaptureTerminalKind::Failed, ProviderError::ERR_PROVIDER_FAILED);
    ok = false;
  }
  capture_latency_trace_printf(
      "synthetic_device_job_done capture_id=%llu device_id=%llu rig_id=%llu members=%llu total_us=%llu ok=%u stopped=0",
      static_cast<unsigned long long>(job.request.capture_id),
      static_cast<unsigned long long>(job.request.device_instance_id),
      static_cast<unsigned long long>(job.request.rig_id),
      static_cast<unsigned long long>(job.request.still_image_bundle.members.size()),
      static_cast<unsigned long long>((capture_latency_trace_now_ns() - device_job_begin_ns) / 1000ull),
      ok ? 1u : 0u);
}

bool SyntheticProvider::generate_device_capture_payloads_(const DeviceCaptureJob& job, uint64_t generation) {
  const uint64_t production_begin_ns = capture_latency_trace_now_ns();
  uint64_t staging_alloc_ns = 0;
  uint64_t spec_setup_ns = 0;
  uint64_t timestamp_lock_wait_ns = 0;
  uint64_t base_render_ns = 0;
  uint64_t member_alloc_ns = 0;
  uint64_t member_copy_ns = 0;
  uint64_t member_ev_bgra_ns = 0;
  uint64_t member_post_ns = 0;
  const CaptureRequest& req = job.request;
  const bool first_capture_after_start = capture_latency_trace_first_capture_after_start_.exchange(false, std::memory_order_relaxed);
  if (should_stop_capture_job_(generation)) {
    return false;
  }

  const uint64_t staging_begin_ns = capture_latency_trace_now_ns();
  auto base_bytes = std::make_shared<std::vector<std::uint8_t>>();
  base_bytes->resize(job.frame_size_bytes);
  staging_alloc_ns = capture_latency_trace_now_ns() - staging_begin_ns;

  const uint64_t spec_begin_ns = capture_latency_trace_now_ns();
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
  spec_setup_ns = capture_latency_trace_now_ns() - spec_begin_ns;

  uint64_t capture_ts_ns = 0;
  {
    const uint64_t timestamp_lock_begin_ns = capture_latency_trace_now_ns();
    std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
    timestamp_lock_wait_ns = capture_latency_trace_now_ns() - timestamp_lock_begin_ns;
    capture_ts_ns = clock_.now_ns();
  }
  PatternOverlayData ov{};
  ov.frame_index = generator_frame_ordinal_from_ns_(capture_ts_ns, req.picture);
  ov.timestamp_ns = capture_ts_ns;
  ov.stream_id = 0;

  CpuPackedPatternRenderer renderer{};
  const uint64_t base_render_begin_ns = capture_latency_trace_now_ns();
  renderer.render_into(spec, dst, ov);
  base_render_ns = capture_latency_trace_now_ns() - base_render_begin_ns;
  if (should_stop_capture_job_(generation)) {
    return false;
  }

  strand_.post_capture_started(req.capture_id, req.device_instance_id);
  const auto& members = req.still_image_bundle.members;
  uint32_t default_base_reused = 0;
  for (size_t i = 0; i < members.size(); ++i) {
    if (should_stop_capture_job_(generation)) {
      return false;
    }
    const auto& member = members[i];
    const bool can_reuse_base_for_default =
        i == 0 &&
        member.intended_exposure_compensation_milli_ev == 0 &&
        job.format_fourcc == FOURCC_RGBA;
    std::shared_ptr<std::vector<std::uint8_t>> bytes;
    if (can_reuse_base_for_default) {
      bytes = base_bytes;
      default_base_reused = 1;
    } else {
      const uint64_t member_alloc_begin_ns = capture_latency_trace_now_ns();
      bytes = std::make_shared<std::vector<std::uint8_t>>();
      bytes->resize(job.frame_size_bytes);
      member_alloc_ns += capture_latency_trace_now_ns() - member_alloc_begin_ns;
      const uint64_t member_copy_begin_ns = capture_latency_trace_now_ns();
      std::memcpy(bytes->data(), base_bytes->data(), job.frame_size_bytes);
      member_copy_ns += capture_latency_trace_now_ns() - member_copy_begin_ns;
    }
    const uint64_t member_ev_bgra_begin_ns = capture_latency_trace_now_ns();
    if (member.intended_exposure_compensation_milli_ev != 0) {
      PatternRenderOptions render_options{};
      render_options.applied_exposure_compensation_milli_ev = member.intended_exposure_compensation_milli_ev;
      PatternRenderTarget member_dst{};
      member_dst.data = bytes->data();
      member_dst.size_bytes = bytes->size();
      member_dst.width = req.width;
      member_dst.height = req.height;
      member_dst.stride_bytes = job.stride_bytes;
      member_dst.format = PatternSpec::PackedFormat::RGBA8;
      renderer.apply_render_options_in_place(member_dst, render_options);
    }
    if (job.format_fourcc == FOURCC_BGRA) {
      for (size_t bi = 0; bi + 3 < bytes->size(); bi += 4) {
        std::swap((*bytes)[bi], (*bytes)[bi + 2]);
      }
    }
    member_ev_bgra_ns += capture_latency_trace_now_ns() - member_ev_bgra_begin_ns;

    FrameView fv{};
    fv.device_instance_id = req.device_instance_id;
    fv.stream_id = 0;
    fv.acquisition_session_id = job.acquisition_session_id;
    fv.capture_id = req.capture_id;
    fv.width = req.width;
    fv.height = req.height;
    fv.format_fourcc = job.format_fourcc;
    fv.capture_timestamp.value = capture_ts_ns;
    fv.capture_timestamp.tick_ns = 1;
    fv.capture_timestamp.domain = CaptureTimestampDomain::PROVIDER_MONOTONIC;
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
    const ProducerBackingCapabilities capture_truth = query_capture_producer_capabilities_(req);
    const SyntheticProducerOutputFormMode output_form_mode = resolve_producer_output_form_mode_(capture_truth);
    const bool retain_cpu_payload = output_form_mode != SyntheticProducerOutputFormMode::GpuOnly;
    std::shared_ptr<void> gpu_backing{};
    if (output_form_mode == SyntheticProducerOutputFormMode::GpuOnly ||
        output_form_mode == SyntheticProducerOutputFormMode::CpuAndGpu) {
      gpu_backing = synthetic_gpu_backing_retain_primary_gpu_backing_rgba8(
          bytes->data(), req.width, req.height, job.stride_bytes);
      if (!gpu_backing) {
        return false;
      }
      fv.primary_backing_kind = ProducerBackingKind::GPU;
      fv.primary_backing_artifact = gpu_backing;
      fv.retained_gpu_backing_descriptor.valid = true;
      fv.retained_gpu_backing_descriptor.stream_id = 0;
      fv.retained_gpu_backing_descriptor.backing_id = 0;
      fv.retained_gpu_backing_descriptor.capture_timestamp_ns = capture_ts_ns;
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
    fv.retain_cpu_sidecar = retain_cpu_payload;
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
    const uint64_t member_post_begin_ns = capture_latency_trace_now_ns();
    strand_.post_frame(fv);
    member_post_ns += capture_latency_trace_now_ns() - member_post_begin_ns;
  }
  capture_latency_trace_printf(
      "synthetic_capture_production capture_id=%llu device_id=%llu rig_id=%llu first_capture_after_start=%u members=%llu frame_bytes=%llu staging_alloc_kind=fresh_vector staging_alloc_us=%llu spec_setup_us=%llu timestamp_lock_wait_us=%llu base_render_us=%llu member_alloc_kind=fresh_vector member_alloc_us=%llu member_copy_us=%llu member_ev_bgra_us=%llu member_post_us=%llu default_base_reused=%u total_us=%llu",
      static_cast<unsigned long long>(req.capture_id),
      static_cast<unsigned long long>(req.device_instance_id),
      static_cast<unsigned long long>(req.rig_id),
      first_capture_after_start ? 1u : 0u,
      static_cast<unsigned long long>(members.size()),
      static_cast<unsigned long long>(job.frame_size_bytes),
      static_cast<unsigned long long>(staging_alloc_ns / 1000ull),
      static_cast<unsigned long long>(spec_setup_ns / 1000ull),
      static_cast<unsigned long long>(timestamp_lock_wait_ns / 1000ull),
      static_cast<unsigned long long>(base_render_ns / 1000ull),
      static_cast<unsigned long long>(member_alloc_ns / 1000ull),
      static_cast<unsigned long long>(member_copy_ns / 1000ull),
      static_cast<unsigned long long>(member_ev_bgra_ns / 1000ull),
      static_cast<unsigned long long>(member_post_ns / 1000ull),
      default_base_reused,
      static_cast<unsigned long long>((capture_latency_trace_now_ns() - production_begin_ns) / 1000ull));
  return true;
}

void SyntheticProvider::finish_device_capture_job_(const DeviceCaptureJob& job,
                                                   uint64_t generation,
                                                   CaptureTerminalKind terminal,
                                                   ProviderError error) {
  const uint64_t finish_begin_ns = capture_latency_trace_now_ns();
  uint64_t terminal_post_ns = 0;
  uint64_t session_release_ns = 0;
  bool should_post_terminal = false;
  bool should_release = false;
  {
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
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
      std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
      auto pause_it = capture_pause_depth_by_device_.find(job.request.device_instance_id);
      if (pause_it != capture_pause_depth_by_device_.end()) {
        if (pause_it->second > 1) {
          --pause_it->second;
        } else {
          capture_pause_depth_by_device_.erase(pause_it);
        }
      }
    }
    in_flight_captures_.erase(it);
  }

  if (should_post_terminal) {
    const uint64_t terminal_post_begin_ns = capture_latency_trace_now_ns();
    if (terminal == CaptureTerminalKind::Completed) {
      strand_.post_capture_completed(job.request.capture_id, job.request.device_instance_id);
    } else {
      const ProviderError failure_error = error == ProviderError::OK ? ProviderError::ERR_PROVIDER_FAILED : error;
      strand_.post_capture_failed(job.request.capture_id, job.request.device_instance_id, failure_error);
    }
    terminal_post_ns = capture_latency_trace_now_ns() - terminal_post_begin_ns;
  }
  if (should_release) {
    const uint64_t release_begin_ns = capture_latency_trace_now_ns();
    std::lock_guard<std::mutex> state_lock(provider_state_mutex_);
    release_native_acquisition_session_for_capture_(job.request.device_instance_id);
    session_release_ns = capture_latency_trace_now_ns() - release_begin_ns;
  }
  capture_latency_trace_diagnostics::note_capture_finished();
  capture_latency_trace_printf(
      "synthetic_terminal_cleanup capture_id=%llu device_id=%llu terminal=%u post_us=%llu release_us=%llu total_us=%llu capture_inflight=%u active_capture_count=%u",
      static_cast<unsigned long long>(job.request.capture_id),
      static_cast<unsigned long long>(job.request.device_instance_id),
      terminal == CaptureTerminalKind::Completed ? 1u : 2u,
      static_cast<unsigned long long>(terminal_post_ns / 1000ull),
      static_cast<unsigned long long>(session_release_ns / 1000ull),
      static_cast<unsigned long long>((capture_latency_trace_now_ns() - finish_begin_ns) / 1000ull),
      capture_latency_trace_diagnostics::capture_inflight(),
      capture_latency_trace_diagnostics::active_capture_count());
}

void SyntheticProvider::join_finished_capture_threads_() {
  // std::thread does not expose non-blocking completion detection. Keep this
  // hook for future executor maintenance; shutdown performs the deterministic join.
}

void SyntheticProvider::stop_and_join_capture_threads_() {
  std::vector<std::thread> threads;
  {
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    capture_admission_closed_ = true;
    ++capture_generation_;
    threads.swap(capture_threads_);
  }
  for (std::thread& thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
}

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

  // Close capture admission and wait for accepted capture production to post a
  // terminal fact and release its retained acquisition-session references before
  // native/device teardown below.
  stop_and_join_capture_threads_();

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
    capture_admission_closed_ = false;
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
  const uint64_t emit_one_begin_ns = capture_latency_trace_now_ns();
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

  bool preset_valid = true;
  const auto spec_t0 = std::chrono::steady_clock::now();
  PatternSpec spec = to_pattern_spec(s.picture, w, h, PatternSpec::PackedFormat::RGBA8, &preset_valid);
  const auto spec_t1 = std::chrono::steady_clock::now();
  const uint64_t spec_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(spec_t1 - spec_t0).count());
  triage_render_spec_build_total_ns_ += spec_ns;
  triage_render_spec_build_max_ns_ = std::max(triage_render_spec_build_max_ns_, spec_ns);
  if (!preset_valid) {
    invalid_preset_requests_.fetch_add(1, std::memory_order_relaxed);
  }

  const auto target_t0 = std::chrono::steady_clock::now();
  PatternRenderTarget dst{};
  dst.data = s.gpu_staging.data();
  dst.size_bytes = s.gpu_staging.size();
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
      } else if (s.live_gpu_backing) {
        // Expose the stable stream-live GPU backing object even before the
        // first successful update so one-shot display binding can attach to a
        // direct retained-GPU view that becomes live on subsequent updates.
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

  const bool publish_cpu_payload =
      s.resolved_output_form_mode != SyntheticProducerOutputFormMode::GpuOnly;
  if (publish_cpu_payload) {
    // Preserve a current CPU materialization source for the exact FrameView that
    // is about to be retained. GPU-only mode keeps CPU staging provider-local.
    const auto copy_t0 = std::chrono::steady_clock::now();
    std::memcpy(slot->bytes.data(), s.gpu_staging.data(), slot->bytes.size());
    const auto copy_t1 = std::chrono::steady_clock::now();
    const uint64_t copy_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(copy_t1 - copy_t0).count());
    record_timing_sample(copy_ns, triage_frame_copy_calls_, triage_frame_copy_total_ns_, triage_frame_copy_max_ns_);
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
    fv.retained_gpu_backing_descriptor.capture_timestamp_ns = scheduled_capture_ns;
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
  fv.capture_timestamp.value = scheduled_capture_ns;
  fv.capture_timestamp.tick_ns = 1;
  fv.capture_timestamp.domain = CaptureTimestampDomain::PROVIDER_MONOTONIC;
  fv.retain_cpu_sidecar = publish_cpu_payload;
  if (publish_cpu_payload) {
    fv.data = slot->bytes.data();
    fv.size_bytes = slot->bytes.size();
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
  const uint64_t emit_one_ns = capture_latency_trace_now_ns() - emit_one_begin_ns;
  ++g_capture_latency_due_frame_stats.frames;
  g_capture_latency_due_frame_stats.emit_one_total_ns += emit_one_ns;
  g_capture_latency_due_frame_stats.emit_one_max_ns = std::max(g_capture_latency_due_frame_stats.emit_one_max_ns, emit_one_ns);
}

void SyntheticProvider::emit_due_frames_() {
  g_capture_latency_due_frame_stats = CaptureLatencyDueFrameStats{};
  const uint64_t emit_due_begin_ns = capture_latency_trace_now_ns();
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
  g_capture_latency_due_frame_stats.emit_due_total_ns = capture_latency_trace_now_ns() - emit_due_begin_ns;
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
  out.frame_render_total_ms = ns_to_ms(triage_frame_render_total_ns_);
  out.pattern_overlay_total_ms = ns_to_ms(pattern_overlay_total_ns);
  out.pattern_base_copy_total_ms = ns_to_ms(pattern_base_copy_total_ns);
  out.gpu_update_total_total_ms = ns_to_ms(triage_gpu_update_total_ns_);
  out.gpu_upload_copy_total_ms = ns_to_ms(has_gpu_subbucket_stats ? gpu_upload_copy_total_ns : 0);
  out.gpu_texture_update_total_ms = ns_to_ms(has_gpu_subbucket_stats ? gpu_texture_update_total_ns : 0);
  out.catchup_ticks_capped = triage_catchup_ticks_capped_total_;
  out.catchup_frames_dropped = triage_catchup_frames_dropped_total_;
  return out;
}

void SyntheticProvider::advance(uint64_t dt_ns) {
  const uint64_t advance_begin_ns = capture_latency_trace_now_ns();
  if (!initialized_ || shutting_down_) {
    return;
  }
  const uint64_t state_lock_wait_begin_ns = capture_latency_trace_now_ns();
  std::unique_lock<std::mutex> state_lock(provider_state_mutex_);
  const uint64_t state_lock_acquired_ns = capture_latency_trace_now_ns();

  // For timeline-role synthetic operation, a host pause must be a true
  // scenario-time pause. Advancing the virtual clock while paused causes due
  // events to accumulate and then burst on unpause, which breaks checkpointed
  // host/timeline synchronization.
  if (cfg_.synthetic_role == SyntheticRole::Timeline && timeline_running_ && timeline_paused_) {
    const uint64_t before_unlock_ns = capture_latency_trace_now_ns();
    capture_latency_trace_emit_or_suppress_advance(
        dt_ns,
        state_lock_acquired_ns - state_lock_wait_begin_ns,
        before_unlock_ns - state_lock_acquired_ns,
        0,
        0,
        0,
        0,
        0,
        before_unlock_ns - advance_begin_ns,
        1);
    return;
  }

  // v1: only VirtualTime is implemented. Advancing by dt=0 is still a valid
  // host-stepper operation because timeline_pump_() executes events already due
  // at the current virtual time.
  g_capture_latency_due_frame_stats = CaptureLatencyDueFrameStats{};
  clock_.advance(dt_ns);
  if (cfg_.synthetic_role == SyntheticRole::Timeline) {
    timeline_pump_();
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
  const uint64_t flush_begin_ns = capture_latency_trace_now_ns();
  strand_.flush();
  const uint64_t flush_ns = capture_latency_trace_now_ns() - flush_begin_ns;
  record_timing_sample(
      flush_ns,
      triage_strand_flush_calls_,
      triage_strand_flush_total_ns_,
      triage_strand_flush_max_ns_);
  const uint64_t before_unlock_ns = capture_latency_trace_now_ns();
  capture_latency_trace_emit_or_suppress_advance(
      dt_ns,
      state_lock_acquired_ns - state_lock_wait_begin_ns,
      before_unlock_ns - state_lock_acquired_ns,
      g_capture_latency_due_frame_stats.emit_due_total_ns,
      g_capture_latency_due_frame_stats.frames,
      g_capture_latency_due_frame_stats.emit_one_total_ns,
      g_capture_latency_due_frame_stats.emit_one_max_ns,
      flush_ns,
      before_unlock_ns - advance_begin_ns,
      0);
}

} // namespace cambang

std::vector<cambang::SyntheticStagedRigTopology> cambang::SyntheticProvider::get_staged_rig_topology_for_host() const { return staged_rig_topology_; }
