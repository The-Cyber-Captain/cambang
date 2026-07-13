// src/core/core_dispatcher.cpp
#include "core/core_dispatcher.h"
#include "core/resource_aggregate_telemetry.h"

#include <chrono>
#include <variant>

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
    if (provider_camera_fact_state_) {
      provider_camera_fact_state_->erase_device(p.device_instance_id);
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
      if (capture_lifecycle_ingress_sink_) {
        const uint64_t acquisition_session_id =
            acquisition_sessions_->resolve_session_id_for_capture(
                p.device_instance_id, p.capture_id, 0);
        capture_lifecycle_ingress_sink_(CoreCaptureLifecycleIngressEvent{
            CoreCaptureLifecycleIngressEvent::Kind::Started,
            p.capture_id,
            p.device_instance_id,
            acquisition_session_id,
            dispatch_begin_ns});
      }
    }
    relevant_state_changed_ = relevant_state_changed_ || state_changed;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_CAPTURE_COMPLETED: {
    const uint64_t dispatch_begin_ns = capture_latency_trace_now_ns();
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderCaptureCompleted>(cmd.payload);
    bool state_changed = false;
    if (acquisition_sessions_) {
      const uint64_t acquisition_session_id =
          acquisition_sessions_->resolve_session_id_for_capture(
              p.device_instance_id, p.capture_id, 0);
      const uint64_t completed_ns = now_ns_ ? now_ns_() : 0;
      state_changed = acquisition_sessions_->on_capture_completed(
          p.device_instance_id, p.capture_id, completed_ns);
      if (capture_assembly_registry_) {
        capture_assembly_registry_->mark_capture_completed(p.capture_id, p.device_instance_id);
      }
      if (capture_lifecycle_ingress_sink_) {
        capture_lifecycle_ingress_sink_(CoreCaptureLifecycleIngressEvent{
            CoreCaptureLifecycleIngressEvent::Kind::Completed,
            p.capture_id,
            p.device_instance_id,
            acquisition_session_id,
            dispatch_begin_ns});
      }
    }
    if (!acquisition_sessions_ && capture_assembly_registry_) {
      capture_assembly_registry_->mark_capture_completed(p.capture_id, p.device_instance_id);
    }
    relevant_state_changed_ = relevant_state_changed_ || state_changed;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_CAPTURE_FAILED: {
    const uint64_t dispatch_begin_ns = capture_latency_trace_now_ns();
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderCaptureFailed>(cmd.payload);
    bool state_changed = false;
    if (acquisition_sessions_) {
      const uint64_t acquisition_session_id =
          acquisition_sessions_->resolve_session_id_for_capture(
              p.device_instance_id, p.capture_id, 0);
      const uint64_t failed_ns = now_ns_ ? now_ns_() : 0;
      state_changed = acquisition_sessions_->on_capture_failed(
          p.device_instance_id, p.capture_id, p.error_code, failed_ns);
      if (capture_lifecycle_ingress_sink_) {
        capture_lifecycle_ingress_sink_(CoreCaptureLifecycleIngressEvent{
            CoreCaptureLifecycleIngressEvent::Kind::Failed,
            p.capture_id,
            p.device_instance_id,
            acquisition_session_id,
            dispatch_begin_ns});
      }
    }
    if (capture_assembly_registry_) {
      capture_assembly_registry_->mark_capture_failed(p.capture_id, p.device_instance_id, p.error_code);
    }
    relevant_state_changed_ = relevant_state_changed_ || state_changed;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_CAMERA_STATIC_FACTS: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderCameraStaticFacts>(cmd.payload);
    if (devices_) {
      const CoreDeviceRegistry::DeviceRecord* device = devices_->find(p.device_instance_id);
      if (device != nullptr && device->open && provider_camera_fact_state_ &&
          provider_camera_fact_state_->replace_static(p.device_instance_id, p.facts)) {
        relevant_state_changed_ = true;
      }
    }
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_CAPTURE_IMAGE_FACTS: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderCaptureImageFacts>(cmd.payload);
    if (devices_ && capture_assembly_registry_ && provider_camera_fact_state_) {
      const CoreDeviceRegistry::DeviceRecord* device = devices_->find(p.device_instance_id);
      if (device != nullptr && device->open &&
          capture_assembly_registry_->has_admitted_capture_member(
              p.capture_id, p.device_instance_id, p.image_member_index) &&
          provider_camera_fact_state_->replace_capture_image(
              ProviderCameraFactState::CaptureImageKey{
                  p.capture_id, p.device_instance_id, p.image_member_index},
              p.facts)) {
        relevant_state_changed_ = true;
      }
    }
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_FRAME: {
    uint64_t result_retention_ns = 0;
    auto& p = std::get<CmdProviderFrame>(cmd.payload);
    const uint64_t trace_capture_id = p.frame.capture_id;

    stats_.commands_handled++;
    stats_.frames_received++;

    const uint64_t sid = p.frame.stream_id;
    const uint64_t asid = p.frame.acquisition_session_id;
    std::optional<StreamIntent> stream_intent;
    bool retained_for_result = false;
    uint64_t integrated_ts_ns = 0;
    bool has_stream_record = (sid == 0);
    uint64_t stream_access_posture_epoch = 0;
    uint64_t capture_access_posture_epoch = 0;
    CoreRetainedProductionPlan stream_requested_retained_plan{};
    CoreRetainedProductionPlan capture_requested_retained_plan{};
    if (streams_) {
      integrated_ts_ns = frame_ts_to_core_ns(p.frame.capture_timestamp);
      if (!streams_->on_frame_received(sid, integrated_ts_ns)) {
        stats_.frames_unknown_stream++;
      }
      if (const CoreStreamRegistry::StreamRecord* stream_rec = streams_->find(sid); stream_rec != nullptr) {
        stream_intent = stream_rec->intent;
        stream_access_posture_epoch = stream_rec->access_posture_epoch;
        stream_requested_retained_plan = stream_rec->requested_retained_plan;
        has_stream_record = true;
      }
    } else {
      integrated_ts_ns = frame_ts_to_core_ns(p.frame.capture_timestamp);
    }
    uint64_t resolved_capture_session_id = 0;
    if (acquisition_sessions_ && p.frame.device_instance_id != 0 &&
        p.frame.capture_id != 0) {
      resolved_capture_session_id =
          acquisition_sessions_->resolve_session_id_for_capture(
              p.frame.device_instance_id,
              p.frame.capture_id,
              p.frame.acquisition_session_id);
      if (resolved_capture_session_id != 0) {
        if (const auto* session_rec =
                acquisition_sessions_->find(resolved_capture_session_id);
            session_rec != nullptr) {
          capture_access_posture_epoch = session_rec->capture_access_posture_epoch;
          capture_requested_retained_plan = session_rec->requested_retained_plan;
        }
      }
    }
    if (stream_requested_retained_plan.valid == false &&
        p.frame.stream_id != 0 &&
        p.frame.requested_retained_plan.valid) {
      stream_requested_retained_plan = p.frame.requested_retained_plan;
    }
    if (p.frame.capture_id != 0 &&
        p.frame.requested_retained_plan.valid) {
      // Capture frames carry the exact core-selected posture that produced the
      // frame. Preserve that per-capture attribution even if the owning
      // device/session requested plan has already advanced for a later
      // evaluation candidate while this capture is still in flight.
      capture_requested_retained_plan = p.frame.requested_retained_plan;
    } else if (capture_requested_retained_plan.valid == false &&
        devices_ && p.frame.device_instance_id != 0) {
      if (const CoreDeviceRegistry::DeviceRecord* device_rec = devices_->find(p.frame.device_instance_id);
          device_rec != nullptr) {
        capture_access_posture_epoch = device_rec->capture_access_posture_epoch;
        capture_requested_retained_plan = device_rec->requested_retained_plan;
      }
    }
    if (capture_requested_retained_plan.valid == false &&
        p.frame.capture_id != 0 &&
        p.frame.requested_retained_plan.valid) {
      capture_requested_retained_plan = p.frame.requested_retained_plan;
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
      if (lifecycle_allows_retention && has_stream_record) {
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
          if (CoreResultStore::try_build_capture_image_member_data_from_frame(
                  p.frame, image_member, capture_requested_retained_plan)) {
            retained_for_result = result_store_->append_additional_capture_image(
                p.frame.capture_id,
                p.frame.device_instance_id,
                std::move(image_member),
                capture_access_posture_epoch,
                capture_requested_retained_plan);
          }
          result_retention_ns += capture_latency_trace_now_ns() - retention_begin_ns;
          }
        } else {
          const uint64_t retention_begin_ns = capture_latency_trace_now_ns();
          retained_for_result = result_store_->retain_frame(
              p.frame,
              stream_intent,
              integrated_ts_ns,
              stream_access_posture_epoch,
              capture_access_posture_epoch,
              stream_requested_retained_plan,
              capture_requested_retained_plan);
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
      (void)result_retention_ns;
    }
    break;
  }

  default:
    stats_.commands_dropped++;
    break;
  }
}

} // namespace cambang
