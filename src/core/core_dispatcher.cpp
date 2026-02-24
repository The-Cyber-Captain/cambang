// src/core/core_dispatcher.cpp

#include "core/core_dispatcher.h"

#include <variant>

namespace cambang {

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
    break;
  }

  case CoreCommandType::PROVIDER_DEVICE_CLOSED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderDeviceClosed>(cmd.payload);
    if (devices_) {
      devices_->on_device_closed(p.device_instance_id);
    }
    break;
  }

  case CoreCommandType::PROVIDER_STREAM_CREATED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamCreated>(cmd.payload);
    if (streams_) {
      streams_->on_stream_created(p.stream_id);
    }
    break;
  }

  case CoreCommandType::PROVIDER_STREAM_DESTROYED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamDestroyed>(cmd.payload);
    if (streams_) {
      streams_->on_stream_destroyed(p.stream_id);
    }
    break;
  }

  case CoreCommandType::PROVIDER_STREAM_STARTED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamStarted>(cmd.payload);
    if (streams_) {
      streams_->on_stream_started(p.stream_id);
    }
    break;
  }

  case CoreCommandType::PROVIDER_STREAM_STOPPED: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamStopped>(cmd.payload);
    if (streams_) {
      streams_->on_stream_stopped(p.stream_id, p.error_code);
    }
    break;
  }

  case CoreCommandType::PROVIDER_STREAM_ERROR: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderStreamError>(cmd.payload);
    if (streams_) {
      streams_->on_stream_error(p.stream_id, p.error_code);
    }
    break;
  }

  case CoreCommandType::PROVIDER_DEVICE_ERROR: {
    stats_.commands_handled++;
    const auto& p = std::get<CmdProviderDeviceError>(cmd.payload);
    if (devices_) {
      devices_->on_device_error(p.device_instance_id, p.error_code);
    }
    break;
  }

  case CoreCommandType::PROVIDER_FRAME: {
    auto& p = std::get<CmdProviderFrame>(cmd.payload);

    stats_.commands_handled++;
    stats_.frames_received++;

    const uint64_t sid = p.frame.stream_id;
    if (streams_) {
      if (!streams_->on_frame_received(sid)) {
        stats_.frames_unknown_stream++;
      }
    }

    if (frame_sink_) {
      // Hand off to sink (core thread). Sink is responsible for deterministic
      // release. For this stage, it copies immediately and releases now.
      FrameView frame = std::move(p.frame);
      // Defensive hygiene: ensure the command payload cannot double-release.
      p.frame.release = nullptr;
      p.frame.release_user = nullptr;
      frame_sink_->on_frame(std::move(frame));
      stats_.frames_released++;
    } else {
      // Release-on-drop at dispatch boundary.
      p.frame.release_now();
      stats_.frames_released++;

      // Defensive hygiene: prevent accidental double-release if this payload is
      // inspected/logged/re-dispatched in future scaffolding.
      p.frame.release = nullptr;
      p.frame.release_user = nullptr;
    }

    if (streams_) {
      // Even if unknown stream, try anyway; registry will ignore if missing.
      streams_->on_frame_released(sid);
    }
    break;
  }

  default:
    stats_.commands_dropped++;
    break;
  }
}

} // namespace cambang
