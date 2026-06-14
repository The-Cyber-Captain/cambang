// src/core/core_dispatcher.cpp
#include "imaging/api/capture_latency_trace_diagnostics.h"

#include "core/core_dispatcher.h"
#include "core/resource_aggregate_telemetry.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <variant>

namespace cambang {

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

uint64_t frame_ts_to_core_ns(const CaptureTimestamp& ts) {
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
} // namespace



void CoreDispatcher::dispatch(ProviderToCoreCommand&& cmd) {
  // NOTE: For this build slice we are proving lifecycle only.
  //
  // Add minimal core state mutation:
  // - Maintain a deterministic per-stream registry (created/started + counters).
  // - Still no downstream frame consumer: frames are ALWAYS released immediately.
  //
  // All operations are core-thread-only.

  stats_.commands_total++;

  switch (cmd.type) {
  case ProviderToCoreCommandType::PROVIDER_DEVICE_OPENED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderDeviceOpened>(cmd.payload);
    if (devices_) {
      devices_->on_device_opened(p.device_instance_id);
    }
    relevant_state_changed_ = true;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_DEVICE_CLOSED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderDeviceClosed>(cmd.payload);
    if (devices_) {
      devices_->on_device_closed(p.device_instance_id);
    }
    relevant_state_changed_ = true;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_STREAM_CREATED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamCreated>(cmd.payload);
    if (streams_) {
      streams_->on_stream_created(p.stream_id);
    }
    relevant_state_changed_ = true;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_STREAM_DESTROYED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamDestroyed>(cmd.payload);
    if (streams_) {
      streams_->on_stream_destroyed(p.stream_id);
    }
    if (result_store_) {
      result_store_->remove_stream_result(p.stream_id);
    }
    relevant_state_changed_ = true;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_STREAM_STARTED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamStarted>(cmd.payload);
    if (streams_) {
      streams_->on_provider_stream_started(p.stream_id);
    }
    relevant_state_changed_ = true;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_STREAM_STOPPED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamStopped>(cmd.payload);
    if (streams_) {
      streams_->on_provider_stream_stopped(p.stream_id, p.error_code);
    }
    relevant_state_changed_ = true;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_STREAM_ERROR: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamError>(cmd.payload);
    if (streams_) {
      streams_->on_stream_error(p.stream_id, p.error_code);
    }
    relevant_state_changed_ = true;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_DEVICE_ERROR: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderDeviceError>(cmd.payload);
    if (devices_) {
      devices_->on_device_error(p.device_instance_id, p.error_code);
    }
    relevant_state_changed_ = true;
    break;
  }

  
case ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_CREATED: {
  stats_.commands_handled++;
  const auto& p = std::get<CmdProviderNativeObjectCreated>(cmd.payload);
  bool state_changed = false;
    if (native_objects_) {
    const uint64_t fallback_created_ns = now_ns_ ? now_ns_() : 0;
    const uint64_t created_ns = p.has_created_ns ? p.created_ns : fallback_created_ns;
    const uint64_t creation_gen = current_gen_ ? *current_gen_ : 0;
    native_objects_->on_native_object_created(
        p.native_id,
        p.type,
        p.root_id,
        p.owner_device_instance_id,
        p.owner_acquisition_session_id,
        p.owner_stream_id,
        p.owner_provider_native_id,
        p.owner_rig_id,
        p.bytes_allocated,
        p.buffers_in_use,
        creation_gen,
        created_ns);
    state_changed = true;
    if (acquisition_sessions_) {
      uint32_t capture_width = 0;
      uint32_t capture_height = 0;
      uint32_t capture_format = 0;
      uint64_t capture_profile_version = 0;
      CaptureStillImageBundle capture_still_image_bundle = make_default_metered_still_image_bundle();
      if (devices_ && p.owner_device_instance_id != 0) {
        if (const CoreDeviceRegistry::DeviceRecord* device = devices_->find(p.owner_device_instance_id);
            device != nullptr) {
          capture_width = device->capture_width;
          capture_height = device->capture_height;
          capture_format = device->capture_format;
          capture_profile_version = device->capture_profile_version;
          capture_still_image_bundle = device->capture_still_image_bundle;
        }
      }
      state_changed =
          acquisition_sessions_->on_native_object_created(
              p.native_id,
              p.type,
              p.owner_device_instance_id,
              created_ns,
              capture_width,
              capture_height,
              capture_format,
              capture_profile_version,
              capture_still_image_bundle) ||
          state_changed;
    }
  }
  relevant_state_changed_ = relevant_state_changed_ || state_changed;
  break;
}

case ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_DESTROYED: {
  stats_.commands_handled++;
  const auto& p = std::get<CmdProviderNativeObjectDestroyed>(cmd.payload);
  bool state_changed = false;
  if (native_objects_) {
    const uint64_t integration_destroyed_ns = now_ns_ ? now_ns_() : 0;
    const uint64_t destroyed_ns = p.has_destroyed_ns ? p.destroyed_ns : integration_destroyed_ns;
    native_objects_->on_native_object_destroyed(p.native_id, destroyed_ns, integration_destroyed_ns);
    state_changed = true;
    if (acquisition_sessions_) {
      state_changed =
          acquisition_sessions_->on_native_object_destroyed(p.native_id, destroyed_ns) || state_changed;
    }
  }
  relevant_state_changed_ = relevant_state_changed_ || state_changed;
  break;
}

  case ProviderToCoreCommandType::PROVIDER_CAPTURE_STARTED: {
    const uint64_t dispatch_begin_ns = capture_latency_trace_now_ns();
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderCaptureStarted>(cmd.payload);
    bool state_changed = false;
    if (acquisition_sessions_) {
      uint32_t capture_width = 0;
      uint32_t capture_height = 0;
      uint32_t capture_format = 0;
      uint64_t capture_profile_version = 0;
      CaptureStillImageBundle capture_still_image_bundle = make_default_metered_still_image_bundle();
      if (devices_ && p.device_instance_id != 0) {
        if (const CoreDeviceRegistry::DeviceRecord* device = devices_->find(p.device_instance_id);
            device != nullptr) {
          capture_width = device->capture_width;
          capture_height = device->capture_height;
          capture_format = device->capture_format;
          capture_profile_version = device->capture_profile_version;
          capture_still_image_bundle = device->capture_still_image_bundle;
        }
      }
      const uint64_t started_ns = now_ns_ ? now_ns_() : 0;
      state_changed = acquisition_sessions_->on_capture_started(p.device_instance_id,
                                                                p.capture_id,
                                                                started_ns,
                                                                capture_width,
                                                                capture_height,
                                                                capture_format,
                                                                capture_profile_version,
                                                                capture_still_image_bundle);
    }
    relevant_state_changed_ = relevant_state_changed_ || state_changed;
    capture_latency_trace_printf(
        "core_dispatch_capture_started capture_id=%llu device_id=%llu total_us=%llu state_changed=%u",
        static_cast<unsigned long long>(p.capture_id),
        static_cast<unsigned long long>(p.device_instance_id),
        static_cast<unsigned long long>((capture_latency_trace_now_ns() - dispatch_begin_ns) / 1000ull),
        state_changed ? 1u : 0u);
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_CAPTURE_COMPLETED: {
    const uint64_t dispatch_begin_ns = capture_latency_trace_now_ns();
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderCaptureCompleted>(cmd.payload);
    bool state_changed = false;
    if (acquisition_sessions_) {
      const uint64_t completed_ns = now_ns_ ? now_ns_() : 0;
      state_changed = acquisition_sessions_->on_capture_completed(
          p.device_instance_id, p.capture_id, completed_ns);
    }
    if (capture_assembly_registry_) {
      capture_assembly_registry_->mark_capture_completed(p.capture_id, p.device_instance_id);
    }
    relevant_state_changed_ = relevant_state_changed_ || state_changed;
    capture_latency_trace_printf(
        "core_dispatch_capture_completed capture_id=%llu device_id=%llu total_us=%llu state_changed=%u",
        static_cast<unsigned long long>(p.capture_id),
        static_cast<unsigned long long>(p.device_instance_id),
        static_cast<unsigned long long>((capture_latency_trace_now_ns() - dispatch_begin_ns) / 1000ull),
        state_changed ? 1u : 0u);
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_CAPTURE_FAILED: {
    const uint64_t dispatch_begin_ns = capture_latency_trace_now_ns();
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderCaptureFailed>(cmd.payload);
    bool state_changed = false;
    if (acquisition_sessions_) {
      const uint64_t failed_ns = now_ns_ ? now_ns_() : 0;
      state_changed = acquisition_sessions_->on_capture_failed(
          p.device_instance_id, p.capture_id, p.error_code, failed_ns);
    }
    if (capture_assembly_registry_) {
      capture_assembly_registry_->mark_capture_failed(p.capture_id, p.device_instance_id, p.error_code);
    }
    relevant_state_changed_ = relevant_state_changed_ || state_changed;
    capture_latency_trace_printf(
        "core_dispatch_capture_failed capture_id=%llu device_id=%llu error=%u total_us=%llu state_changed=%u",
        static_cast<unsigned long long>(p.capture_id),
        static_cast<unsigned long long>(p.device_instance_id),
        static_cast<unsigned>(p.error_code),
        static_cast<unsigned long long>((capture_latency_trace_now_ns() - dispatch_begin_ns) / 1000ull),
        state_changed ? 1u : 0u);
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_FRAME: {
    const uint64_t dispatch_begin_ns = capture_latency_trace_now_ns();
    uint64_t result_retention_ns = 0;
    auto& p = std::get<CmdProviderFrame>(cmd.payload);
    const uint64_t trace_capture_id = p.frame.capture_id;
    const uint64_t trace_device_id = p.frame.device_instance_id;

    stats_.commands_handled++;
    stats_.frames_received++;

    const uint64_t sid = p.frame.stream_id;
    const uint64_t asid = p.frame.acquisition_session_id;
    std::optional<StreamIntent> stream_intent;
    bool retained_for_result = false;
    uint64_t integrated_ts_ns = 0;
    bool has_stream_record = (sid == 0);
    if (streams_) {
      integrated_ts_ns = frame_ts_to_core_ns(p.frame.capture_timestamp);
      if (!streams_->on_frame_received(sid, integrated_ts_ns)) {
        stats_.frames_unknown_stream++;
      }
      if (const CoreStreamRegistry::StreamRecord* stream_rec = streams_->find(sid); stream_rec != nullptr) {
        stream_intent = stream_rec->intent;
        has_stream_record = true;
      }
    } else {
      integrated_ts_ns = frame_ts_to_core_ns(p.frame.capture_timestamp);
    }
    const bool is_additional_bracket =
        p.frame.capture_image.routing == CaptureImageRouting::ADDITIONAL_BRACKET;
    const uint32_t frame_member_index = p.frame.capture_image.image_member_index;
    const int32_t frame_member_applied_ev = p.frame.capture_image.applied_exposure_compensation_milli_ev;
    const bool frame_member_has_realized_ev = p.frame.capture_image.has_realized_exposure_compensation_milli_ev;
    const int32_t frame_member_realized_ev = p.frame.capture_image.realized_exposure_compensation_milli_ev;
    if (result_store_) {
      const bool lifecycle_allows_retention =
          result_retention_allowed_ ? result_retention_allowed_() : true;
      if (result_routing_enabled_ && lifecycle_allows_retention && has_stream_record) {
        if (is_additional_bracket) {
          // This tranche only accepts still-capture-only bracket frames.
          // Reject malformed/unsupported bracket routes deterministically.
          if (p.frame.capture_id == 0 || p.frame.stream_id != 0) {
            retained_for_result = false;
          } else {
          CoreCaptureResultData::ImageMemberData image_member{};
          image_member.role = CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET;
          image_member.image_member_index = frame_member_index;
          image_member.applied_exposure_compensation_milli_ev = frame_member_applied_ev;
          image_member.has_realized_exposure_compensation_milli_ev = frame_member_has_realized_ev;
          image_member.realized_exposure_compensation_milli_ev = frame_member_realized_ev;
          image_member.capture_timestamp_ns = integrated_ts_ns;
          const uint64_t retention_begin_ns = capture_latency_trace_now_ns();
          if (CoreResultStore::try_build_capture_image_member_data_from_frame(p.frame, image_member)) {
            retained_for_result = result_store_->append_additional_capture_image(
                p.frame.capture_id, p.frame.device_instance_id, std::move(image_member));
          }
          result_retention_ns += capture_latency_trace_now_ns() - retention_begin_ns;
          }
        } else {
          const uint64_t retention_begin_ns = capture_latency_trace_now_ns();
          retained_for_result = result_store_->retain_frame(p.frame, stream_intent, integrated_ts_ns);
          result_retention_ns += capture_latency_trace_now_ns() - retention_begin_ns;
        }
      }
    }
    if (capture_assembly_registry_ && retained_for_result && p.frame.capture_id != 0 && !is_additional_bracket) {
      capture_assembly_registry_->mark_default_image_retained(p.frame.capture_id, p.frame.device_instance_id);
    }

    if (frame_sink_) {
      // Delivered means handed off to the configured frame sink.
      FrameView frame = std::move(p.frame);
      p.frame.release = nullptr;
      p.frame.release_user = nullptr;
      const CoreVisibilityPath visibility_path = frame_sink_->on_frame(std::move(frame));
      stats_.frames_released++;
      global_resource_aggregate_telemetry().lease_released(make_framebuffer_lease_scoped_resource_telemetry_key(
          sid,
          asid));
      if (streams_) {
        streams_->on_frame_released(sid);
        if (streams_->on_visibility_path(sid, visibility_path)) {
          relevant_state_changed_ = true;
        }
      }
    } else {
      // No sink configured: release payload deterministically.
      // If result retention already accepted this frame, it is not counted as dropped.
      p.frame.release_now();
      stats_.frames_released++;
      global_resource_aggregate_telemetry().lease_released(make_framebuffer_lease_scoped_resource_telemetry_key(
          sid,
          asid));
      p.frame.release = nullptr;
      p.frame.release_user = nullptr;
      if (streams_) {
        if (retained_for_result) {
          streams_->on_frame_released(sid);
        } else {
          streams_->on_frame_dropped(sid);
        }
      }
    }
    if (trace_capture_id != 0) {
      capture_latency_trace_printf(
          "core_dispatch_capture_frame capture_id=%llu device_id=%llu acquisition_session_id=%llu member=%u retained=%u retention_us=%llu total_us=%llu",
          static_cast<unsigned long long>(trace_capture_id),
          static_cast<unsigned long long>(trace_device_id),
          static_cast<unsigned long long>(asid),
          static_cast<unsigned>(frame_member_index),
          retained_for_result ? 1u : 0u,
          static_cast<unsigned long long>(result_retention_ns / 1000ull),
          static_cast<unsigned long long>((capture_latency_trace_now_ns() - dispatch_begin_ns) / 1000ull));
    }
    break;
  }

  default:
    stats_.commands_dropped++;
    break;
  }
}

} // namespace cambang
