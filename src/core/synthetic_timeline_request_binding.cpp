#include "core/synthetic_timeline_request_binding.h"

#include <string>

#include "core/core_runtime.h"
#include "imaging/api/timeline_teardown_trace.h"

namespace cambang {

namespace {

const char* stop_status_cstr(TryStopStreamStatus s) {
  switch (s) {
    case TryStopStreamStatus::OK: return "OK";
    case TryStopStreamStatus::InvalidArgument: return "INVALID_ARGUMENT";
    case TryStopStreamStatus::Busy: return "BUSY";
  }
  return "UNKNOWN";
}

const char* destroy_status_cstr(TryDestroyStreamStatus s) {
  switch (s) {
    case TryDestroyStreamStatus::OK: return "OK";
    case TryDestroyStreamStatus::InvalidArgument: return "INVALID_ARGUMENT";
    case TryDestroyStreamStatus::Busy: return "BUSY";
  }
  return "UNKNOWN";
}

const char* close_status_cstr(TryCloseDeviceStatus s) {
  switch (s) {
    case TryCloseDeviceStatus::OK: return "OK";
    case TryCloseDeviceStatus::InvalidArgument: return "INVALID_ARGUMENT";
    case TryCloseDeviceStatus::Busy: return "BUSY";
  }
  return "UNKNOWN";
}

void dispatch_timeline_request_to_core(const SyntheticScheduledEvent& ev, CoreRuntime& runtime) {
  switch (ev.type) {
    case SyntheticEventType::OpenDevice: {
      const std::string hardware_id = std::string("synthetic:") + std::to_string(ev.endpoint_index);
      (void)runtime.try_open_device(hardware_id, ev.device_instance_id, ev.root_id);
      break;
    }
    case SyntheticEventType::CloseDevice: {
      const TryCloseDeviceStatus rc = runtime.try_close_device(ev.device_instance_id);
      if (rc != TryCloseDeviceStatus::OK) {
        timeline_teardown_trace_emit("fail CloseDevice device_instance_id=%llu reason=submit_%s",
                                     static_cast<unsigned long long>(ev.device_instance_id),
                                     close_status_cstr(rc));
      }
      break;
    }
    case SyntheticEventType::CreateStream:
      (void)runtime.try_create_stream(
          ev.stream_id,
          ev.device_instance_id,
          StreamIntent::PREVIEW,
          nullptr,
          nullptr,
          0);
      break;
    case SyntheticEventType::DestroyStream: {
      const TryDestroyStreamStatus rc = runtime.try_destroy_stream(ev.stream_id);
      if (rc != TryDestroyStreamStatus::OK) {
        timeline_teardown_trace_emit("fail DestroyStream stream_id=%llu reason=submit_%s",
                                     static_cast<unsigned long long>(ev.stream_id),
                                     destroy_status_cstr(rc));
      }
      break;
    }
    case SyntheticEventType::StartStream:
      (void)runtime.try_start_stream(ev.stream_id);
      break;
    case SyntheticEventType::StopStream: {
      const TryStopStreamStatus rc = runtime.try_stop_stream(ev.stream_id);
      if (rc != TryStopStreamStatus::OK) {
        timeline_teardown_trace_emit("fail StopStream stream_id=%llu reason=submit_%s",
                                     static_cast<unsigned long long>(ev.stream_id),
                                     stop_status_cstr(rc));
      }
      break;
    }
    case SyntheticEventType::UpdateStreamPicture:
      if (ev.has_picture) {
        (void)runtime.try_set_stream_picture_config(ev.stream_id, ev.picture);
      }
      break;
    case SyntheticEventType::EmitFrame:
      // Fact-like provider-originated event; remains provider-direct.
      break;
  }
}

} // namespace

SyntheticTimelineRequestDispatchHook make_synthetic_timeline_request_dispatch_hook(CoreRuntime& runtime) {
  return [&runtime](const SyntheticScheduledEvent& ev) {
    dispatch_timeline_request_to_core(ev, runtime);
  };
}

} // namespace cambang
