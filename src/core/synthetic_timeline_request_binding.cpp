#include "core/synthetic_timeline_request_binding.h"

#include <cstdio>
#include <string>

#include "core/core_runtime.h"
#include "imaging/api/timeline_teardown_trace.h"

namespace cambang {

namespace {

// TEMPORARY Scene 65 diagnostic instrumentation. Remove after the deferred
// startup due-zero chain break is identified; this is not a production trace
// facility and intentionally has no env var, project setting, or public API.
constexpr const char* kDeferredStartTracePrefix = "[CamBANG][DeferredStartTrace]";

const char* open_status_cstr(TryOpenDeviceStatus s) {
  switch (s) {
    case TryOpenDeviceStatus::OK: return "OK";
    case TryOpenDeviceStatus::InvalidArgument: return "INVALID_ARGUMENT";
    case TryOpenDeviceStatus::Busy: return "BUSY";
  }
  return "UNKNOWN";
}

const char* create_status_cstr(TryCreateStreamStatus s) {
  switch (s) {
    case TryCreateStreamStatus::OK: return "OK";
    case TryCreateStreamStatus::InvalidArgument: return "INVALID_ARGUMENT";
    case TryCreateStreamStatus::Busy: return "BUSY";
  }
  return "UNKNOWN";
}

const char* start_status_cstr(TryStartStreamStatus s) {
  switch (s) {
    case TryStartStreamStatus::OK: return "OK";
    case TryStartStreamStatus::InvalidArgument: return "INVALID_ARGUMENT";
    case TryStartStreamStatus::Busy: return "BUSY";
    case TryStartStreamStatus::ProviderRejected: return "PROVIDER_REJECTED";
  }
  return "UNKNOWN";
}

const char* stop_status_cstr(TryStopStreamStatus s) {
  switch (s) {
    case TryStopStreamStatus::OK: return "OK";
    case TryStopStreamStatus::InvalidArgument: return "INVALID_ARGUMENT";
    case TryStopStreamStatus::Busy: return "BUSY";
    case TryStopStreamStatus::ProviderRejected: return "PROVIDER_REJECTED";
  }
  return "UNKNOWN";
}

const char* destroy_status_cstr(TryDestroyStreamStatus s) {
  switch (s) {
    case TryDestroyStreamStatus::OK: return "OK";
    case TryDestroyStreamStatus::InvalidArgument: return "INVALID_ARGUMENT";
    case TryDestroyStreamStatus::Busy: return "BUSY";
    case TryDestroyStreamStatus::Started: return "STARTED";
    case TryDestroyStreamStatus::ProviderRejected: return "PROVIDER_REJECTED";
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
      std::fprintf(stderr,
                   "%s timeline_binding OpenDevice submit at_ns=%llu hardware_id=%s device_instance_id=%llu root_id=%llu\n",
                   kDeferredStartTracePrefix,
                   static_cast<unsigned long long>(ev.at_ns),
                   hardware_id.c_str(),
                   static_cast<unsigned long long>(ev.device_instance_id),
                   static_cast<unsigned long long>(ev.root_id));
      const TryOpenDeviceStatus rc = runtime.try_open_device(hardware_id, ev.device_instance_id, ev.root_id);
      std::fprintf(stderr,
                   "%s timeline_binding OpenDevice submit_result=%s device_instance_id=%llu\n",
                   kDeferredStartTracePrefix,
                   open_status_cstr(rc),
                   static_cast<unsigned long long>(ev.device_instance_id));
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
    case SyntheticEventType::CreateStream: {
      std::fprintf(stderr,
                   "%s timeline_binding CreateStream submit at_ns=%llu stream_id=%llu device_instance_id=%llu\n",
                   kDeferredStartTracePrefix,
                   static_cast<unsigned long long>(ev.at_ns),
                   static_cast<unsigned long long>(ev.stream_id),
                   static_cast<unsigned long long>(ev.device_instance_id));
      const TryCreateStreamStatus rc = runtime.try_create_stream(
          ev.stream_id,
          ev.device_instance_id,
          StreamIntent::PREVIEW,
          nullptr,
          nullptr,
          0);
      std::fprintf(stderr,
                   "%s timeline_binding CreateStream submit_result=%s stream_id=%llu device_instance_id=%llu\n",
                   kDeferredStartTracePrefix,
                   create_status_cstr(rc),
                   static_cast<unsigned long long>(ev.stream_id),
                   static_cast<unsigned long long>(ev.device_instance_id));
      break;
    }
    case SyntheticEventType::DestroyStream: {
      const TryDestroyStreamStatus rc = runtime.try_destroy_stream(ev.stream_id);
      if (rc != TryDestroyStreamStatus::OK) {
        timeline_teardown_trace_emit("fail DestroyStream stream_id=%llu reason=submit_%s",
                                     static_cast<unsigned long long>(ev.stream_id),
                                     destroy_status_cstr(rc));
      }
      break;
    }
    case SyntheticEventType::StartStream: {
      std::fprintf(stderr,
                   "%s timeline_binding StartStream submit at_ns=%llu stream_id=%llu device_instance_id=%llu\n",
                   kDeferredStartTracePrefix,
                   static_cast<unsigned long long>(ev.at_ns),
                   static_cast<unsigned long long>(ev.stream_id),
                   static_cast<unsigned long long>(ev.device_instance_id));
      const TryStartStreamStatus rc = runtime.try_start_stream(ev.stream_id);
      std::fprintf(stderr,
                   "%s timeline_binding StartStream submit_result=%s stream_id=%llu\n",
                   kDeferredStartTracePrefix,
                   start_status_cstr(rc),
                   static_cast<unsigned long long>(ev.stream_id));
      break;
    }
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
    case SyntheticEventType::UpdateCapturePicture:
      if (ev.has_picture) {
        (void)runtime.try_set_capture_picture_config(ev.device_instance_id, ev.picture);
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
