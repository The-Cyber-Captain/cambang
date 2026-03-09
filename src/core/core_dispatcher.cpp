// src/core/core_dispatcher.cpp

#include "core/core_dispatcher.h"

#include <variant>

namespace cambang {

namespace {
uint64_t frame_ts_to_core_ns(const CaptureTimestamp& ts) {
  if (ts.tick_ns == 0) {
    return 0;
  }
  if (ts.domain != CaptureTimestampDomain::CORE_MONOTONIC) {
    return 0;
  }
  return static_cast<uint64_t>(ts.value) * static_cast<uint64_t>(ts.tick_ns);
}
} // namespace



void CoreDispatcher::dispatch(CoreCommand&& cmd) {
  // NOTE: For this build slice we are proving lifecycle only.
  //
  // Add minimal core state mutation:
  // - Maintain a deterministic per-stream registry (created/started + counters).
  // - Still no downstream frame consumer: frames are ALWAYS released immediately.
  //
  // All operations are core-thread-only.

  stats_.commands_total++;

  switch (cmd.type) {
  case CoreCommandType::PROVIDER_DEVICE_OPENED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderDeviceOpened>(cmd.payload);
    if (devices_) {
      devices_->on_device_opened(p.device_instance_id);
    }
    relevant_state_changed_ = true;
    break;
  }

  case CoreCommandType::PROVIDER_DEVICE_CLOSED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderDeviceClosed>(cmd.payload);
    if (devices_) {
      devices_->on_device_closed(p.device_instance_id);
    }
    relevant_state_changed_ = true;
    break;
  }

  case CoreCommandType::PROVIDER_STREAM_CREATED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamCreated>(cmd.payload);
    if (streams_) {
      streams_->on_stream_created(p.stream_id);
    }
    relevant_state_changed_ = true;
    break;
  }

  case CoreCommandType::PROVIDER_STREAM_DESTROYED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamDestroyed>(cmd.payload);
    if (streams_) {
      streams_->on_stream_destroyed(p.stream_id);
    }
    relevant_state_changed_ = true;
    break;
  }

  case CoreCommandType::PROVIDER_STREAM_STARTED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamStarted>(cmd.payload);
    if (streams_) {
      streams_->on_stream_started(p.stream_id);
    }
    relevant_state_changed_ = true;
    break;
  }

  case CoreCommandType::PROVIDER_STREAM_STOPPED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamStopped>(cmd.payload);
    if (streams_) {
      streams_->on_stream_stopped(p.stream_id, p.error_code);
    }
    relevant_state_changed_ = true;
    break;
  }

  case CoreCommandType::PROVIDER_STREAM_ERROR: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamError>(cmd.payload);
    if (streams_) {
      streams_->on_stream_error(p.stream_id, p.error_code);
    }
    relevant_state_changed_ = true;
    break;
  }

  case CoreCommandType::PROVIDER_DEVICE_ERROR: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderDeviceError>(cmd.payload);
    if (devices_) {
      devices_->on_device_error(p.device_instance_id, p.error_code);
    }
    relevant_state_changed_ = true;
    break;
  }

  
case CoreCommandType::PROVIDER_NATIVE_OBJECT_CREATED: {
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
        p.owner_stream_id,
        p.bytes_allocated,
        p.buffers_in_use,
        creation_gen,
        created_ns);
  }
  relevant_state_changed_ = true;
  break;
}

case CoreCommandType::PROVIDER_NATIVE_OBJECT_DESTROYED: {
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

  case CoreCommandType::PROVIDER_FRAME: {
    auto& p = std::get<CmdProviderFrame>(cmd.payload);

    stats_.commands_handled++;
    stats_.frames_received++;

    const uint64_t sid = p.frame.stream_id;
    if (streams_) {
      const uint64_t integrated_ts_ns = frame_ts_to_core_ns(p.frame.capture_timestamp);
      if (!streams_->on_frame_received(sid, integrated_ts_ns, p.ingress_queue_depth)) {
        stats_.frames_unknown_stream++;
      }
    }

    if (frame_sink_) {
      // Delivered means handed off to the configured frame sink.
      FrameView frame = std::move(p.frame);
      p.frame.release = nullptr;
      p.frame.release_user = nullptr;
      frame_sink_->on_frame(std::move(frame));
      stats_.frames_released++;
      if (streams_) {
        streams_->on_frame_released(sid);
      }
    } else {
      // No sink configured: release-on-drop and count as dropped (not delivered).
      p.frame.release_now();
      stats_.frames_released++;
      p.frame.release = nullptr;
      p.frame.release_user = nullptr;
      if (streams_) {
        streams_->on_frame_dropped(sid);
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
