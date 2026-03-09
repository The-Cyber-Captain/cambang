#include "core/provider_callback_ingress.h"

#include <utility>

namespace cambang {

uint32_t ProviderCallbackIngress::on_frame_ingress_enqueued_(uint64_t stream_id) {
  if (stream_id == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(ingress_mu_);
  uint32_t& depth = stream_ingress_depth_[stream_id];
  depth++;
  return depth;
}

void ProviderCallbackIngress::on_frame_ingress_failed_(uint64_t stream_id) {
  if (stream_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(ingress_mu_);
  auto it = stream_ingress_depth_.find(stream_id);
  if (it == stream_ingress_depth_.end()) {
    return;
  }
  if (it->second > 0) {
    it->second--;
  }
  if (it->second == 0) {
    stream_ingress_depth_.erase(it);
  }
}

void ProviderCallbackIngress::on_frame_ingress_dispatched_(uint64_t stream_id) {
  on_frame_ingress_failed_(stream_id);
}

ProviderCallbackIngress::ProviderCallbackIngress(CoreThread* core_thread,
                                                 std::function<void(CoreCommand&&)> sink,
                                                 std::function<uint64_t()> core_monotonic_now_ns)
    : core_thread_(core_thread),
      sink_(std::move(sink)),
      core_monotonic_now_ns_(std::move(core_monotonic_now_ns)) {}

uint64_t ProviderCallbackIngress::allocate_native_id(NativeObjectType /*type*/) {
  // Core-issued, globally unique for this process/session.
  // Providers may request IDs from any thread; atomic is sufficient.
  return native_id_seq_.fetch_add(1, std::memory_order_relaxed);
}

uint64_t ProviderCallbackIngress::core_monotonic_now_ns() {
  if (core_monotonic_now_ns_) {
    return core_monotonic_now_ns_();
  }
  return 0;
}

ProviderCallbackIngress::Stats ProviderCallbackIngress::stats_copy() const noexcept {
  Stats s;
  s.commands_dropped_full = commands_dropped_full_.load(std::memory_order_relaxed);
  s.commands_dropped_closed = commands_dropped_closed_.load(std::memory_order_relaxed);
  s.commands_dropped_allocfail = commands_dropped_allocfail_.load(std::memory_order_relaxed);

  s.frames_dropped_full = frames_dropped_full_.load(std::memory_order_relaxed);
  s.frames_dropped_closed = frames_dropped_closed_.load(std::memory_order_relaxed);
  s.frames_dropped_allocfail = frames_dropped_allocfail_.load(std::memory_order_relaxed);

  s.frames_released_on_drop_full = frames_released_on_drop_full_.load(std::memory_order_relaxed);
  s.frames_released_on_drop_closed = frames_released_on_drop_closed_.load(std::memory_order_relaxed);
  s.frames_released_on_drop_allocfail = frames_released_on_drop_allocfail_.load(std::memory_order_relaxed);
  return s;
}

uint32_t ProviderCallbackIngress::ingress_depth_for_stream(uint64_t stream_id) const {
  if (stream_id == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(ingress_mu_);
  const auto it = stream_ingress_depth_.find(stream_id);
  return (it != stream_ingress_depth_.end()) ? it->second : 0;
}

void ProviderCallbackIngress::post_command(CoreCommand cmd) {
  // Transport only: package command into a posted task.
  // Note: This uses std::function internally (CoreThread::Task), which may allocate.
  // That is acceptable for scaffolding; later we can replace with fixed-capacity mailbox.
  if (!core_thread_) {
    return;
  }

  const CoreCommandType type = cmd.type;
  uint64_t frame_stream_id = 0;

  // Preserve a copy of the frame for the failure-to-enqueue path. We must not rely on
  // moved-from CoreCommand contents after constructing the posted lambda.
  FrameView fail_frame;
  bool has_fail_frame = false;
  if (type == CoreCommandType::PROVIDER_FRAME) {
    auto& frame_payload = std::get<CmdProviderFrame>(cmd.payload);
    fail_frame = frame_payload.frame;
    frame_stream_id = frame_payload.frame.stream_id;
    has_fail_frame = true;
  }

  // NOTE: sink_ is copied into the posted lambda. This keeps ingress transport-pure
  // and avoids coupling to any specific dispatcher type.
  const CoreThread::PostResult r = core_thread_->try_post([this, c = std::move(cmd), sink = sink_, frame_stream_id]() mutable {
    if (c.type == CoreCommandType::PROVIDER_FRAME) {
      on_frame_ingress_dispatched_(frame_stream_id);
    }
    if (sink) {
      sink(std::move(c));
      return;
    }

    // Defensive fallback (should not be used in normal wiring):
    // Ensure release-on-drop semantics are upheld even if no sink is bound.
    if (c.type == CoreCommandType::PROVIDER_FRAME) {
      auto& p = std::get<CmdProviderFrame>(c.payload);
      p.frame.release_now();
      p.frame.release = nullptr;
      p.frame.release_user = nullptr;
    }
  });

  if (r == CoreThread::PostResult::Enqueued) {
    return;
  }

  auto account_command_drop = [this](CoreThread::PostResult rr) {
    switch (rr) {
      case CoreThread::PostResult::QueueFull:
        commands_dropped_full_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::Closed:
        commands_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::AllocFail:
        commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::Enqueued:
        break;
    }
  };

  auto account_frame_drop_and_release = [this](CoreThread::PostResult rr, FrameView& frame) {
    switch (rr) {
      case CoreThread::PostResult::QueueFull:
        frames_dropped_full_.fetch_add(1, std::memory_order_relaxed);
        frame.release_now();
        frame.release = nullptr;
        frame.release_user = nullptr;
        frames_released_on_drop_full_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::Closed:
        frames_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
        frame.release_now();
        frame.release = nullptr;
        frame.release_user = nullptr;
        frames_released_on_drop_closed_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::AllocFail:
        frames_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
        frame.release_now();
        frame.release = nullptr;
        frame.release_user = nullptr;
        frames_released_on_drop_allocfail_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::Enqueued:
        break;
    }
  };

  // Failure path: command never entered the core thread.
  // Best-effort enqueue; otherwise drop-with-accounting.
  account_command_drop(r);

  if (has_fail_frame) {
    on_frame_ingress_failed_(frame_stream_id);
    account_frame_drop_and_release(r, fail_frame);
  }
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
  (void)on_frame_ingress_enqueued_(frame.stream_id);
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
  p.owner_device_instance_id = info.owner_device_instance_id;
  p.owner_stream_id = info.owner_stream_id;
  p.bytes_allocated = info.bytes_allocated;
  p.buffers_in_use = info.buffers_in_use;
  p.has_created_ns = info.has_created_ns;
  p.created_ns = info.created_ns;

  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_NATIVE_OBJECT_CREATED;
  cmd.payload = p;
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_native_object_destroyed(const NativeObjectDestroyInfo& info) {
  CoreCommand cmd;
  cmd.type = CoreCommandType::PROVIDER_NATIVE_OBJECT_DESTROYED;
  cmd.payload = CmdProviderNativeObjectDestroyed{info.native_id, info.has_destroyed_ns, info.destroyed_ns};
  post_command(std::move(cmd));
}

} // namespace cambang
