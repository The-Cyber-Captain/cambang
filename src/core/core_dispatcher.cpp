// src/core/core_dispatcher.cpp

#include "core/core_dispatcher.h"

#include <cstdlib>
#include <variant>

namespace cambang {

namespace {
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
    const uint64_t now_ns = now_ns_ ? now_ns_() : 0;
    if (devices_) {
      devices_->on_device_opened(p.device_instance_id);
    }
    if (acquisition_sessions_) {
      acquisition_sessions_->on_device_opened(p.device_instance_id, now_ns);
    }
    relevant_state_changed_ = true;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_DEVICE_CLOSED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderDeviceClosed>(cmd.payload);
    const uint64_t now_ns = now_ns_ ? now_ns_() : 0;
    if (devices_) {
      devices_->on_device_closed(p.device_instance_id);
    }
    if (acquisition_sessions_) {
      acquisition_sessions_->on_device_closed(p.device_instance_id, now_ns);
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
    relevant_state_changed_ = true;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_STREAM_STARTED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamStarted>(cmd.payload);
    if (streams_) {
      streams_->on_stream_started(p.stream_id);
    }
    relevant_state_changed_ = true;
    break;
  }

  case ProviderToCoreCommandType::PROVIDER_STREAM_STOPPED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamStopped>(cmd.payload);
    if (streams_) {
      streams_->on_stream_stopped(p.stream_id, p.error_code);
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
  }
  relevant_state_changed_ = true;
  break;
}

case ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_DESTROYED: {
  stats_.commands_handled++;
  const auto& p = std::get<CmdProviderNativeObjectDestroyed>(cmd.payload);
  if (native_objects_) {
    const uint64_t integration_destroyed_ns = now_ns_ ? now_ns_() : 0;
    const uint64_t destroyed_ns = p.has_destroyed_ns ? p.destroyed_ns : integration_destroyed_ns;
    native_objects_->on_native_object_destroyed(p.native_id, destroyed_ns, integration_destroyed_ns);
  }
  relevant_state_changed_ = true;
  break;
}

  case ProviderToCoreCommandType::PROVIDER_FRAME: {
    auto& p = std::get<CmdProviderFrame>(cmd.payload);

    stats_.commands_handled++;
    stats_.frames_received++;

    const uint64_t sid = p.frame.stream_id;
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
    if (result_store_) {
      const bool lifecycle_allows_retention =
          result_retention_allowed_ ? result_retention_allowed_() : true;
      if (result_routing_enabled_ && lifecycle_allows_retention && has_stream_record) {
        retained_for_result = result_store_->retain_frame(p.frame, stream_intent, integrated_ts_ns);
      }
    }

    if (frame_sink_) {
      // Delivered means handed off to the configured frame sink.
      FrameView frame = std::move(p.frame);
      p.frame.release = nullptr;
      p.frame.release_user = nullptr;
      const CoreVisibilityPath visibility_path = frame_sink_->on_frame(std::move(frame));
      stats_.frames_released++;
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
    break;
  }

  default:
    stats_.commands_dropped++;
    break;
  }
}

} // namespace cambang
