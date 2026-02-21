#include "core/provider_callback_ingress.h"

#include <utility>

namespace cambang {

ProviderCallbackIngress::ProviderCallbackIngress(CoreThread* core_thread)
    : core_thread_(core_thread) {}

void ProviderCallbackIngress::post_command(CoreCommand cmd) {
  // Transport only: package command into a posted task.
  // Note: This uses std::function internally (CoreThread::Task), which may allocate.
  // That is acceptable for scaffolding; later we can replace with fixed-capacity mailbox.
  if (!core_thread_) {
    return;
  }

  core_thread_->post([c = std::move(cmd)]() mutable {
    // No dispatch here yet. The next layer will pop/visit commands on core thread.
    // For now, this preserves the deterministic ingress path.
    (void)c;
  });
}

void ProviderCallbackIngress::on_device_opened(uint64_t device_instance_id) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_DEVICE_OPENED;
  cmd.payload = CmdProviderDeviceOpened{device_instance_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_device_closed(uint64_t device_instance_id) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_DEVICE_CLOSED;
  cmd.payload = CmdProviderDeviceClosed{device_instance_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_stream_created(uint64_t stream_id) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_STREAM_CREATED;
  cmd.payload = CmdProviderStreamCreated{stream_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_stream_destroyed(uint64_t stream_id) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_STREAM_DESTROYED;
  cmd.payload = CmdProviderStreamDestroyed{stream_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_stream_started(uint64_t stream_id) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_STREAM_STARTED;
  cmd.payload = CmdProviderStreamStarted{stream_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_stream_stopped(uint64_t stream_id, ProviderError error_or_ok) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_STREAM_STOPPED;
  cmd.payload = CmdProviderStreamStopped{stream_id, static_cast<uint32_t>(error_or_ok)};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_capture_started(uint64_t capture_id) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_CAPTURE_STARTED;
  cmd.payload = CmdProviderCaptureStarted{capture_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_capture_completed(uint64_t capture_id) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_CAPTURE_COMPLETED;
  cmd.payload = CmdProviderCaptureCompleted{capture_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_capture_failed(uint64_t capture_id, ProviderError error) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_CAPTURE_FAILED;
  cmd.payload = CmdProviderCaptureFailed{capture_id, static_cast<uint32_t>(error)};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_frame(const FrameView& frame) {
  // FrameView is a provider-owned view. Ownership is returned to the provider only when
  // core calls frame.release_now(). The core dispatcher MUST ensure release-on-drop.
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_FRAME;
  cmd.payload = CmdProviderFrame{frame}; // copies the view (not the buffer)
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_device_error(uint64_t device_instance_id, ProviderError error) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_DEVICE_ERROR;
  cmd.payload = CmdProviderDeviceError{device_instance_id, static_cast<uint32_t>(error)};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_stream_error(uint64_t stream_id, ProviderError error) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_STREAM_ERROR;
  cmd.payload = CmdProviderStreamError{stream_id, static_cast<uint32_t>(error)};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_native_object_created(const NativeObjectCreateInfo& info) {
  CmdProviderNativeObjectCreated p{};
  p.native_id = info.native_id;
  p.type = info.type;
  p.root_id = info.root_id;

  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_NATIVE_OBJECT_CREATED;
  cmd.payload = p;
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_native_object_destroyed(const NativeObjectDestroyInfo& info) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_NATIVE_OBJECT_DESTROYED;
  cmd.payload = CmdProviderNativeObjectDestroyed{info.native_id};
  post_command(std::move(cmd));
}

} // namespace cambang