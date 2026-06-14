// src/core/core_runtime.cpp

#include "core/core_runtime.h"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstdarg>
#include <limits>
#include <memory>
#include <map>
#include <future>
#include <vector>

#include <utility>

#include "imaging/broker/banner_info.h"
#include "core/resource_aggregate_telemetry.h"
#include "imaging/api/timeline_teardown_trace.h"
#include "imaging/api/capture_latency_trace_diagnostics.h"

namespace cambang {

namespace {

constexpr uint64_t kNsPerMs = 1000000ull;

// Stage A command-fairness bounds: provider facts remain FIFO and non-dropping,
// but Core yields to pending request work after deterministic slices so sustained
// stream/provider-event production cannot starve public commands. When requests
// are already queued, use the smallest provider-fact service slice before the
// request turn to honor capture-over-stream command fairness promptly.
constexpr size_t kMaxProviderFactsPerCoreTurn = 64;
constexpr size_t kMaxProviderFactsBeforeRequestWhenRequestsPending = 1;

// BEGIN TEMPORARY CAPTURE LATENCY DIAGNOSTICS
uint64_t capture_latency_trace_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void capture_latency_trace_printf(const char* format, ...) {
  char buffer[2048];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  capture_latency_trace_diagnostics::print_line(buffer);
}
// END TEMPORARY CAPTURE LATENCY DIAGNOSTICS


class CoreRuntimeDiagnosticPhaseScope final {
public:
  CoreRuntimeDiagnosticPhaseScope(CoreThread& core_thread, CoreThread::DiagnosticPhase phase)
      : core_thread_(core_thread) {
    core_thread_.diagnostic_set_phase_from_core(phase);
  }
  ~CoreRuntimeDiagnosticPhaseScope() = default;

