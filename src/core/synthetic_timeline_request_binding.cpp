#include "core/synthetic_timeline_request_binding.h"

#include <string>
#include <utility>

#include "core/core_runtime.h"
#include "imaging/broker/provider_broker.h"

#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  #include "imaging/synthetic/provider.h"
#endif

namespace cambang {

namespace {

void dispatch_timeline_request_to_core(const SyntheticScheduledEvent& ev, CoreRuntime& runtime) {
  switch (ev.type) {
    case SyntheticEventType::OpenDevice: {
      const std::string hardware_id = std::string("synthetic:") + std::to_string(ev.endpoint_index);
      (void)runtime.try_open_device(hardware_id, ev.device_instance_id, ev.root_id);
      break;
    }
    case SyntheticEventType::CloseDevice:
      (void)runtime.try_close_device(ev.device_instance_id);
      break;
    case SyntheticEventType::CreateStream:
      (void)runtime.try_create_stream(
          ev.stream_id,
          ev.device_instance_id,
          StreamIntent::PREVIEW,
          nullptr,
          nullptr,
          0);
      break;
    case SyntheticEventType::DestroyStream:
      (void)runtime.try_destroy_stream(ev.stream_id);
      break;
    case SyntheticEventType::StartStream:
      (void)runtime.try_start_stream(ev.stream_id);
      break;
    case SyntheticEventType::StopStream:
      (void)runtime.try_stop_stream(ev.stream_id);
      break;
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

void bind_synthetic_timeline_request_dispatch(ICameraProvider& provider, CoreRuntime& runtime) {
  auto hook = [&runtime](const SyntheticScheduledEvent& ev) {
    dispatch_timeline_request_to_core(ev, runtime);
  };

  if (auto* broker = dynamic_cast<ProviderBroker*>(&provider)) {
    broker->set_synthetic_timeline_request_dispatch_hook(std::move(hook));
    return;
  }

#if defined(CAMBANG_ENABLE_SYNTHETIC) && CAMBANG_ENABLE_SYNTHETIC
  if (auto* synthetic = dynamic_cast<SyntheticProvider*>(&provider)) {
    synthetic->set_timeline_request_dispatch_hook_for_host(std::move(hook));
    return;
  }
#endif

  // Non-synthetic providers do not expose a synthetic timeline request hook.
}

} // namespace cambang