  CoreRuntimeDiagnosticPhaseScope(const CoreRuntimeDiagnosticPhaseScope&) = delete;
  CoreRuntimeDiagnosticPhaseScope& operator=(const CoreRuntimeDiagnosticPhaseScope&) = delete;

private:
  CoreThread& core_thread_;
};

void capture_latency_trace_emit_core_command_wait_context(
    const char* kind,
    uint64_t capture_id,
    uint64_t target_id,
    const char* target_label,
    uint64_t wait_us,
    const CoreThread::DiagnosticSnapshot& post_snapshot,
    const CoreThread::DiagnosticSnapshot& start_snapshot,
    size_t runtime_requests_depth_at_start,
    size_t runtime_provider_facts_depth_at_start) {
  capture_latency_trace_printf(
      "core_command_wait_context kind=%s capture_id=%llu %s=%llu wait_us=%llu "
      "phase_at_post=%s phase_age_at_post_us=%llu previous_phase_at_post=%s previous_phase_ended_before_post_us=%llu "
      "essential_depth_at_post=%llu command_depth_at_post=%llu ordinary_depth_at_post=%llu timer_requested_at_post=%u timer_running_at_post=%u "
      "phase_at_start=%s phase_age_at_start_us=%llu previous_phase_at_start=%s previous_phase_ended_before_start_us=%llu "
      "essential_depth_at_start=%llu command_depth_at_start=%llu ordinary_depth_at_start=%llu timer_requested_at_start=%u timer_running_at_start=%u "
      "runtime_request_depth_at_start=%llu runtime_provider_fact_depth_at_start=%llu",
      kind,
      static_cast<unsigned long long>(capture_id),
      target_label,
      static_cast<unsigned long long>(target_id),
      static_cast<unsigned long long>(wait_us),
      CoreThread::diagnostic_phase_name(post_snapshot.phase),
      static_cast<unsigned long long>(post_snapshot.phase_age_us),
      CoreThread::diagnostic_phase_name(post_snapshot.previous_phase),
      static_cast<unsigned long long>(post_snapshot.previous_phase_ended_before_us),
      static_cast<unsigned long long>(post_snapshot.essential_queue_depth),
      static_cast<unsigned long long>(post_snapshot.command_queue_depth),
      static_cast<unsigned long long>(post_snapshot.ordinary_queue_depth),
      post_snapshot.timer_requested ? 1u : 0u,
      post_snapshot.timer_running ? 1u : 0u,
      CoreThread::diagnostic_phase_name(start_snapshot.phase),
      static_cast<unsigned long long>(start_snapshot.phase_age_us),
      CoreThread::diagnostic_phase_name(start_snapshot.previous_phase),
      static_cast<unsigned long long>(start_snapshot.previous_phase_ended_before_us),
      static_cast<unsigned long long>(start_snapshot.essential_queue_depth),
      static_cast<unsigned long long>(start_snapshot.command_queue_depth),
      static_cast<unsigned long long>(start_snapshot.ordinary_queue_depth),
      start_snapshot.timer_requested ? 1u : 0u,
      start_snapshot.timer_running ? 1u : 0u,
      static_cast<unsigned long long>(runtime_requests_depth_at_start),
      static_cast<unsigned long long>(runtime_provider_facts_depth_at_start));
}

// Stage B.1 provider-fact classification. Temporary diagnostics above remain
// in place; this helper implements the existing capture-over-stream policy at
// the provider-fact integration seam without changing public/provider APIs.
enum class ProviderFactClass : uint8_t {
  CriticalNonLossy = 0,
  RigCaptureCritical,
  DeviceCaptureCritical,
  RepeatingStreamFrame,
  OtherStream,
  UnknownNonLossy,
};

struct ProviderFactSummary {
  ProviderFactClass fact_class = ProviderFactClass::UnknownNonLossy;
  ProviderToCoreCommandType type = ProviderToCoreCommandType::INVALID;
  uint64_t capture_id = 0;
  uint64_t rig_id = 0;
  uint64_t device_instance_id = 0;
  uint64_t stream_id = 0;
  uint64_t acquisition_session_id = 0;
  uint32_t image_member_index = 0;
};

const char* provider_fact_class_name(ProviderFactClass fact_class) noexcept {
  switch (fact_class) {
    case ProviderFactClass::CriticalNonLossy: return "critical_non_lossy";
    case ProviderFactClass::RigCaptureCritical: return "rig_capture_critical";
    case ProviderFactClass::DeviceCaptureCritical: return "device_capture_critical";
    case ProviderFactClass::RepeatingStreamFrame: return "repeating_stream_frame";
    case ProviderFactClass::OtherStream: return "other_stream";
    case ProviderFactClass::UnknownNonLossy: return "unknown_non_lossy";
  }
  return "unknown_non_lossy";
}

bool provider_fact_is_capture_critical(ProviderFactClass fact_class) noexcept {
  return fact_class == ProviderFactClass::RigCaptureCritical ||
         fact_class == ProviderFactClass::DeviceCaptureCritical;
}

bool provider_fact_has_capture_id_for_priority(const ProviderToCoreCommand& cmd) {
  switch (cmd.type) {
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_STARTED:
      return std::get<CmdProviderCaptureStarted>(cmd.payload).capture_id != 0;
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_COMPLETED:
      return std::get<CmdProviderCaptureCompleted>(cmd.payload).capture_id != 0;
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_FAILED:
      return std::get<CmdProviderCaptureFailed>(cmd.payload).capture_id != 0;
    case ProviderToCoreCommandType::PROVIDER_FRAME:
      return std::get<CmdProviderFrame>(cmd.payload).frame.capture_id != 0;
    default:
      return false;
  }
}

ProviderFactClass provider_capture_fact_class(uint64_t capture_id,
                                              const CoreCaptureCohortRegistry& capture_cohorts) noexcept {
  if (capture_id == 0) {
    return ProviderFactClass::UnknownNonLossy;
  }
  return capture_cohorts.contains(capture_id)
      ? ProviderFactClass::RigCaptureCritical
      : ProviderFactClass::DeviceCaptureCritical;
}

ProviderFactSummary summarize_provider_fact(const ProviderToCoreCommand& cmd,
                                            const CoreCaptureCohortRegistry& capture_cohorts) {
  ProviderFactSummary out{};
  out.type = cmd.type;

  switch (cmd.type) {
    case ProviderToCoreCommandType::PROVIDER_DEVICE_OPENED: {
      const auto& p = std::get<CmdProviderDeviceOpened>(cmd.payload);
      out.device_instance_id = p.device_instance_id;
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_DEVICE_CLOSED: {
      const auto& p = std::get<CmdProviderDeviceClosed>(cmd.payload);
      out.device_instance_id = p.device_instance_id;
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_CREATED: {
      const auto& p = std::get<CmdProviderStreamCreated>(cmd.payload);
      out.stream_id = p.stream_id;
      out.fact_class = ProviderFactClass::OtherStream;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_DESTROYED: {
      const auto& p = std::get<CmdProviderStreamDestroyed>(cmd.payload);
      out.stream_id = p.stream_id;
      out.fact_class = ProviderFactClass::OtherStream;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_STARTED: {
      const auto& p = std::get<CmdProviderStreamStarted>(cmd.payload);
      out.stream_id = p.stream_id;
      out.fact_class = ProviderFactClass::OtherStream;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_STOPPED: {
      const auto& p = std::get<CmdProviderStreamStopped>(cmd.payload);
      out.stream_id = p.stream_id;
      out.fact_class = ProviderFactClass::OtherStream;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_STARTED: {
      const auto& p = std::get<CmdProviderCaptureStarted>(cmd.payload);
      out.capture_id = p.capture_id;
      out.device_instance_id = p.device_instance_id;
      out.fact_class = provider_capture_fact_class(p.capture_id, capture_cohorts);
      if (const CoreCaptureCohortRegistry::CohortRecord* cohort = capture_cohorts.find(p.capture_id)) {
        out.rig_id = cohort->rig_id;
      }
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_COMPLETED: {
      const auto& p = std::get<CmdProviderCaptureCompleted>(cmd.payload);
      out.capture_id = p.capture_id;
      out.device_instance_id = p.device_instance_id;
      out.fact_class = provider_capture_fact_class(p.capture_id, capture_cohorts);
      if (const CoreCaptureCohortRegistry::CohortRecord* cohort = capture_cohorts.find(p.capture_id)) {
        out.rig_id = cohort->rig_id;
      }
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_CAPTURE_FAILED: {
      const auto& p = std::get<CmdProviderCaptureFailed>(cmd.payload);
      out.capture_id = p.capture_id;
      out.device_instance_id = p.device_instance_id;
      out.fact_class = provider_capture_fact_class(p.capture_id, capture_cohorts);
      if (const CoreCaptureCohortRegistry::CohortRecord* cohort = capture_cohorts.find(p.capture_id)) {
        out.rig_id = cohort->rig_id;
      }
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_FRAME: {
      const auto& p = std::get<CmdProviderFrame>(cmd.payload);
      out.capture_id = p.frame.capture_id;
      out.device_instance_id = p.frame.device_instance_id;
      out.stream_id = p.frame.stream_id;
      out.acquisition_session_id = p.frame.acquisition_session_id;
      out.image_member_index = p.frame.capture_image.image_member_index;
      if (p.frame.capture_id != 0) {
        out.fact_class = provider_capture_fact_class(p.frame.capture_id, capture_cohorts);
        if (const CoreCaptureCohortRegistry::CohortRecord* cohort = capture_cohorts.find(p.frame.capture_id)) {
          out.rig_id = cohort->rig_id;
        }
      } else if (p.frame.stream_id != 0) {
        out.fact_class = ProviderFactClass::RepeatingStreamFrame;
      } else {
        out.fact_class = ProviderFactClass::UnknownNonLossy;
      }
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_DEVICE_ERROR: {
      const auto& p = std::get<CmdProviderDeviceError>(cmd.payload);
      out.device_instance_id = p.device_instance_id;
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_ERROR: {
      const auto& p = std::get<CmdProviderStreamError>(cmd.payload);
      out.stream_id = p.stream_id;
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_CREATED: {
      const auto& p = std::get<CmdProviderNativeObjectCreated>(cmd.payload);
      out.device_instance_id = p.owner_device_instance_id;
      out.stream_id = p.owner_stream_id;
      out.rig_id = p.owner_rig_id;
      out.acquisition_session_id = p.owner_acquisition_session_id;
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    }
    case ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_DESTROYED:
      out.fact_class = ProviderFactClass::CriticalNonLossy;
      break;
    case ProviderToCoreCommandType::TIMER_TICK:
    case ProviderToCoreCommandType::INVALID:
      out.fact_class = ProviderFactClass::UnknownNonLossy;
      break;
  }

  return out;
}

bool promote_capture_fact_over_repeating_stream_prefix(
    std::deque<ProviderToCoreCommand>& provider_facts,
    const CoreCaptureCohortRegistry& capture_cohorts) {
  size_t skipped_stream_frames = 0;
  for (auto it = provider_facts.begin(); it != provider_facts.end(); ++it) {
    const ProviderFactSummary summary = summarize_provider_fact(*it, capture_cohorts);
    if (summary.fact_class == ProviderFactClass::RepeatingStreamFrame) {
      ++skipped_stream_frames;
      continue;
    }

    if (skipped_stream_frames != 0 && provider_fact_is_capture_critical(summary.fact_class)) {
      ProviderToCoreCommand promoted = std::move(*it);
      provider_facts.erase(it);
      provider_facts.push_front(std::move(promoted));
      capture_latency_trace_printf(
          "capture_fact_promoted_over_stream class=%s capture_id=%llu rig_id=%llu device_id=%llu stream_frames_skipped=%llu provider_fact_depth=%llu type=%u member=%u",
          provider_fact_class_name(summary.fact_class),
          static_cast<unsigned long long>(summary.capture_id),
          static_cast<unsigned long long>(summary.rig_id),
          static_cast<unsigned long long>(summary.device_instance_id),
          static_cast<unsigned long long>(skipped_stream_frames),
          static_cast<unsigned long long>(provider_facts.size()),
          static_cast<unsigned>(summary.type),
          static_cast<unsigned>(summary.image_member_index));
      return true;
    }

    return false;
  }

  return false;
}

uint64_t frame_ts_to_core_ns_for_backpressure(const CaptureTimestamp& ts) {
  if (ts.tick_ns == 0) {
    return 0;
  }
  switch (ts.domain) {
    case CaptureTimestampDomain::CORE_MONOTONIC:
    case CaptureTimestampDomain::PROVIDER_MONOTONIC:
      return static_cast<uint64_t>(ts.value) * static_cast<uint64_t>(ts.tick_ns);
    default:
      return 0;
  }
}

bool has_newer_repeating_stream_frame_before_barrier(
    const std::deque<ProviderToCoreCommand>& provider_facts,
    const ProviderFactSummary& front_summary,
    const CoreCaptureCohortRegistry& capture_cohorts) {
  if (front_summary.fact_class != ProviderFactClass::RepeatingStreamFrame ||
      front_summary.stream_id == 0) {
    return false;
  }

  bool skipped_front = false;
  for (const ProviderToCoreCommand& cmd : provider_facts) {
    if (!skipped_front) {
      skipped_front = true;
      continue;
    }

    const ProviderFactSummary summary = summarize_provider_fact(cmd, capture_cohorts);
    if (summary.fact_class != ProviderFactClass::RepeatingStreamFrame) {
      return false;
    }
    if (summary.stream_id == front_summary.stream_id &&
        summary.acquisition_session_id == front_summary.acquisition_session_id) {
      return true;
    }
  }

  return false;
}

struct StreamFrameCoalesceResult {
  bool coalesced = false;
  bool stream_counters_changed = false;
};

StreamFrameCoalesceResult coalesce_front_repeating_stream_frame_if_superseded(
    std::deque<ProviderToCoreCommand>& provider_facts,
    const ProviderFactSummary& front_summary,
    const CoreCaptureCohortRegistry& capture_cohorts,
    CoreStreamRegistry& streams) {
  StreamFrameCoalesceResult out{};
  if (!has_newer_repeating_stream_frame_before_barrier(provider_facts, front_summary, capture_cohorts)) {
    return out;
  }

  ProviderToCoreCommand cmd = std::move(provider_facts.front());
  provider_facts.pop_front();
  auto& frame = std::get<CmdProviderFrame>(cmd.payload).frame;
  const uint64_t integrated_ts_ns = frame_ts_to_core_ns_for_backpressure(frame.capture_timestamp);
  const bool received_counted = streams.on_frame_received(frame.stream_id, integrated_ts_ns);
  const bool released_counted = streams.on_frame_released(frame.stream_id);
  const bool dropped_counted = streams.on_frame_dropped(frame.stream_id);
  frame.release_now();
  global_resource_aggregate_telemetry().lease_released(make_framebuffer_lease_scoped_resource_telemetry_key(
      frame.stream_id,
      frame.acquisition_session_id));
  frame.release = nullptr;
  frame.release_user = nullptr;

  out.coalesced = true;
  out.stream_counters_changed = received_counted || released_counted || dropped_counted;
  capture_latency_trace_printf(
      "stream_frame_coalesced stream_id=%llu acquisition_session_id=%llu provider_fact_depth_after=%llu received_counted=%u released_counted=%u dropped_counted=%u delivered=0 publication_requested=1",
      static_cast<unsigned long long>(front_summary.stream_id),
      static_cast<unsigned long long>(front_summary.acquisition_session_id),
      static_cast<unsigned long long>(provider_facts.size()),
      received_counted ? 1u : 0u,
      released_counted ? 1u : 0u,
      dropped_counted ? 1u : 0u);
  return out;
}

constexpr uint64_t kProviderFactDispatchSlowThresholdUs = 5000;

void emit_provider_fact_dispatch_slow_if_needed(const ProviderFactSummary& summary,
                                                uint64_t dispatch_us,
                                                size_t provider_fact_depth_after_dispatch) {
  if (dispatch_us < kProviderFactDispatchSlowThresholdUs) {
    return;
  }
  capture_latency_trace_printf(
      "provider_fact_dispatch_slow class=%s type=%u capture_id=%llu rig_id=%llu device_id=%llu stream_id=%llu acquisition_session_id=%llu member=%u dispatch_us=%llu provider_fact_depth_after=%llu",
      provider_fact_class_name(summary.fact_class),
      static_cast<unsigned>(summary.type),
      static_cast<unsigned long long>(summary.capture_id),
      static_cast<unsigned long long>(summary.rig_id),
      static_cast<unsigned long long>(summary.device_instance_id),
      static_cast<unsigned long long>(summary.stream_id),
      static_cast<unsigned long long>(summary.acquisition_session_id),
      static_cast<unsigned>(summary.image_member_index),
      static_cast<unsigned long long>(dispatch_us),
      static_cast<unsigned long long>(provider_fact_depth_after_dispatch));
}

uint64_t warm_delay_ns(uint32_t warm_hold_ms) {
  if (warm_hold_ms == 0) {
    return 0;
  }
  if (warm_hold_ms >= (std::numeric_limits<uint64_t>::max() / kNsPerMs)) {
    return std::numeric_limits<uint64_t>::max();
  }
  return static_cast<uint64_t>(warm_hold_ms) * kNsPerMs;
}

bool seed_retained_device_still_profile_from_template(CoreDeviceRegistry& devices,
                                                      uint64_t device_instance_id,
                                                      const CaptureTemplate& capture_tmpl) {
  if (device_instance_id == 0) {
    return false;
  }
  if (const auto* rec = devices.find(device_instance_id)) {
    if (rec->capture_profile_version != 0 ||
        rec->capture_width != 0 ||
        rec->capture_height != 0 ||
        rec->capture_format != 0) {
      return false;
    }
  }

  const uint32_t format = capture_tmpl.profile.format_fourcc == 0
      ? FOURCC_RGBA
      : capture_tmpl.profile.format_fourcc;
  return devices.retain_capture_profile(device_instance_id,
                                         capture_tmpl.profile.width,
                                         capture_tmpl.profile.height,
                                         format,
                                         /*capture_profile_version=*/0);
}

} // namespace

static bool banners_enabled() noexcept {
  const char* v = std::getenv("CAMBANG_BANNERS");
  // Spec: CAMBANG_BANNERS=0 disables banners.
  return !(v && v[0] == '0' && v[1] == '\0');
}

static bool disable_result_routing_requested() noexcept {
  const char* v = std::getenv("CAMBANG_DISABLE_RESULT_ROUTING");
  return (v && v[0] == '1' && v[1] == '\0');
}

static bool display_demand_trace_enabled() noexcept {
  const char* v = std::getenv("CAMBANG_DEV_DISPLAY_DEMAND_TRACE");
  return v && v[0] != '\0' && v[0] != '0';
}

CoreRuntime::CoreRuntime()
    : core_thread_(),
      devices_(),
      streams_(),
      gen_counter_(0),
      current_gen_(0),
      version_(0),
      topology_version_(0),
      last_topology_sig_(0),
      dispatcher_(&streams_, &acquisition_sessions_, &devices_, &native_objects_, &current_gen_, [this]() -> uint64_t {
        const auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_).count());
      }, [this]() -> bool {
        return state_.load(std::memory_order_acquire) == CoreRuntimeState::LIVE;
      }),
      ingress_(&core_thread_, [this](ProviderToCoreCommand&& cmd) {
        // This lambda is executed ONLY on the core thread (posted by ingress).
        // Provider callbacks are "facts"; we enqueue them and process them before requests
        // on each core pump tick.
        assert(core_thread_.is_core_thread());
        enqueue_provider_fact(std::move(cmd));
      }, [this]() -> uint64_t {
        // Core monotonic timebase: ns since core epoch_ (session-relative).
        const auto now = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_).count());
      }, [this, demand_last = std::map<uint64_t, bool>{}](uint64_t stream_id) mutable -> bool {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t now_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_).count());
        const auto state = result_store_.get_stream_display_demand_state(stream_id, now_ns);
        if (display_demand_trace_enabled()) {
          const bool prev = demand_last[stream_id];
          if (prev != state.active) {
            const char* reason = "none";
            if (state.reason == CoreResultStore::DisplayDemandReason::PERSISTENT_REFCOUNT) {
              reason = "persistent_refcount";
            } else if (state.reason == CoreResultStore::DisplayDemandReason::LEASE) {
              reason = "lease";
            }
            std::printf("[CamBANG][DemandTrace] demand_transition stream_id=%llu active=%d reason=%s refcount=%u\n",
                        static_cast<unsigned long long>(stream_id),
                        state.active ? 1 : 0,
                        reason,
                        state.refcount);
            demand_last[stream_id] = state.active;
          }
        }
        return state.active;
      }) {
  dispatcher_.set_result_store(&result_store_);
  dispatcher_.set_capture_assembly_registry(&capture_assembly_registry_);
  const bool result_routing_enabled = !disable_result_routing_requested();
  dispatcher_.set_result_routing_enabled(result_routing_enabled);
}

CoreRuntime::~CoreRuntime() {
  stop();
}

SharedCaptureResultData CoreRuntime::get_capture_result(uint64_t capture_id, uint64_t device_instance_id) const {
  if (!capture_assembly_registry_.is_assembly_successful(capture_id, device_instance_id)) {
    return nullptr;
  }
  return result_store_.get_capture_result(capture_id, device_instance_id);
}


void CoreRuntime::begin_capture_stream_preemption_(uint64_t capture_id, uint64_t device_instance_id) {
  assert(core_thread_.is_core_thread());
  if (capture_id == 0 || device_instance_id == 0) {
    return;
  }

  auto& by_capture = capture_stream_preemptions_by_device_[device_instance_id];
  const bool was_empty = by_capture.empty();
  by_capture[capture_id] = CaptureStreamPreemptionRecord{capture_id, device_instance_id};
  if (was_empty) {
    capture_latency_trace_printf(
        "stream_preempted_for_capture capture_id=%llu device_id=%llu active_captures_for_device=%llu",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>(by_capture.size()));
  }
}

void CoreRuntime::begin_capture_stream_preemption_for_bundle_(const RigAdmittedRequestBundle& bundle) {
  assert(core_thread_.is_core_thread());
  if (!bundle.ok || bundle.capture_id == 0) {
    return;
  }
  for (const RigAdmittedParticipantRequest& participant : bundle.participants) {
    begin_capture_stream_preemption_(bundle.capture_id, participant.request.device_instance_id);
  }
}

void CoreRuntime::release_result_safe_capture_stream_preemptions_() {
  assert(core_thread_.is_core_thread());
  for (auto device_it = capture_stream_preemptions_by_device_.begin();
       device_it != capture_stream_preemptions_by_device_.end();) {
    const uint64_t device_instance_id = device_it->first;
    auto& by_capture = device_it->second;
    for (auto capture_it = by_capture.begin(); capture_it != by_capture.end();) {
      const uint64_t capture_id = capture_it->first;
      if (!capture_assembly_registry_.is_result_safe(capture_id, device_instance_id)) {
        ++capture_it;
        continue;
      }
      capture_latency_trace_printf(
          "stream_preemption_released capture_id=%llu device_id=%llu",
          static_cast<unsigned long long>(capture_id),
          static_cast<unsigned long long>(device_instance_id));
      capture_it = by_capture.erase(capture_it);
    }
    if (by_capture.empty()) {
      device_it = capture_stream_preemptions_by_device_.erase(device_it);
    } else {
      ++device_it;
    }
  }
}

bool CoreRuntime::is_stream_preempted_for_capture_(uint64_t stream_id) const {
  assert(core_thread_.is_core_thread());
  if (stream_id == 0) {
    return false;
  }
  const CoreStreamRegistry::StreamRecord* stream = streams_.find(stream_id);
  if (!stream || stream->device_instance_id == 0) {
    return false;
  }
  const auto it = capture_stream_preemptions_by_device_.find(stream->device_instance_id);
  return it != capture_stream_preemptions_by_device_.end() && !it->second.empty();
}

bool CoreRuntime::suppress_repeating_stream_frame_for_capture_(ProviderToCoreCommand&& cmd) {
  assert(core_thread_.is_core_thread());
  const ProviderFactSummary summary = summarize_provider_fact(cmd, capture_cohort_registry_);
  if (summary.fact_class != ProviderFactClass::RepeatingStreamFrame ||
      !is_stream_preempted_for_capture_(summary.stream_id)) {
    return false;
  }

  auto& frame = std::get<CmdProviderFrame>(cmd.payload).frame;
  const uint64_t integrated_ts_ns = frame_ts_to_core_ns_for_backpressure(frame.capture_timestamp);
  const bool received_counted = streams_.on_frame_received(frame.stream_id, integrated_ts_ns);
  const bool released_counted = streams_.on_frame_released(frame.stream_id);
  const bool dropped_counted = streams_.on_frame_dropped(frame.stream_id);
  frame.release_now();
  global_resource_aggregate_telemetry().lease_released(make_framebuffer_lease_scoped_resource_telemetry_key(
      frame.stream_id,
      frame.acquisition_session_id));
  frame.release = nullptr;
  frame.release_user = nullptr;

  if (received_counted || released_counted || dropped_counted) {
    request_publish_from_core_unchecked();
  }
  capture_latency_trace_printf(
      "stream_frame_suppressed_for_capture stream_id=%llu acquisition_session_id=%llu device_id=%llu received_counted=%u released_counted=%u dropped_counted=%u delivered=0 publication_requested=%u",
      static_cast<unsigned long long>(summary.stream_id),
      static_cast<unsigned long long>(summary.acquisition_session_id),
      static_cast<unsigned long long>(summary.device_instance_id),
      received_counted ? 1u : 0u,
      released_counted ? 1u : 0u,
      dropped_counted ? 1u : 0u,
      (received_counted || released_counted || dropped_counted) ? 1u : 0u);
  return true;
}

size_t CoreRuntime::suppress_queued_repeating_stream_frames_for_capture_() {
  assert(core_thread_.is_core_thread());
  size_t suppressed = 0;
  for (auto it = provider_facts_.begin(); it != provider_facts_.end();) {
    const ProviderFactSummary summary = summarize_provider_fact(*it, capture_cohort_registry_);
    if (summary.fact_class != ProviderFactClass::RepeatingStreamFrame ||
        !is_stream_preempted_for_capture_(summary.stream_id)) {
      ++it;
      continue;
    }
    ProviderToCoreCommand cmd = std::move(*it);
    it = provider_facts_.erase(it);
    if (suppress_repeating_stream_frame_for_capture_(std::move(cmd))) {
      ++suppressed;
    }
  }
  return suppressed;
}

std::vector<SharedCaptureResultData> CoreRuntime::get_capture_result_set(uint64_t capture_id) const {
  if (const auto* cohort = capture_cohort_registry_.find(capture_id); cohort != nullptr) {
    if (cohort->state == CoreCaptureCohortRegistry::CohortState::FAILED) {
      return {};
    }
    std::vector<SharedCaptureResultData> cohort_results;
    cohort_results.reserve(cohort->expected_participants.size());
    for (const auto& participant : cohort->expected_participants) {
      const uint64_t device_instance_id = participant.device_instance_id;
      if (!capture_assembly_registry_.is_assembly_successful(capture_id, device_instance_id)) {
        return {};
      }
      SharedCaptureResultData result = result_store_.get_capture_result(capture_id, device_instance_id);
      if (!result) {
        return {};
      }
      cohort_results.push_back(std::move(result));
    }
    return curate_capture_result_set_accept_all_assembly_successful_(std::move(cohort_results));
  }

  std::vector<SharedCaptureResultData> candidates = result_store_.get_capture_result_set(capture_id);
  std::vector<SharedCaptureResultData> assembly_successful;
  assembly_successful.reserve(candidates.size());
  for (auto& candidate : candidates) {
    if (!candidate) {
      continue;
    }
    if (!capture_assembly_registry_.is_assembly_successful(capture_id, candidate->device_instance_id)) {
      continue;
    }
    assembly_successful.push_back(std::move(candidate));
  }
  return curate_capture_result_set_accept_all_assembly_successful_(std::move(assembly_successful));
}

bool CoreRuntime::start() {
  // Idempotent start: already running is a success.
  const CoreRuntimeState st0 = state_.load(std::memory_order_acquire);
  if (st0 == CoreRuntimeState::LIVE || st0 == CoreRuntimeState::STARTING) {
    return true;
  }
  if (st0 == CoreRuntimeState::TEARING_DOWN) {
    return false;
  }

  // Attempt to transition CREATED/STOPPED -> STARTING.
  CoreRuntimeState expected = st0;
  while (expected == CoreRuntimeState::CREATED || expected == CoreRuntimeState::STOPPED) {
    if (state_.compare_exchange_weak(expected, CoreRuntimeState::STARTING,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire)) {
      break;
    }
  }
  if (state_.load(std::memory_order_acquire) != CoreRuntimeState::STARTING) {
    // Another thread moved us to LIVE/STARTING concurrently.
    const CoreRuntimeState st1 = state_.load(std::memory_order_acquire);
    return (st1 == CoreRuntimeState::LIVE || st1 == CoreRuntimeState::STARTING);
  }

  publish_pending_.store(false, std::memory_order_relaxed);
  publish_requests_coalesced_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_full_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_closed_.store(0, std::memory_order_relaxed);
  publish_requests_dropped_allocfail_.store(0, std::memory_order_relaxed);
  display_demand_release_async_dropped_full_.store(0, std::memory_order_relaxed);
  display_demand_release_async_dropped_closed_.store(0, std::memory_order_relaxed);
  display_demand_release_async_dropped_allocfail_.store(0, std::memory_order_relaxed);

  // Reset per-generation snapshot bookkeeping (schema v1).
  // gen is monotonic across the app/server lifetime and increments only when a new core loop
  // is successfully started.
  const uint64_t pending_gen = gen_counter_;
  current_gen_ = pending_gen;
  version_ = 0;
  topology_version_ = 0;
  last_topology_sig_ = 0;
  has_topology_sig_ = false;
  provider_banner_printed_ = false;

  // Reset core-thread-only pump state.
  rigs_.clear();
  capture_assembly_registry_.clear();
  capture_cohort_registry_.clear();
  capture_stream_preemptions_by_device_.clear();
  provider_facts_.clear();
  provider_capture_facts_queued_ = 0;
  requests_.clear();
  shutdown_requested_from_stop_.store(false, std::memory_order_release);
  shutdown_requested_ = false;
  shutdown_phase_ = ShutdownPhase::NONE;
  shutdown_phase_code_.store(0, std::memory_order_relaxed);
  shutdown_phase_changes_.store(0, std::memory_order_relaxed);
  shutdown_final_publish_requested_ = false;
  shutdown_wait_ticks_ = 0;

  const bool ok = core_thread_.start(this);
  if (ok) {
    ++gen_counter_;
  } else {
    // Failed to start core thread; revert to a sensible stable state.
    state_.store(CoreRuntimeState::STOPPED, std::memory_order_release);
  }
  return ok;
}

std::vector<SharedCaptureResultData> CoreRuntime::curate_capture_result_set_accept_all_assembly_successful_(
    std::vector<SharedCaptureResultData> candidates) const {
  // Placeholder curation seam: current policy accepts all assembly-successful
  // device captures. Future rig/cohort-aware policy can replace this step.
  return candidates;
}

void CoreRuntime::stop() {
  // Idempotent stop.
  const CoreRuntimeState st0 = state_.exchange(CoreRuntimeState::TEARING_DOWN, std::memory_order_acq_rel);
  if (st0 == CoreRuntimeState::STOPPED || st0 == CoreRuntimeState::CREATED) {
    state_.store(st0, std::memory_order_release);
    return;
  }

  // Deterministic shutdown request:
  // - Stop accepting new external requests immediately (state == TEARING_DOWN).
  // - Keep provider fact ingestion best-effort until the core thread closes.
  // - Core thread executes a deterministic shutdown pump and requests stop only at the end.
  //
  // This signal intentionally avoids the bounded best-effort CoreThread task queue:
  // attached-provider shutdown must not be skipped because the ordinary queue is
  // full or closed to new external work during stop.
  if (core_thread_.is_running()) {
    shutdown_requested_from_stop_.store(true, std::memory_order_release);
    core_thread_.request_timer_tick();
    core_thread_.join();
  }

  state_.store(CoreRuntimeState::STOPPED, std::memory_order_release);
}


void CoreRuntime::on_core_start() {
  // Core thread has started; begin accepting new work.
  capture_latency_trace_diagnostics::reset_trace_group_seen();
  epoch_ = std::chrono::steady_clock::now();
  // Do not carry retained result artifacts across generation boundaries.
  result_store_.clear();
  global_resource_aggregate_telemetry().clear();
  acquisition_sessions_.clear();
  spec_state_.reset_for_generation(0);
  state_.store(CoreRuntimeState::LIVE, std::memory_order_release);

  // Start "dirty": publish an initial baseline snapshot (version=0, topology_version=0)
  // via the normal coalesced publish path.
  publish_pending_.store(true, std::memory_order_release);
  core_thread_.request_timer_tick();
}

void CoreRuntime::on_core_timer_tick() {
  assert(core_thread_.is_core_thread());

  const auto now = std::chrono::steady_clock::now();
  const uint64_t now_ns = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_).count());

  // Banner 2: Core-loop provider attachment (effective runtime attachment).
  // Printed once per CoreRuntime session, the first time Core observes a non-null provider.
  if (!provider_banner_printed_ && banners_enabled()) {
    if (ICameraProvider* prov = provider_.load(std::memory_order_acquire)) {
#if defined(CAMBANG_INTERNAL_SMOKE)
      // Smoke can be stress-loop spammy: print only once per *process*.
      static std::atomic<bool> smoke_printed_process{false};
      if (smoke_printed_process.exchange(true)) {
        provider_banner_printed_ = true;
      } else {
        const ProviderBannerInfo bi = describe_provider_for_banner(prov);
        const int n = std::snprintf(core_banner_line_, sizeof(core_banner_line_),
                                    "[CamBANG][Core] provider attached: %s / %s",
                                    bi.provider_mode, bi.provider_name);
        (void)n;
        std::fprintf(stdout, "%s\n", core_banner_line_);
        std::fflush(stdout);
        core_banner_line_pending_.store(true, std::memory_order_release);
        provider_banner_printed_ = true;
      }
#else
      const ProviderBannerInfo bi = describe_provider_for_banner(prov);
      const int n = std::snprintf(core_banner_line_, sizeof(core_banner_line_),
                                  "[CamBANG][Core] provider attached: %s / %s",
                                  bi.provider_mode, bi.provider_name);
      (void)n;
      std::fprintf(stdout, "%s\n", core_banner_line_);
      std::fflush(stdout);
      core_banner_line_pending_.store(true, std::memory_order_release);
#endif
      provider_banner_printed_ = true;
    }
  }


  // 1) Drain provider facts ("what happened") first, but only for a
  // deterministic fairness slice. Conservative/non-lossy provider facts remain
  // FIFO and non-dropping; when backlog remains, the next requested tick
  // continues from the next queued fact after higher-priority command/capture
  // work has had a service opportunity. Stage B.1 honors capture-over-stream
  // priority inside this integration queue: capture facts may pass only a prefix
  // of lower-priority repeating stream frames, and repeating stream frames yield
  // to already-pending command/request work. Stage C coalesces stale repeating
  // stream frames only when a newer frame for the same stream/session is queued
  // before any non-lossy barrier.
  const bool requests_pending_before_provider_drain = !requests_.empty();
  bool command_or_request_waiting_for_stream_frame = false;
  {
    CoreRuntimeDiagnosticPhaseScope diagnostic_phase_scope(
        core_thread_, CoreThread::DiagnosticPhase::RuntimeProviderFactIntegration);
    const size_t provider_fact_drain_bound = requests_pending_before_provider_drain
        ? kMaxProviderFactsBeforeRequestWhenRequestsPending
        : kMaxProviderFactsPerCoreTurn;
    size_t provider_facts_drained = 0;
    while (!provider_facts_.empty() &&
           provider_facts_drained < provider_fact_drain_bound) {
      if (provider_capture_facts_queued_ != 0) {
        (void)promote_capture_fact_over_repeating_stream_prefix(provider_facts_, capture_cohort_registry_);
      }
      const ProviderFactSummary summary =
          summarize_provider_fact(provider_facts_.front(), capture_cohort_registry_);
      const bool command_or_request_pending = requests_pending_before_provider_drain ||
          core_thread_.has_pending_command_tasks();
      if (summary.fact_class == ProviderFactClass::RepeatingStreamFrame &&
          is_stream_preempted_for_capture_(summary.stream_id)) {
        ProviderToCoreCommand cmd = std::move(provider_facts_.front());
        provider_facts_.pop_front();
        (void)suppress_repeating_stream_frame_for_capture_(std::move(cmd));
        ++provider_facts_drained;
        continue;
      }
      if (summary.fact_class == ProviderFactClass::RepeatingStreamFrame) {
        const StreamFrameCoalesceResult coalesce_result =
            coalesce_front_repeating_stream_frame_if_superseded(
                provider_facts_, summary, capture_cohort_registry_, streams_);
        if (coalesce_result.coalesced) {
          request_publish_from_core_unchecked();
          ++provider_facts_drained;
          if (command_or_request_pending) {
            command_or_request_waiting_for_stream_frame = true;
            break;
          }
          continue;
        }
      }
      if (command_or_request_pending && summary.fact_class == ProviderFactClass::RepeatingStreamFrame) {
        command_or_request_waiting_for_stream_frame = true;
        capture_latency_trace_printf(
            "provider_fact_deferred_for_command class=%s stream_id=%llu acquisition_session_id=%llu provider_fact_depth=%llu requests_pending=%u command_lane_pending=%u",
            provider_fact_class_name(summary.fact_class),
            static_cast<unsigned long long>(summary.stream_id),
            static_cast<unsigned long long>(summary.acquisition_session_id),
            static_cast<unsigned long long>(provider_facts_.size()),
            requests_pending_before_provider_drain ? 1u : 0u,
            core_thread_.has_pending_command_tasks() ? 1u : 0u);
        break;
      }

      ProviderToCoreCommand cmd = std::move(provider_facts_.front());
      provider_facts_.pop_front();
      if (provider_fact_is_capture_critical(summary.fact_class) && provider_capture_facts_queued_ != 0) {
        --provider_capture_facts_queued_;
      }
      const uint64_t dispatch_begin_ns = capture_latency_trace_now_ns();
      dispatcher_.dispatch(std::move(cmd));
      const uint64_t dispatch_us = (capture_latency_trace_now_ns() - dispatch_begin_ns) / 1000ull;
      emit_provider_fact_dispatch_slow_if_needed(summary, dispatch_us, provider_facts_.size());
      if (provider_fact_is_capture_critical(summary.fact_class)) {
        release_result_safe_capture_stream_preemptions_();
      }
      ++provider_facts_drained;

      if (!requests_pending_before_provider_drain && core_thread_.has_pending_command_tasks()) {
        command_or_request_waiting_for_stream_frame = true;
        break;
      }
    }
  }
  const bool provider_facts_remain_after_fairness_slice = !provider_facts_.empty();

  // If provider facts mutated relevant state, request a coalesced publish.
  if (dispatcher_.consume_relevant_state_changed()) {
    request_publish_from_core_unchecked();
  }

  // If command-lane work arrived while this timer tick was integrating provider
  // facts, return to CoreThread promptly so the command lane can enqueue into
  // requests_. The pending publish/timer state remains retained and this tick is
  // re-requested for deterministic continuation.
  if (command_or_request_waiting_for_stream_frame && requests_.empty() &&
      !shutdown_requested_ && !shutdown_requested_from_stop_.load(std::memory_order_acquire)) {
    core_thread_.request_timer_tick();
    return;
  }

  // 2) Drain queued requests ("what should we do").
  {
    CoreRuntimeDiagnosticPhaseScope diagnostic_phase_scope(
        core_thread_, CoreThread::DiagnosticPhase::RuntimeRequestDrain);
    while (!requests_.empty()) {
      auto task = std::move(requests_.front());
      requests_.pop_front();
      if (task) {
        task();
      }
    }
  }

  if (provider_facts_remain_after_fairness_slice) {
    core_thread_.request_timer_tick();
  }

  // 3) Retention / timers.
  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  {
    CoreRuntimeDiagnosticPhaseScope diagnostic_phase_scope(
        core_thread_, CoreThread::DiagnosticPhase::RuntimeRetentionTimerWork);

    uint64_t next_warm_delay_ns = 0;
    bool has_next_warm_delay = false;
    for (const auto& [device_id, rec] : devices_.all()) {
      (void)device_id;
      if (!rec.open) {
        continue;
      }

      const bool active_use = streams_.has_flowing_stream_for_device(rec.device_instance_id);

      if (active_use) {
        (void)devices_.set_warm_was_in_use(rec.device_instance_id, true);
        if (rec.warm_deadline_active || rec.warm_expired_close_requested) {
          (void)devices_.clear_warm_deadline(rec.device_instance_id);
          request_publish_from_core_unchecked();
        }
        continue;
      }

      const bool became_not_in_use = rec.warm_was_in_use;
      if (became_not_in_use) {
        (void)devices_.set_warm_was_in_use(rec.device_instance_id, false);
      }

      if (rec.warm_hold_ms == 0) {
        if (rec.warm_deadline_active || rec.warm_expired_close_requested) {
          (void)devices_.clear_warm_deadline(rec.device_instance_id);
          request_publish_from_core_unchecked();
        }
        continue;
      }

      if (!rec.warm_deadline_active) {
        const uint64_t hold_ns = warm_delay_ns(rec.warm_hold_ms);
        const uint64_t deadline_ns = (hold_ns > (std::numeric_limits<uint64_t>::max() - now_ns))
            ? std::numeric_limits<uint64_t>::max()
            : (now_ns + hold_ns);
        if (devices_.arm_warm_deadline(rec.device_instance_id, deadline_ns)) {
          request_publish_from_core_unchecked();
        }
        continue;
      }

      if (rec.warm_deadline_active && now_ns >= rec.warm_deadline_ns) {
        if (!rec.warm_expired_close_requested && prov) {
          (void)devices_.mark_warm_expired_close_requested(rec.device_instance_id, true);
          (void)prov->close_device(rec.device_instance_id);
          request_publish_from_core_unchecked();
        }
        continue;
      }

      const uint64_t remaining_ns = rec.warm_deadline_ns - now_ns;
      if (!has_next_warm_delay || remaining_ns < next_warm_delay_ns) {
        has_next_warm_delay = true;
        next_warm_delay_ns = remaining_ns;
      }
    }

    const size_t retired_count =
        native_objects_.retire_destroyed_older_than(now_ns, kDestroyedNativeObjectRetentionWindowNs);
    global_resource_aggregate_telemetry().reconcile_lifecycle(
        now_ns,
        current_gen_,
        &streams_,
        &acquisition_sessions_,
        &devices_,
        &native_objects_);
    const size_t retired_telemetry_count =
        global_resource_aggregate_telemetry().retire_destroyed_older_than(now_ns, kDestroyedNativeObjectRetentionWindowNs);
    if (retired_count > 0 || retired_telemetry_count > 0) {
      request_publish_from_core_unchecked();
    }

    uint64_t next_deadline_delay_ns = 0;
    bool has_next_deadline_delay = false;
    if (const auto next_retirement_delay_ns =
            native_objects_.next_retirement_delay_ns(now_ns, kDestroyedNativeObjectRetentionWindowNs);
        next_retirement_delay_ns.has_value()) {
      has_next_deadline_delay = true;
      next_deadline_delay_ns = *next_retirement_delay_ns;
    }
    if (const auto next_telemetry_retirement_delay_ns =
            global_resource_aggregate_telemetry().next_retirement_delay_ns(now_ns, kDestroyedNativeObjectRetentionWindowNs);
        next_telemetry_retirement_delay_ns.has_value()) {
      if (!has_next_deadline_delay || *next_telemetry_retirement_delay_ns < next_deadline_delay_ns) {
        has_next_deadline_delay = true;
        next_deadline_delay_ns = *next_telemetry_retirement_delay_ns;
      }
    }
    if (has_next_warm_delay && (!has_next_deadline_delay || next_warm_delay_ns < next_deadline_delay_ns)) {
      has_next_deadline_delay = true;
      next_deadline_delay_ns = next_warm_delay_ns;
    }

    if (has_next_deadline_delay) {
      core_thread_.set_timer_deadline_ns(next_deadline_delay_ns);
    } else {
      core_thread_.clear_timer_deadline();
    }

  }

  // 4) Snapshot publish (coalesced).
  if (publish_pending_.load(std::memory_order_acquire)) {
    CoreRuntimeDiagnosticPhaseScope diagnostic_phase_scope(core_thread_, CoreThread::DiagnosticPhase::RuntimeSnapshotPublication);
    // Clear pending first so a new request can enqueue even if publish work is heavy.
    publish_pending_.store(false, std::memory_order_release);

    SnapshotBuilder::Inputs in;
    in.rigs = &rigs_;
    in.devices = &devices_;
    in.acquisition_sessions = &acquisition_sessions_;
    in.streams = &streams_;
    in.ingress = &ingress_;
    in.native_objects = &native_objects_;
    in.spec_state = &spec_state_;
    in.scoped_resource_telemetry = &global_resource_aggregate_telemetry();

    const uint64_t topo_sig = snapshot_builder_.compute_topology_signature(in);

    // Publish-side topology signature for boundary diffing (Godot-facing).
    // This is updated on every successful snapshot build/publish.
    published_topology_sig_.store(topo_sig, std::memory_order_release);

    // topology_version is zero-indexed within each gen.
    // The first published snapshot establishes the baseline topology_version=0.
    if (!has_topology_sig_) {
      has_topology_sig_ = true;
      last_topology_sig_ = topo_sig;
    } else if (topo_sig != last_topology_sig_) {
      last_topology_sig_ = topo_sig;
      ++topology_version_;
    }

    const uint64_t gen_out = current_gen_;
    const uint64_t ver_out = version_;
    const uint64_t topo_out = topology_version_;
    const uint64_t timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - epoch_).count());

    CamBANGStateSnapshot snap = snapshot_builder_.build(in, gen_out, ver_out, topo_out, timestamp_ns);
    auto shared = std::make_shared<CamBANGStateSnapshot>(std::move(snap));

    // Advance per-generation publish counter only after snapshot assembly succeeds.
    ++version_;

    if (IStateSnapshotPublisher* pub = snapshot_publisher_.load(std::memory_order_acquire)) {
      pub->publish(std::move(shared));
    }

    // published_seq_ must not become visible before the corresponding snapshot
    // is visible to boundary consumers.
    published_seq_.fetch_add(1, std::memory_order_acq_rel);
  }

  if (shutdown_requested_from_stop_.exchange(false, std::memory_order_acq_rel)) {
    shutdown_requested_ = true;
  }

  // 5) Shutdown choreography (§10).
  if (shutdown_requested_) {
    CoreRuntimeDiagnosticPhaseScope diagnostic_phase_scope(core_thread_, CoreThread::DiagnosticPhase::RuntimeShutdownChoreography);
    auto set_phase = [this](ShutdownPhase p) {
      if (shutdown_phase_ != p) {
        shutdown_phase_ = p;
        shutdown_phase_code_.store(static_cast<uint8_t>(p), std::memory_order_relaxed);
        shutdown_phase_changes_.fetch_add(1, std::memory_order_relaxed);
      }
    };

    if (shutdown_phase_ == ShutdownPhase::NONE) {
      set_phase(ShutdownPhase::STOP_STREAMS);
      shutdown_wait_ticks_ = 0;
    }

    constexpr uint32_t kMaxShutdownWaitTicks = 200; // deterministic bound; avoids teardown hangs.

    auto any_stream_started = [this]() -> bool {
      for (const auto& kv : streams_.all()) {
        if (kv.second.started) {
          return true;
        }
      }
      return false;
    };

    auto any_streams_exist = [this]() -> bool {
      return !streams_.all().empty();
    };

    auto any_device_open = [this]() -> bool {
      for (const auto& kv : devices_.all()) {
        if (kv.second.open) {
          return true;
        }
      }
      return false;
    };

    switch (shutdown_phase_) {
      case ShutdownPhase::STOP_STREAMS: {
        // Step 3: stop streams (deterministic order).  Public try_destroy_stream()
        // remains strict, but shutdown is host-owned cleanup: when the provider
        // stop call returns OK the provider has synchronously ceased production, so
        // reflect that truth locally as well as accepting the later provider fact.
        if (prov) {
          std::vector<uint64_t> started_stream_ids;
          started_stream_ids.reserve(streams_.all().size());
          for (const auto& kv : streams_.all()) {
            const auto& rec = kv.second;
            if (rec.started) {
              started_stream_ids.push_back(rec.stream_id);
            }
          }
          for (const uint64_t stream_id : started_stream_ids) {
            (void)streams_.mark_stop_requested_by_core(stream_id);
            const ProviderResult sr = prov->stop_stream(stream_id);
            if (sr.ok()) {
              (void)streams_.on_core_stream_stopped(stream_id, /*error_code=*/0);
            }
          }
        }
        set_phase(ShutdownPhase::AWAIT_STREAMS_STOPPED);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::AWAIT_STREAMS_STOPPED: {
        // Best-effort drain: wait for provider facts to reflect stopped streams.
        if (any_stream_started() && shutdown_wait_ticks_++ < kMaxShutdownWaitTicks) {
          core_thread_.request_timer_tick();
          return;
        }
        set_phase(ShutdownPhase::DESTROY_STREAMS);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::DESTROY_STREAMS: {
        // Step 4 (part): tear down stream instances (destroy) before closing devices.
        // Provider destroy_stream() is still strict for ordinary callers; this phase
        // only runs after shutdown stop convergence.  On OK, provider storage has
        // been structurally destroyed, so make registry absence immediately
        // observable instead of depending on a provider-strand round trip.
        if (prov) {
          std::vector<uint64_t> created_stream_ids;
          created_stream_ids.reserve(streams_.all().size());
          for (const auto& kv : streams_.all()) {
            const auto& rec = kv.second;
            if (rec.created) {
              created_stream_ids.push_back(rec.stream_id);
            }
          }
          for (const uint64_t stream_id : created_stream_ids) {
            const ProviderResult dr = prov->destroy_stream(stream_id);
            if (dr.ok()) {
              (void)streams_.on_stream_destroyed(stream_id);
              result_store_.remove_stream_result(stream_id);
            }
          }
        }
        set_phase(ShutdownPhase::AWAIT_STREAMS_DESTROYED);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::AWAIT_STREAMS_DESTROYED: {
        // Best-effort: wait until provider confirms destruction.
        if (any_streams_exist() && shutdown_wait_ticks_++ < kMaxShutdownWaitTicks) {
          core_thread_.request_timer_tick();
          return;
        }
        set_phase(ShutdownPhase::CLOSE_DEVICES);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::CLOSE_DEVICES: {
        if (prov) {
          for (const auto& kv : devices_.all()) {
            const auto& rec = kv.second;
            if (rec.open) {
              (void)prov->close_device(rec.device_instance_id);
            }
          }
        }
        set_phase(ShutdownPhase::AWAIT_DEVICES_CLOSED);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::AWAIT_DEVICES_CLOSED: {
        if (any_device_open() && shutdown_wait_ticks_++ < kMaxShutdownWaitTicks) {
          core_thread_.request_timer_tick();
          return;
        }
        set_phase(ShutdownPhase::PROVIDER_SHUTDOWN);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::PROVIDER_SHUTDOWN: {
        // Step 5: request provider shutdown (idempotent).
        if (prov) {
          (void)prov->shutdown();
        }
        set_phase(ShutdownPhase::FINAL_RETENTION_SWEEP);
        shutdown_wait_ticks_ = 0;
        core_thread_.request_timer_tick();
        return;
      }

      case ShutdownPhase::FINAL_RETENTION_SWEEP: {
        (void)native_objects_.retire_destroyed_older_than(now_ns, kDestroyedNativeObjectRetentionWindowNs);
        global_resource_aggregate_telemetry().reconcile_lifecycle(
            now_ns,
            current_gen_,
            &streams_,
            &acquisition_sessions_,
            &devices_,
            &native_objects_);
        (void)global_resource_aggregate_telemetry().retire_destroyed_older_than(now_ns, kDestroyedNativeObjectRetentionWindowNs);
        set_phase(ShutdownPhase::FINAL_PUBLISH);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::FINAL_PUBLISH: {
        // Step 7: final snapshot publication.
        if (!shutdown_final_publish_requested_) {
          request_publish_from_core_unchecked();
          shutdown_final_publish_requested_ = true;
          core_thread_.request_timer_tick();
          return;
        }

        set_phase(ShutdownPhase::CLEAR_DESTROYED_RETAINED_NATIVE_OBJECTS);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::CLEAR_DESTROYED_RETAINED_NATIVE_OBJECTS: {
        // Do not carry retained DESTROYED records across stop/start boundaries.
        // They remain truthfully retained while the generation is live and through
        // final prior-generation publication, then are quarantined before exit.
        (void)native_objects_.clear_destroyed();
        global_resource_aggregate_telemetry().clear();
        set_phase(ShutdownPhase::EXIT);
        shutdown_wait_ticks_ = 0;
        // fallthrough
      }

      case ShutdownPhase::EXIT: {
        // Step 8: exit when fully drained and publish completed.
        const bool publish_still_pending = publish_pending_.load(std::memory_order_acquire);
        if ((provider_facts_.empty() && requests_.empty() && !publish_still_pending) ||
            (shutdown_wait_ticks_++ >= kMaxShutdownWaitTicks)) {
          core_thread_.request_stop_from_core();
        } else {
          core_thread_.request_timer_tick();
        }
        return;
      }

      default:
        break;
    }
  }
}

void CoreRuntime::on_core_stop() {
  capture_latency_trace_diagnostics::print_trace_group_seen_summary();
  // Runtime is no longer live; clear retained results so stop/start boundaries
  // cannot expose stale prior-generation result truth.
  result_store_.clear();
  global_resource_aggregate_telemetry().clear();
  // Core thread is exiting. Ensure external gating sees STOPPED promptly.
  state_.store(CoreRuntimeState::STOPPED, std::memory_order_release);
}

void CoreRuntime::post(CoreThread::Task task) {
  // Best-effort compatibility shim for disposable work only; retained runtime
  // truth must call an admission-returning path and handle the result.
  (void)try_post(std::move(task));
}

CoreThread::PostResult CoreRuntime::retain_device_identity(uint64_t device_instance_id, const std::string& hardware_id) {
  if (device_instance_id == 0 || hardware_id.empty()) {
    return CoreThread::PostResult::Closed;
  }

  CaptureTemplate capture_tmpl{};
  bool has_capture_template = false;
  if (ICameraProvider* p = provider_.load(std::memory_order_acquire)) {
    capture_tmpl = p->capture_template();
    has_capture_template = true;
  }

  return try_post([this, device_instance_id, hardware_id, capture_tmpl, has_capture_template]() {
    if (!devices_.note_device_identity(device_instance_id, hardware_id)) {
      return;
    }
    devices_.set_camera_spec_version(
        device_instance_id,
        spec_state_.camera_spec_version(hardware_id));
    if (has_capture_template) {
      (void)seed_retained_device_still_profile_from_template(
          devices_, device_instance_id, capture_tmpl);
    }
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::retain_camera_spec_version(const std::string& hardware_id, uint64_t camera_spec_version) {
  if (hardware_id.empty()) {
    return CoreThread::PostResult::Closed;
  }

  return try_post([this, hardware_id, camera_spec_version]() {
    spec_state_.set_camera_spec_version(hardware_id, camera_spec_version);
    for (const auto& [device_instance_id, rec] : devices_.all()) {
      if (rec.hardware_id == hardware_id) {
        (void)devices_.set_camera_spec_version(device_instance_id, camera_spec_version);
      }
    }
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::retain_device_capture_profile(uint64_t device_instance_id,
                                                                  uint32_t width,
                                                                  uint32_t height,
                                                                  uint32_t format,
                                                                  uint64_t capture_profile_version) {
  if (device_instance_id == 0) {
    return CoreThread::PostResult::Closed;
  }

  return try_post([this, device_instance_id, width, height, format, capture_profile_version]() {
    (void)devices_.retain_capture_profile(
        device_instance_id, width, height, format, capture_profile_version);
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::retain_rig_capture_profile(uint64_t rig_id,
                                                               uint32_t width,
                                                               uint32_t height,
                                                               uint32_t format,
                                                               uint64_t capture_profile_version) {
  if (rig_id == 0) {
    return CoreThread::PostResult::Closed;
  }

  return try_post([this, rig_id, width, height, format, capture_profile_version]() {
    (void)rigs_.retain_capture_profile(rig_id, width, height, format, capture_profile_version);
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::retain_imaging_spec_version(uint64_t imaging_spec_version) {
  return try_post([this, imaging_spec_version]() {
    spec_state_.set_imaging_spec_version(imaging_spec_version);
    request_publish_from_core_unchecked();
  });
}

CoreThread::PostResult CoreRuntime::try_post(CoreThread::Task task) {
  const CoreRuntimeState st = state_.load(std::memory_order_acquire);
  if (st != CoreRuntimeState::LIVE) {
    return core_thread_.reject_closed();
  }

  // Marshal Core-owned commands into the command lane. The CoreRuntime pump still
  // enforces facts-before-requests ordering inside on_core_timer_tick(), but the
  // command lane prevents public/request work from sitting behind ordinary
  // provider-frame ingress tasks before it can enter requests_.
  return core_thread_.try_post_command([this, t = std::move(task)]() mutable {
    assert(core_thread_.is_core_thread());
    enqueue_request(std::move(t));
  });
}

TryCreateStreamStatus CoreRuntime::try_create_stream(
    uint64_t stream_id,
    uint64_t device_instance_id,
    StreamIntent intent,
    const CaptureProfile* request_profile,
    const PictureConfig* request_picture,
    uint64_t profile_version) noexcept {
  if (stream_id == 0 || device_instance_id == 0) {
    return TryCreateStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryCreateStreamStatus::Busy;
  }

  // Compute effective config (core owns defaulting).
  const StreamTemplate tmpl = prov->stream_template();
  const bool has_request_profile = (request_profile != nullptr);
  const bool has_request_picture = (request_picture != nullptr);
  const CaptureProfile request_profile_copy = has_request_profile ? *request_profile : CaptureProfile{};
  const PictureConfig request_picture_copy = has_request_picture ? *request_picture : PictureConfig{};

  const CoreThread::PostResult pr = try_post([this,
                                              stream_id,
                                              device_instance_id,
                                              intent,
                                              profile_version,
                                              tmpl,
                                              has_request_profile,
                                              request_profile_copy,
                                              has_request_picture,
                                              request_picture_copy]() {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) return;

    const uint64_t effective_profile_version =
        (profile_version != 0)
            ? profile_version
            : create_stream_profile_version_seq_.fetch_add(1, std::memory_order_relaxed);

    StreamRequest effective{};
    effective.stream_id = stream_id;
    effective.device_instance_id = device_instance_id;
    effective.intent = intent;
    effective.profile_version = effective_profile_version;
    effective.profile = has_request_profile ? request_profile_copy : tmpl.profile;
    effective.picture = has_request_picture ? request_picture_copy : tmpl.picture;

    // Declare before calling into the provider so any synchronous callbacks
    // can resolve the record deterministically.
    (void)streams_.declare_stream_effective(effective);

    const ProviderResult r = p->create_stream(effective);
    if (!r.ok()) {
      // Best-effort rollback; create_stream failure must not leave a ghost record.
      (void)streams_.forget_stream(effective.stream_id);
    }
  });

  return (pr == CoreThread::PostResult::Enqueued) ? TryCreateStreamStatus::OK
                                                  : TryCreateStreamStatus::Busy;
}

TryStartStreamStatus CoreRuntime::try_start_stream(uint64_t stream_id) noexcept {
  if (stream_id == 0) {
    return TryStartStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryStartStreamStatus::Busy;
  }

  auto result_promise = std::make_shared<std::promise<TryStartStreamStatus>>();
  std::future<TryStartStreamStatus> f = result_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, stream_id, result_promise]() mutable {
    ICameraProvider* prov_local = provider_.load(std::memory_order_acquire);
    if (!prov_local) {
      result_promise->set_value(TryStartStreamStatus::Busy);
      return;
    }

    const CoreStreamRegistry::StreamRecord* rec = streams_.find(stream_id);
    if (!rec) {
      result_promise->set_value(TryStartStreamStatus::InvalidArgument);
      return;
    }
    if (rec->started) {
      result_promise->set_value(TryStartStreamStatus::OK);
      return;
    }
    const uint64_t owner_device_instance_id = rec->device_instance_id;
    for (const auto& kv : streams_.all()) {
      const auto& other = kv.second;
      if (other.stream_id == stream_id) {
        continue;
      }
      if (other.device_instance_id == owner_device_instance_id && other.created && other.started) {
        result_promise->set_value(TryStartStreamStatus::Busy);
        return;
      }
    }

    const ProviderResult sr = prov_local->start_stream(stream_id, rec->profile, rec->picture);
    if (!sr.ok()) {
      timeline_teardown_trace_emit("fail StartStream stream_id=%llu reason=provider_rc_%u",
                                   static_cast<unsigned long long>(stream_id),
                                   static_cast<unsigned>(sr.code));
      (void)streams_.on_stream_error(stream_id, static_cast<uint32_t>(sr.code));
      result_promise->set_value(TryStartStreamStatus::ProviderRejected);
      return;
    }
    (void)streams_.on_core_stream_started(stream_id);
    result_promise->set_value(TryStartStreamStatus::OK);
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return TryStartStreamStatus::Busy;
  }
  return f.get();
}

TryStopStreamStatus CoreRuntime::try_stop_stream(uint64_t stream_id) noexcept {
  if (stream_id == 0) {
    return TryStopStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryStopStreamStatus::Busy;
  }

  auto result_promise = std::make_shared<std::promise<TryStopStreamStatus>>();
  std::future<TryStopStreamStatus> f = result_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, stream_id, result_promise]() mutable {
    ICameraProvider* prov_local = provider_.load(std::memory_order_acquire);
    if (!prov_local) {
      result_promise->set_value(TryStopStreamStatus::Busy);
      return;
    }
    const CoreStreamRegistry::StreamRecord* rec = streams_.find(stream_id);
    if (!rec) {
      result_promise->set_value(TryStopStreamStatus::InvalidArgument);
      return;
    }
    if (!rec->started) {
      result_promise->set_value(TryStopStreamStatus::OK);
      return;
    }
    (void)streams_.mark_stop_requested_by_core(stream_id);
    const ProviderResult sr = prov_local->stop_stream(stream_id);
    if (!sr.ok()) {
      timeline_teardown_trace_emit("fail StopStream stream_id=%llu reason=provider_rc_%u",
                                   static_cast<unsigned long long>(stream_id),
                                   static_cast<unsigned>(sr.code));
      result_promise->set_value(TryStopStreamStatus::ProviderRejected);
      return;
    }
    (void)streams_.on_core_stream_stopped(stream_id, /*error_code=*/0);
    result_promise->set_value(TryStopStreamStatus::OK);
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return TryStopStreamStatus::Busy;
  }
  return f.get();
}

TryDestroyStreamStatus CoreRuntime::try_destroy_stream(uint64_t stream_id) noexcept {
  if (stream_id == 0) {
    return TryDestroyStreamStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryDestroyStreamStatus::Busy;
  }

  auto result_promise = std::make_shared<std::promise<TryDestroyStreamStatus>>();
  std::future<TryDestroyStreamStatus> f = result_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, stream_id, result_promise]() mutable {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) {
      result_promise->set_value(TryDestroyStreamStatus::Busy);
      return;
    }

    const CoreStreamRegistry::StreamRecord* rec = streams_.find(stream_id);
    if (!rec) {
      result_promise->set_value(TryDestroyStreamStatus::InvalidArgument);
      return;
    }
    if (rec->started) {
      timeline_teardown_trace_emit("fail DestroyStream stream_id=%llu reason=stream_started",
                                   static_cast<unsigned long long>(stream_id));
      result_promise->set_value(TryDestroyStreamStatus::Started);
      return;
    }

    const ProviderResult dr = p->destroy_stream(stream_id);
    if (!dr.ok()) {
      timeline_teardown_trace_emit("fail DestroyStream stream_id=%llu reason=provider_rc_%u",
                                   static_cast<unsigned long long>(stream_id),
                                   static_cast<unsigned>(dr.code));
      result_promise->set_value(TryDestroyStreamStatus::ProviderRejected);
      return;
    }
    result_promise->set_value(TryDestroyStreamStatus::OK);
  });

  if (pr != CoreThread::PostResult::Enqueued) {
    return TryDestroyStreamStatus::Busy;
  }
  return f.get();
}

TryOpenDeviceStatus CoreRuntime::try_open_device(
    const std::string& hardware_id,
    uint64_t device_instance_id,
    uint64_t root_id) noexcept {
  if (hardware_id.empty() || device_instance_id == 0 || root_id == 0) {
    return TryOpenDeviceStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryOpenDeviceStatus::Busy;
  }

  const CaptureTemplate capture_tmpl = prov->capture_template();
  auto result_promise = std::make_shared<std::promise<TryOpenDeviceStatus>>();
  std::future<TryOpenDeviceStatus> f = result_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, hardware_id, device_instance_id, root_id, capture_tmpl, result_promise]() mutable {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) {
      result_promise->set_value(TryOpenDeviceStatus::Busy);
      return;
    }

    const ProviderResult open_result = p->open_device(hardware_id, device_instance_id, root_id);
    if (!open_result.ok()) {
      timeline_teardown_trace_emit("fail OpenDevice device_instance_id=%llu reason=provider_rc_%u",
                                   static_cast<unsigned long long>(device_instance_id),
                                   static_cast<unsigned>(open_result.code));
      result_promise->set_value(TryOpenDeviceStatus::ProviderRejected);
      return;
    }

    // Retain core-owned device identity/profile truth only after provider open
    // submission was accepted. A provider-refused open must not publish a
    // speculative CREATED device record.
    (void)devices_.note_device_identity(device_instance_id, hardware_id);
    (void)seed_retained_device_still_profile_from_template(devices_, device_instance_id, capture_tmpl);
    (void)devices_.set_capture_picture(device_instance_id, capture_tmpl.picture);
    result_promise->set_value(TryOpenDeviceStatus::OK);
  });

  if (pr != CoreThread::PostResult::Enqueued) {
    return TryOpenDeviceStatus::Busy;
  }
  return f.get();
}

TryCloseDeviceStatus CoreRuntime::try_close_device(uint64_t device_instance_id) noexcept {
  if (device_instance_id == 0) {
    return TryCloseDeviceStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryCloseDeviceStatus::Busy;
  }

  auto result_promise = std::make_shared<std::promise<TryCloseDeviceStatus>>();
  std::future<TryCloseDeviceStatus> f = result_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, device_instance_id, result_promise]() mutable {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) {
      result_promise->set_value(TryCloseDeviceStatus::Busy);
      return;
    }
    const ProviderResult cr = p->close_device(device_instance_id);
    if (!cr.ok()) {
      timeline_teardown_trace_emit("fail CloseDevice device_instance_id=%llu reason=provider_rc_%u",
                                   static_cast<unsigned long long>(device_instance_id),
                                   static_cast<unsigned>(cr.code));
      result_promise->set_value(TryCloseDeviceStatus::ProviderRejected);
      return;
    }
    result_promise->set_value(TryCloseDeviceStatus::OK);
  });

  if (pr != CoreThread::PostResult::Enqueued) {
    return TryCloseDeviceStatus::Busy;
  }
  return f.get();
}

TrySetStreamPictureStatus CoreRuntime::try_set_stream_picture_config(
    uint64_t stream_id,
    const PictureConfig& picture) noexcept {
  if (stream_id == 0) {
    return TrySetStreamPictureStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TrySetStreamPictureStatus::Busy;
  }
  if (!prov->supports_stream_picture_updates()) {
    return TrySetStreamPictureStatus::NotSupported;
  }

  const CoreThread::PostResult pr = try_post([this, stream_id, picture]() {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) return;
    const ProviderResult sr = p->set_stream_picture_config(stream_id, picture);
    if (!sr.ok()) {
      return;
    }
    if (streams_.set_picture(stream_id, picture)) {
      // Picture config is snapshot-visible stream state. Route updates through
      // the normal core coalesced publication path.
      request_publish_from_core_unchecked();
    }
  });

  return (pr == CoreThread::PostResult::Enqueued) ? TrySetStreamPictureStatus::OK
                                                  : TrySetStreamPictureStatus::Busy;
}

TrySetCapturePictureStatus CoreRuntime::try_set_capture_picture_config(
    uint64_t device_instance_id,
    const PictureConfig& picture) noexcept {
  if (device_instance_id == 0) {
    return TrySetCapturePictureStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TrySetCapturePictureStatus::Busy;
  }
  if (!prov->supports_capture_picture_updates()) {
    return TrySetCapturePictureStatus::NotSupported;
  }

  const CoreThread::PostResult pr = try_post([this, device_instance_id, picture]() {
    ICameraProvider* p = provider_.load(std::memory_order_acquire);
    if (!p) return;
    const ProviderResult sr = p->set_capture_picture_config(device_instance_id, picture);
    if (!sr.ok()) {
      return;
    }
    if (devices_.set_capture_picture(device_instance_id, picture)) {
      request_publish_from_core_unchecked();
    }
  });

  return (pr == CoreThread::PostResult::Enqueued) ? TrySetCapturePictureStatus::OK
                                                  : TrySetCapturePictureStatus::Busy;
}

TrySetStillCaptureProfileStatus CoreRuntime::try_set_device_still_capture_profile(
    uint64_t device_instance_id,
    const CaptureProfile& profile,
    const CaptureStillImageBundle& still_image_bundle) noexcept {
  if (device_instance_id == 0 || profile.width == 0 || profile.height == 0 || profile.format_fourcc == 0) {
    return TrySetStillCaptureProfileStatus::InvalidArgument;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TrySetStillCaptureProfileStatus::Busy;
  }
  const bool supports_multi_image = prov->supports_multi_image_still_sequence();
  if (!is_valid_capture_still_image_bundle(still_image_bundle, supports_multi_image)) {
    return supports_multi_image
        ? TrySetStillCaptureProfileStatus::InvalidArgument
        : TrySetStillCaptureProfileStatus::NotSupported;
  }

  const CoreThread::PostResult pr = try_post([this, device_instance_id, profile, still_image_bundle]() {
    uint64_t next_version = 1;
    if (const auto* rec = devices_.find(device_instance_id)) {
      bool same_sequence = (rec->capture_still_image_bundle.members.size() == still_image_bundle.members.size());
      if (same_sequence) {
        for (size_t i = 0; i < rec->capture_still_image_bundle.members.size(); ++i) {
          const auto& a = rec->capture_still_image_bundle.members[i];
          const auto& b = still_image_bundle.members[i];
          if (a.image_member_index != b.image_member_index ||
              a.role != b.role ||
              a.intended_exposure_compensation_milli_ev != b.intended_exposure_compensation_milli_ev) {
            same_sequence = false;
            break;
          }
        }
      }
      const bool unchanged =
          rec->capture_width == profile.width &&
          rec->capture_height == profile.height &&
          rec->capture_format == profile.format_fourcc &&
          same_sequence;
      if (unchanged) {
        return;
      }
      next_version = rec->capture_profile_version + 1;
      if (next_version == 0) next_version = 1;
    }
    (void)devices_.retain_capture_profile(
        device_instance_id,
        profile.width,
        profile.height,
        profile.format_fourcc,
        next_version);
    (void)devices_.set_capture_still_image_bundle(device_instance_id, still_image_bundle, next_version);
    request_publish_from_core_unchecked();
  });

  return (pr == CoreThread::PostResult::Enqueued)
      ? TrySetStillCaptureProfileStatus::OK
      : TrySetStillCaptureProfileStatus::Busy;
}

TrySetWarmHoldStatus CoreRuntime::try_set_device_warm_hold_ms(
    uint64_t device_instance_id,
    uint32_t warm_hold_ms) noexcept {
  if (device_instance_id == 0) {
    return TrySetWarmHoldStatus::InvalidArgument;
  }
  auto status_promise = std::make_shared<std::promise<TrySetWarmHoldStatus>>();
  std::future<TrySetWarmHoldStatus> status_future = status_promise->get_future();
  const CoreThread::PostResult pr = try_post([this, device_instance_id, warm_hold_ms, status_promise]() {
    const CoreDeviceRegistry::DeviceRecord* rec = devices_.find(device_instance_id);
    if (rec == nullptr || !rec->open) {
      status_promise->set_value(TrySetWarmHoldStatus::Busy);
      return;
    }
    (void)devices_.set_warm_hold_ms(device_instance_id, warm_hold_ms);
    request_publish_from_core_unchecked();
    status_promise->set_value(TrySetWarmHoldStatus::OK);
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return TrySetWarmHoldStatus::Busy;
  }
  return status_future.get();
}

bool CoreRuntime::materialize_capture_request_for_server(uint64_t device_instance_id, CaptureRequest& out) const {
  out = CaptureRequest{};
  if (device_instance_id == 0) {
    return false;
  }

  if (core_thread_.is_core_thread()) {
    return materialize_capture_request_(device_instance_id, out);
  }

  auto completion = std::make_shared<std::promise<std::pair<bool, CaptureRequest>>>();
  std::future<std::pair<bool, CaptureRequest>> completed = completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  const CoreThread::PostResult pr = self->try_post([this, device_instance_id, completion]() {
    CaptureRequest request{};
    const bool ok = materialize_capture_request_(device_instance_id, request);
    completion->set_value(std::make_pair(ok, request));
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return false;
  }

  const auto result = completed.get();
  if (!result.first) {
    return false;
  }
  out = result.second;
  return true;
}

bool CoreRuntime::materialize_capture_request(uint64_t device_instance_id, CaptureRequest& out) const {
  return materialize_capture_request_for_server(device_instance_id, out);
}

bool CoreRuntime::materialize_capture_request_(uint64_t device_instance_id, CaptureRequest& out) const {
  assert(core_thread_.is_core_thread());

  if (device_instance_id == 0) {
    return false;
  }
  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return false;
  }
  const bool supports_multi_image = prov->supports_multi_image_still_sequence();

  const CaptureTemplate tmpl = prov->capture_template();
  out.device_instance_id = device_instance_id;
  out.rig_id = 0;
  out.width = tmpl.profile.width;
  out.height = tmpl.profile.height;
  out.format_fourcc = tmpl.profile.format_fourcc == 0 ? FOURCC_RGBA : tmpl.profile.format_fourcc;
  out.profile_version = 0;
  out.picture = tmpl.picture;
  out.still_image_bundle = make_default_metered_still_image_bundle();

  if (const auto* rec = devices_.find(device_instance_id)) {
    if (rec->capture_width > 0) {
      out.width = rec->capture_width;
    }
    if (rec->capture_height > 0) {
      out.height = rec->capture_height;
    }
    if (rec->capture_format != 0) {
      out.format_fourcc = rec->capture_format;
    }
    out.profile_version = rec->capture_profile_version;
    out.picture = rec->capture_picture;
    out.still_image_bundle = rec->capture_still_image_bundle;
  }

  if (!(out.width > 0 && out.height > 0)) {
    return false;
  }
  return is_valid_capture_still_image_bundle(out.still_image_bundle, supports_multi_image);
}

TryTriggerDeviceCaptureStatus CoreRuntime::trigger_device_capture_with_capture_id_(
    uint64_t device_instance_id,
    uint64_t capture_id) {
  assert(core_thread_.is_core_thread());

  if (device_instance_id == 0 || capture_id == 0) {
    return TryTriggerDeviceCaptureStatus::InvalidArgument;
  }

  CaptureRequest req{};
  if (!materialize_capture_request_(device_instance_id, req)) {
    return TryTriggerDeviceCaptureStatus::Busy;
  }
  req.capture_id = capture_id;

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    return TryTriggerDeviceCaptureStatus::Busy;
  }
  const ProviderResult pr = prov->trigger_capture(req);
  if (!pr.ok()) {
    return TryTriggerDeviceCaptureStatus::ProviderRejected;
  }

  begin_capture_stream_preemption_(capture_id, device_instance_id);
  (void)suppress_queued_repeating_stream_frames_for_capture_();
  return TryTriggerDeviceCaptureStatus::OK;
}

TryTriggerDeviceCaptureStatus CoreRuntime::try_trigger_device_capture_with_capture_id_for_server(
    uint64_t device_instance_id,
    uint64_t capture_id) {
  if (device_instance_id == 0 || capture_id == 0) {
    return TryTriggerDeviceCaptureStatus::InvalidArgument;
  }

  if (core_thread_.is_core_thread()) {
    const uint64_t direct_begin_ns = capture_latency_trace_now_ns();
    const TryTriggerDeviceCaptureStatus status =
        trigger_device_capture_with_capture_id_(device_instance_id, capture_id);
    const uint64_t direct_end_ns = capture_latency_trace_now_ns();
    capture_latency_trace_printf(
        "core_device_admission capture_id=%llu device_id=%llu post_to_core_us=0 core_queue_wait_us=0 core_execution_us=%llu status=%u path=direct",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>((direct_end_ns - direct_begin_ns) / 1000ull),
        static_cast<unsigned>(status));
    capture_latency_trace_printf(
        "core_device_future capture_id=%llu device_id=%llu post_to_core_us=0 future_wait_us=0 total_us=%llu status=%u path=direct",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>((direct_end_ns - direct_begin_ns) / 1000ull),
        static_cast<unsigned>(status));
    return status;
  }

  auto completion = std::make_shared<std::promise<TryTriggerDeviceCaptureStatus>>();
  std::future<TryTriggerDeviceCaptureStatus> completed = completion->get_future();
  const uint64_t post_begin_ns = capture_latency_trace_now_ns();
  const CoreThread::DiagnosticSnapshot post_snapshot = core_thread_.diagnostic_snapshot();
  const CoreThread::PostResult pr = try_post([this,
                                              device_instance_id,
                                              capture_id,
                                              completion,
                                              post_begin_ns,
                                              post_snapshot]() {
    const uint64_t core_start_ns = capture_latency_trace_now_ns();
    const CoreThread::DiagnosticSnapshot start_snapshot = core_thread_.diagnostic_snapshot();
    const size_t runtime_request_depth_at_start = requests_.size();
    const size_t runtime_provider_fact_depth_at_start = provider_facts_.size();
    const TryTriggerDeviceCaptureStatus status =
        trigger_device_capture_with_capture_id_(device_instance_id, capture_id);
    const uint64_t core_end_ns = capture_latency_trace_now_ns();
    const uint64_t core_queue_wait_us = (core_start_ns - post_begin_ns) / 1000ull;
    capture_latency_trace_emit_core_command_wait_context(
        "device_capture",
        capture_id,
        device_instance_id,
        "device_id",
        core_queue_wait_us,
        post_snapshot,
        start_snapshot,
        runtime_request_depth_at_start,
        runtime_provider_fact_depth_at_start);
    capture_latency_trace_printf(
        "core_device_admission capture_id=%llu device_id=%llu post_to_core_us=0 core_queue_wait_us=%llu core_execution_us=%llu status=%u path=queued",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>(core_queue_wait_us),
        static_cast<unsigned long long>((core_end_ns - core_start_ns) / 1000ull),
        static_cast<unsigned>(status));
    completion->set_value(status);
  });
  const uint64_t post_end_ns = capture_latency_trace_now_ns();
  if (pr != CoreThread::PostResult::Enqueued) {
    capture_latency_trace_printf(
        "core_device_admission_post_failed capture_id=%llu device_id=%llu post_us=%llu post_result=%u",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(device_instance_id),
        static_cast<unsigned long long>((post_end_ns - post_begin_ns) / 1000ull),
        static_cast<unsigned>(pr));
    return TryTriggerDeviceCaptureStatus::Busy;
  }

  const TryTriggerDeviceCaptureStatus status = completed.get();
  const uint64_t wait_end_ns = capture_latency_trace_now_ns();
  capture_latency_trace_printf(
      "core_device_future capture_id=%llu device_id=%llu post_to_core_us=%llu future_wait_us=%llu total_us=%llu status=%u path=queued",
      static_cast<unsigned long long>(capture_id),
      static_cast<unsigned long long>(device_instance_id),
      static_cast<unsigned long long>((post_end_ns - post_begin_ns) / 1000ull),
      static_cast<unsigned long long>((wait_end_ns - post_end_ns) / 1000ull),
      static_cast<unsigned long long>((wait_end_ns - post_begin_ns) / 1000ull),
      static_cast<unsigned>(status));
  return status;
}

CoreRuntime::RigPreflightResult CoreRuntime::preflight_rig_participants_materialize_(uint64_t rig_id) const {
  assert(core_thread_.is_core_thread());

  RigPreflightResult out{};
  out.rig_id = rig_id;
  if (rig_id == 0) {
    out.failure = RigPreflightFailure::RigNotFound;
    return out;
  }

  const auto* rig = rigs_.find(rig_id);
  if (!rig) {
    out.failure = RigPreflightFailure::RigNotFound;
    return out;
  }
  if (rig->member_hardware_ids.empty()) {
    out.failure = RigPreflightFailure::EmptyMembership;
    return out;
  }

  out.participants.reserve(rig->member_hardware_ids.size());
  std::map<uint64_t, bool> seen_devices;

  for (size_t i = 0; i < rig->member_hardware_ids.size(); ++i) {
    const std::string& hardware_id = rig->member_hardware_ids[i];
    uint64_t resolved_device_id = 0;
    size_t matches = 0;
    for (const auto& [device_instance_id, rec] : devices_.all()) {
      if (rec.hardware_id == hardware_id && rec.open) {
        ++matches;
        resolved_device_id = device_instance_id;
      }
    }

    if (matches == 0) {
      out.failure = RigPreflightFailure::HardwareIdUnresolved;
      out.failure_member_index = i;
      out.failure_hardware_id = hardware_id;
      return out;
    }
    if (matches > 1) {
      out.failure = RigPreflightFailure::HardwareIdAmbiguous;
      out.failure_member_index = i;
      out.failure_hardware_id = hardware_id;
      return out;
    }
    if (seen_devices.find(resolved_device_id) != seen_devices.end()) {
      out.failure = RigPreflightFailure::DuplicateResolvedDevice;
      out.failure_member_index = i;
      out.failure_hardware_id = hardware_id;
      out.failure_device_instance_id = resolved_device_id;
      return out;
    }
    seen_devices.emplace(resolved_device_id, true);

    CaptureRequest req{};
    if (!materialize_capture_request_(resolved_device_id, req)) {
      out.failure = RigPreflightFailure::MaterializeFailed;
      out.failure_member_index = i;
      out.failure_hardware_id = hardware_id;
      out.failure_device_instance_id = resolved_device_id;
      return out;
    }

    RigPreflightParticipant participant{};
    participant.hardware_id = hardware_id;
    participant.device_instance_id = resolved_device_id;
    participant.request = req;
    out.participants.push_back(std::move(participant));
  }

  out.ok = true;
  out.failure = RigPreflightFailure::None;
  return out;
}

CoreRuntime::RigAdmittedRequestBundle CoreRuntime::admit_rig_cohort_from_preflight_(
    uint64_t rig_id,
    uint64_t capture_id,
    const RigPreflightResult& preflight) {
  RigAdmittedRequestBundle out{};
  out.capture_id = capture_id;
  out.rig_id = rig_id;

  if (capture_id == 0 || rig_id == 0) {
    out.failure = RigCohortAdmissionFailure::InvalidCaptureId;
    return out;
  }
  if (!preflight.ok || preflight.failure != RigPreflightFailure::None || preflight.rig_id != rig_id) {
    out.failure = RigCohortAdmissionFailure::PreflightFailed;
    return out;
  }
  if (preflight.participants.empty()) {
    out.failure = RigCohortAdmissionFailure::EmptyParticipants;
    return out;
  }

  CoreCaptureCohortRegistry::CohortRecord cohort{};
  cohort.capture_id = capture_id;
  cohort.rig_id = rig_id;
  cohort.expected_participants.reserve(preflight.participants.size());
  out.participants.reserve(preflight.participants.size());
  for (const auto& p : preflight.participants) {
    if (p.device_instance_id == 0) {
      out.failure = RigCohortAdmissionFailure::PreflightFailed;
      out.participants.clear();
      return out;
    }
    cohort.expected_participants.push_back({p.device_instance_id, p.hardware_id});
    RigAdmittedParticipantRequest ap{};
    ap.hardware_id = p.hardware_id;
    ap.request = p.request;
    ap.request.capture_id = capture_id;
    ap.request.rig_id = rig_id;
    ap.request.device_instance_id = p.device_instance_id;
    out.participants.push_back(std::move(ap));
  }

  if (!capture_cohort_registry_.insert(std::move(cohort))) {
    out.failure = RigCohortAdmissionFailure::DuplicateCaptureId;
    out.participants.clear();
    return out;
  }

  out.ok = true;
  out.failure = RigCohortAdmissionFailure::None;
  return out;
}

CoreRuntime::RigSubmissionResult CoreRuntime::submit_admitted_rig_bundle_(
    const RigAdmittedRequestBundle& bundle) {
  RigSubmissionResult out{};
  out.capture_id = bundle.capture_id;
  out.rig_id = bundle.rig_id;

  if (!bundle.ok || bundle.capture_id == 0 || bundle.rig_id == 0 || bundle.participants.empty()) {
    out.failure = RigSubmissionFailure::InvalidBundle;
    return out;
  }

  ICameraProvider* prov = provider_.load(std::memory_order_acquire);
  if (!prov) {
    out.failure = RigSubmissionFailure::ProviderUnavailable;
    for (const auto& participant : bundle.participants) {
      capture_assembly_registry_.mark_capture_failed(bundle.capture_id,
                                                     participant.request.device_instance_id,
                                                     static_cast<uint32_t>(ProviderError::ERR_BAD_STATE));
    }
    (void)capture_cohort_registry_.mark_failed(bundle.capture_id,
                                               0,
                                               static_cast<uint32_t>(ProviderError::ERR_BAD_STATE),
                                               CoreCaptureCohortRegistry::CohortFailurePhase::SUBMISSION);
    out.provider_error_code = static_cast<uint32_t>(ProviderError::ERR_BAD_STATE);
    return out;
  }

  CaptureSubmission submission{};
  submission.capture_id = bundle.capture_id;
  submission.origin = CaptureSubmissionOrigin::RIG_CAPTURE;
  submission.rig_id = bundle.rig_id;
  submission.device_requests.reserve(bundle.participants.size());

  for (size_t i = 0; i < bundle.participants.size(); ++i) {
    const auto& participant = bundle.participants[i];
    if (!is_valid_capture_still_image_bundle(
            participant.request.still_image_bundle,
            prov->supports_multi_image_still_sequence())) {
      out.failure = RigSubmissionFailure::TriggerFailed;
      out.failed_index = i;
      out.failed_device_instance_id = participant.request.device_instance_id;
      out.provider_error_code = static_cast<uint32_t>(ProviderError::ERR_INVALID_ARGUMENT);
      capture_assembly_registry_.mark_capture_failed(bundle.capture_id,
                                                     participant.request.device_instance_id,
                                                     static_cast<uint32_t>(ProviderError::ERR_INVALID_ARGUMENT));
      (void)capture_cohort_registry_.mark_failed(bundle.capture_id,
                                                 participant.request.device_instance_id,
                                                 static_cast<uint32_t>(ProviderError::ERR_INVALID_ARGUMENT),
                                                 CoreCaptureCohortRegistry::CohortFailurePhase::SUBMISSION);
      return out;
    }
    submission.device_requests.push_back(participant.request);
  }

  const ProviderResult pr = prov->trigger_capture_submission(submission);
  if (!pr.ok()) {
    out.failure = RigSubmissionFailure::TriggerFailed;
    out.failed_index = 0;
    out.failed_device_instance_id = submission.device_requests.empty() ? 0 : submission.device_requests.front().device_instance_id;
    out.provider_error_code = static_cast<uint32_t>(pr.code);
    for (const auto& participant : bundle.participants) {
      capture_assembly_registry_.mark_capture_failed(bundle.capture_id,
                                                     participant.request.device_instance_id,
                                                     static_cast<uint32_t>(pr.code));
    }
    (void)capture_cohort_registry_.mark_failed(bundle.capture_id,
                                               out.failed_device_instance_id,
                                               static_cast<uint32_t>(pr.code),
                                               CoreCaptureCohortRegistry::CohortFailurePhase::SUBMISSION);
    return out;
  }

  begin_capture_stream_preemption_for_bundle_(bundle);
  (void)suppress_queued_repeating_stream_frames_for_capture_();

  out.submitted_count = bundle.participants.size();
  out.ok = true;
  out.failure = RigSubmissionFailure::None;
  return out;
}

CoreRuntime::RigTriggerOrchestrationResult CoreRuntime::orchestrate_rig_capture_with_capture_id_(
    uint64_t rig_id,
    uint64_t capture_id) {
  assert(core_thread_.is_core_thread());

  RigTriggerOrchestrationResult out{};
  out.rig_id = rig_id;
  out.capture_id = capture_id;

  const RigPreflightResult preflight = preflight_rig_participants_materialize_(rig_id);
  if (!preflight.ok) {
    out.failure = RigOrchestrationFailure::PreflightFailed;
    out.preflight_failure = preflight.failure;
    return out;
  }

  if (capture_id == 0) {
    out.failure = RigOrchestrationFailure::InvalidCaptureId;
    return out;
  }

  const RigAdmittedRequestBundle admitted = admit_rig_cohort_from_preflight_(rig_id, capture_id, preflight);
  if (!admitted.ok) {
    out.failure = RigOrchestrationFailure::AdmissionFailed;
    out.admission_failure = admitted.failure;
    return out;
  }

  const RigSubmissionResult submitted = submit_admitted_rig_bundle_(admitted);
  if (!submitted.ok) {
    out.failure = RigOrchestrationFailure::SubmissionFailed;
    out.submission_failure = submitted.failure;
    out.submitted_count = submitted.submitted_count;
    out.failed_index = submitted.failed_index;
    out.failed_device_instance_id = submitted.failed_device_instance_id;
    out.provider_error_code = submitted.provider_error_code;
    return out;
  }

  out.ok = true;
  out.failure = RigOrchestrationFailure::None;
  out.submitted_count = submitted.submitted_count;
  return out;
}

#if defined(CAMBANG_INTERNAL_SMOKE)
CoreRuntime::RigPreflightResult CoreRuntime::preflight_rig_participants_materialize(uint64_t rig_id) const {
  if (core_thread_.is_core_thread()) {
    return preflight_rig_participants_materialize_(rig_id);
  }

  auto completion = std::make_shared<std::promise<RigPreflightResult>>();
  std::future<RigPreflightResult> completed = completion->get_future();
  CoreRuntime* self = const_cast<CoreRuntime*>(this);
  const CoreThread::PostResult pr = self->try_post([this, rig_id, completion]() {
    completion->set_value(preflight_rig_participants_materialize_(rig_id));
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    RigPreflightResult out{};
    out.rig_id = rig_id;
    out.failure = RigPreflightFailure::RigNotFound;
    return out;
  }
  return completed.get();
}

bool CoreRuntime::smoke_set_rig_member_hardware_ids(uint64_t rig_id, std::vector<std::string> member_hardware_ids) {
  if (rig_id == 0) {
    return false;
  }

  if (core_thread_.is_core_thread()) {
    const bool retained = rigs_.retain_member_hardware_ids(rig_id, std::move(member_hardware_ids));
    request_publish_from_core_unchecked();
    return retained;
  }

  auto completion = std::make_shared<std::promise<bool>>();
  std::future<bool> completed = completion->get_future();
  const CoreThread::PostResult pr = try_post([this, rig_id, member_hardware_ids = std::move(member_hardware_ids), completion]() mutable {
    const bool retained = rigs_.retain_member_hardware_ids(rig_id, std::move(member_hardware_ids));
    request_publish_from_core_unchecked();
    completion->set_value(retained);
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    return false;
  }

  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    return false;
  }
  return completed.get();
}

CoreRuntime::RigAdmittedRequestBundle CoreRuntime::smoke_admit_rig_cohort_from_preflight(
    uint64_t rig_id,
    uint64_t capture_id,
    const RigPreflightResult& preflight) {
  if (core_thread_.is_core_thread()) {
    return admit_rig_cohort_from_preflight_(rig_id, capture_id, preflight);
  }

  auto completion = std::make_shared<std::promise<RigAdmittedRequestBundle>>();
  std::future<RigAdmittedRequestBundle> completed = completion->get_future();
  const CoreThread::PostResult pr = try_post([this, rig_id, capture_id, preflight, completion]() {
    completion->set_value(admit_rig_cohort_from_preflight_(rig_id, capture_id, preflight));
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    RigAdmittedRequestBundle out{};
    out.rig_id = rig_id;
    out.capture_id = capture_id;
    out.failure = RigCohortAdmissionFailure::PreflightFailed;
    return out;
  }

  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    RigAdmittedRequestBundle out{};
    out.rig_id = rig_id;
    out.capture_id = capture_id;
    out.failure = RigCohortAdmissionFailure::PreflightFailed;
    return out;
  }
  return completed.get();
}

CoreRuntime::RigSubmissionResult CoreRuntime::smoke_submit_admitted_rig_bundle(
    const RigAdmittedRequestBundle& bundle) {
  if (core_thread_.is_core_thread()) {
    return submit_admitted_rig_bundle_(bundle);
  }

  auto completion = std::make_shared<std::promise<RigSubmissionResult>>();
  std::future<RigSubmissionResult> completed = completion->get_future();
  const CoreThread::PostResult pr = try_post([this, bundle, completion]() {
    completion->set_value(submit_admitted_rig_bundle_(bundle));
  });
  if (pr != CoreThread::PostResult::Enqueued) {
    RigSubmissionResult out{};
    out.capture_id = bundle.capture_id;
    out.rig_id = bundle.rig_id;
    out.failure = RigSubmissionFailure::ProviderUnavailable;
    return out;
  }

  if (completed.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    RigSubmissionResult out{};
    out.capture_id = bundle.capture_id;
    out.rig_id = bundle.rig_id;
    out.failure = RigSubmissionFailure::ProviderUnavailable;
    return out;
  }
  return completed.get();
}

CoreRuntime::RigTriggerOrchestrationResult CoreRuntime::smoke_orchestrate_rig_capture_with_capture_id(
    uint64_t rig_id,
    uint64_t capture_id) {
  return orchestrate_rig_capture_with_capture_id_for_server(rig_id, capture_id);
}

#endif

CoreRuntime::RigTriggerOrchestrationResult CoreRuntime::orchestrate_rig_capture_with_capture_id_for_server(
    uint64_t rig_id,
    uint64_t capture_id) {
  if (core_thread_.is_core_thread()) {
    const uint64_t direct_begin_ns = capture_latency_trace_now_ns();
    RigTriggerOrchestrationResult result = orchestrate_rig_capture_with_capture_id_(rig_id, capture_id);
    const uint64_t direct_end_ns = capture_latency_trace_now_ns();
    capture_latency_trace_printf(
        "core_rig_admission capture_id=%llu rig_id=%llu post_to_core_us=0 core_queue_wait_us=0 core_execution_us=%llu ok=%u submitted=%llu path=direct",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(rig_id),
        static_cast<unsigned long long>((direct_end_ns - direct_begin_ns) / 1000ull),
        result.ok ? 1u : 0u,
        static_cast<unsigned long long>(result.submitted_count));
    capture_latency_trace_printf(
        "core_rig_future capture_id=%llu rig_id=%llu post_to_core_us=0 future_wait_us=0 total_us=%llu ok=%u submitted=%llu path=direct",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(rig_id),
        static_cast<unsigned long long>((direct_end_ns - direct_begin_ns) / 1000ull),
        result.ok ? 1u : 0u,
        static_cast<unsigned long long>(result.submitted_count));
    return result;
  }

  auto completion = std::make_shared<std::promise<RigTriggerOrchestrationResult>>();
  std::future<RigTriggerOrchestrationResult> completed = completion->get_future();
  const uint64_t post_begin_ns = capture_latency_trace_now_ns();
  const CoreThread::DiagnosticSnapshot post_snapshot = core_thread_.diagnostic_snapshot();
  const CoreThread::PostResult pr = try_post([this, rig_id, capture_id, completion, post_begin_ns, post_snapshot]() {
    const uint64_t core_start_ns = capture_latency_trace_now_ns();
    const CoreThread::DiagnosticSnapshot start_snapshot = core_thread_.diagnostic_snapshot();
    const size_t runtime_request_depth_at_start = requests_.size();
    const size_t runtime_provider_fact_depth_at_start = provider_facts_.size();
    RigTriggerOrchestrationResult result = orchestrate_rig_capture_with_capture_id_(rig_id, capture_id);
    const uint64_t core_end_ns = capture_latency_trace_now_ns();
    const uint64_t core_queue_wait_us = (core_start_ns - post_begin_ns) / 1000ull;
    capture_latency_trace_emit_core_command_wait_context(
        "rig_capture",
        capture_id,
        rig_id,
        "rig_id",
        core_queue_wait_us,
        post_snapshot,
        start_snapshot,
        runtime_request_depth_at_start,
        runtime_provider_fact_depth_at_start);
    capture_latency_trace_printf(
        "core_rig_admission capture_id=%llu rig_id=%llu post_to_core_us=0 core_queue_wait_us=%llu core_execution_us=%llu ok=%u submitted=%llu path=queued",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(rig_id),
        static_cast<unsigned long long>(core_queue_wait_us),
        static_cast<unsigned long long>((core_end_ns - core_start_ns) / 1000ull),
        result.ok ? 1u : 0u,
        static_cast<unsigned long long>(result.submitted_count));
    completion->set_value(std::move(result));
  });
  const uint64_t post_end_ns = capture_latency_trace_now_ns();
  if (pr != CoreThread::PostResult::Enqueued) {
    capture_latency_trace_printf(
        "core_rig_admission_post_failed capture_id=%llu rig_id=%llu post_us=%llu post_result=%u",
        static_cast<unsigned long long>(capture_id),
        static_cast<unsigned long long>(rig_id),
        static_cast<unsigned long long>((post_end_ns - post_begin_ns) / 1000ull),
        static_cast<unsigned>(pr));
    RigTriggerOrchestrationResult out{};
    out.rig_id = rig_id;
    out.capture_id = capture_id;
    out.failure = RigOrchestrationFailure::SubmissionFailed;
    out.submission_failure = RigSubmissionFailure::ProviderUnavailable;
    return out;
  }

  RigTriggerOrchestrationResult result = completed.get();
  const uint64_t wait_end_ns = capture_latency_trace_now_ns();
  capture_latency_trace_printf(
      "core_rig_future capture_id=%llu rig_id=%llu post_to_core_us=%llu future_wait_us=%llu total_us=%llu ok=%u submitted=%llu path=queued",
      static_cast<unsigned long long>(capture_id),
      static_cast<unsigned long long>(rig_id),
      static_cast<unsigned long long>((post_end_ns - post_begin_ns) / 1000ull),
      static_cast<unsigned long long>((wait_end_ns - post_end_ns) / 1000ull),
      static_cast<unsigned long long>((wait_end_ns - post_begin_ns) / 1000ull),
      result.ok ? 1u : 0u,
      static_cast<unsigned long long>(result.submitted_count));
  return result;
}

bool CoreRuntime::retain_rig_member_hardware_ids(
    uint64_t rig_id,
    const std::vector<std::string>& member_hardware_ids) {
  if (rig_id == 0) {
    return false;
  }
  auto pr = try_post([this, rig_id, member_hardware_ids]() {
    (void)rigs_.retain_member_hardware_ids(rig_id, member_hardware_ids);
    request_publish_from_core_unchecked();
  });
  return pr == CoreThread::PostResult::Enqueued;
}


void CoreRuntime::release_stream_display_demand_async(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  const CoreThread::PostResult pr = core_thread_.try_post_essential([this, stream_id]() {
    result_store_.release_stream_display_demand(stream_id);
  });
  if (pr == CoreThread::PostResult::Enqueued) {
    return;
  }
  account_display_demand_release_async_post_failure_(pr);
}

void CoreRuntime::account_display_demand_release_async_post_failure_(CoreThread::PostResult result) noexcept {
  switch (result) {
    case CoreThread::PostResult::QueueFull:
      display_demand_release_async_dropped_full_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Closed:
      display_demand_release_async_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::AllocFail:
      display_demand_release_async_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Enqueued:
      break;
  }
}

CoreRuntime::Stats CoreRuntime::stats_copy() const noexcept {
  Stats s;
  s.publish_requests_coalesced = publish_requests_coalesced_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_full = publish_requests_dropped_full_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_closed = publish_requests_dropped_closed_.load(std::memory_order_relaxed);
  s.publish_requests_dropped_allocfail = publish_requests_dropped_allocfail_.load(std::memory_order_relaxed);
  s.display_demand_release_async_dropped_full =
      display_demand_release_async_dropped_full_.load(std::memory_order_relaxed);
  s.display_demand_release_async_dropped_closed =
      display_demand_release_async_dropped_closed_.load(std::memory_order_relaxed);
  s.display_demand_release_async_dropped_allocfail =
      display_demand_release_async_dropped_allocfail_.load(std::memory_order_relaxed);
  return s;
}

CoreRuntime::ShutdownDiag CoreRuntime::shutdown_diag_copy() const noexcept {
  ShutdownDiag d;
  d.phase_code = shutdown_phase_code_.load(std::memory_order_relaxed);
  d.phase_changes = shutdown_phase_changes_.load(std::memory_order_relaxed);
  return d;
}

void CoreRuntime::request_publish() {
  // Lifecycle gating: only LIVE accepts publish work.
  if (state_.load(std::memory_order_acquire) != CoreRuntimeState::LIVE) {
    publish_requests_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
    publish_pending_.store(false, std::memory_order_release);
    return;
  }

  // Coalesce publish requests to avoid spamming the provider_to_core_commands queue.
  const bool was_pending = publish_pending_.exchange(true, std::memory_order_acq_rel);
  if (was_pending) {
    publish_requests_coalesced_.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  // Schedule a pump tick to perform publish after draining facts/requests.
  // We post directly to CoreThread (not via CoreRuntime::try_post) to avoid
  // routing this through the requests_ queue.
  const CoreThread::PostResult r = core_thread_.try_post([this]() {
    assert(core_thread_.is_core_thread());
    core_thread_.request_timer_tick();
  });

  if (r == CoreThread::PostResult::Enqueued) {
    return;
  }

  // Failed to enqueue: clear pending so callers can retry later.
  publish_pending_.store(false, std::memory_order_release);

  switch (r) {
    case CoreThread::PostResult::QueueFull:
      publish_requests_dropped_full_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Closed:
      publish_requests_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::AllocFail:
      publish_requests_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      break;
    case CoreThread::PostResult::Enqueued:
      break;
  }
}

void CoreRuntime::enqueue_provider_fact(ProviderToCoreCommand&& cmd) {
  assert(core_thread_.is_core_thread());
  if (suppress_repeating_stream_frame_for_capture_(std::move(cmd))) {
    core_thread_.request_timer_tick();
    return;
  }
  if (provider_fact_has_capture_id_for_priority(cmd)) {
    ++provider_capture_facts_queued_;
  }
  provider_facts_.push_back(std::move(cmd));
  // Facts wake the core pump so descendant realization can be observed through
  // dispatcher state changes and the normal coalesced publish path. Scenario
  // start must not force host-side publication ahead of provider truth.
  core_thread_.request_timer_tick();
}

void CoreRuntime::enqueue_request(CoreThread::Task task) {
  assert(core_thread_.is_core_thread());
  requests_.push_back(std::move(task));
  // Requests also wake the core pump; any snapshot publication remains a
  // consequence of provider facts or core-owned state mutation, not a broad
  // publish request from the host scenario-start path.
  core_thread_.request_timer_tick();
}

void CoreRuntime::request_publish_from_core_unchecked() {
  assert(core_thread_.is_core_thread());
  // Coalesce naturally: if already pending, keep it pending.
  publish_pending_.store(true, std::memory_order_release);
}

} // namespace cambang
