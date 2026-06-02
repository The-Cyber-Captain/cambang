#include "core/provider_callback_ingress.h"

#include <cstdio>
#include <utility>

#include "imaging/api/timeline_teardown_trace.h"
#include "core/resource_aggregate_telemetry.h"

namespace cambang {

namespace {

// TEMPORARY Scene 65 diagnostic instrumentation. Remove after the deferred
// startup due-zero chain break is identified; this is not a production trace
// facility and intentionally has no env var, project setting, or public API.
constexpr const char* kDeferredStartTracePrefix = "[CamBANG][DeferredStartTrace]";
constexpr uint64_t kDeferredStartTraceDeviceInstanceId = 10001ull;
constexpr uint64_t kDeferredStartTraceStreamId = 30001ull;

const char* ingress_post_result_trace_name(CoreThread::PostResult r) noexcept {
  switch (r) {
    case CoreThread::PostResult::Enqueued: return "ENQUEUED";
    case CoreThread::PostResult::QueueFull: return "QUEUE_FULL";
    case CoreThread::PostResult::Closed: return "CLOSED";
    case CoreThread::PostResult::AllocFail: return "ALLOC_FAIL";
  }
  return "UNKNOWN";
}

const char* ingress_command_trace_name(ProviderToCoreCommandType type) noexcept {
  switch (type) {
    case ProviderToCoreCommandType::PROVIDER_DEVICE_OPENED: return "PROVIDER_DEVICE_OPENED";
    case ProviderToCoreCommandType::PROVIDER_STREAM_CREATED: return "PROVIDER_STREAM_CREATED";
    case ProviderToCoreCommandType::PROVIDER_STREAM_STARTED: return "PROVIDER_STREAM_STARTED";
    default: return "OTHER";
  }
}

bool ingress_trace_command(const ProviderToCoreCommand& cmd, uint64_t& id_out) {
  id_out = 0;
  switch (cmd.type) {
    case ProviderToCoreCommandType::PROVIDER_DEVICE_OPENED: {
      const auto& p = std::get<CmdProviderDeviceOpened>(cmd.payload);
      id_out = p.device_instance_id;
      return p.device_instance_id == kDeferredStartTraceDeviceInstanceId;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_CREATED: {
      const auto& p = std::get<CmdProviderStreamCreated>(cmd.payload);
      id_out = p.stream_id;
      return p.stream_id == kDeferredStartTraceStreamId;
    }
    case ProviderToCoreCommandType::PROVIDER_STREAM_STARTED: {
      const auto& p = std::get<CmdProviderStreamStarted>(cmd.payload);
      id_out = p.stream_id;
      return p.stream_id == kDeferredStartTraceStreamId;
    }
    default:
      return false;
  }
}

} // namespace

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
                                                 std::function<void(ProviderToCoreCommand&&)> sink,
                                                 std::function<uint64_t()> core_monotonic_now_ns,
                                                 std::function<bool(uint64_t)> is_stream_display_demand_active)
    : core_thread_(core_thread),
      sink_(std::move(sink)),
      core_monotonic_now_ns_(std::move(core_monotonic_now_ns)),
      is_stream_display_demand_active_(std::move(is_stream_display_demand_active)) {}

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

bool ProviderCallbackIngress::is_stream_display_demand_active(uint64_t stream_id) {
  if (!is_stream_display_demand_active_) {
    return false;
  }
  return is_stream_display_demand_active_(stream_id);
}

ProviderCallbackIngress::Stats ProviderCallbackIngress::stats_copy() const noexcept {
  Stats s;
  s.commands_dropped_full = commands_dropped_full_.load(std::memory_order_relaxed);
  s.commands_dropped_closed = commands_dropped_closed_.load(std::memory_order_relaxed);
  s.commands_dropped_allocfail = commands_dropped_allocfail_.load(std::memory_order_relaxed);

  s.non_frame_rejected_closed = non_frame_rejected_closed_.load(std::memory_order_relaxed);
  s.non_frame_rejected_allocfail = non_frame_rejected_allocfail_.load(std::memory_order_relaxed);

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

bool ProviderCallbackIngress::is_frame_command_(ProviderToCoreCommandType type) noexcept {
  return type == ProviderToCoreCommandType::PROVIDER_FRAME;
}

void ProviderCallbackIngress::post_command(ProviderToCoreCommand cmd) {
  // Transport only: package command into a posted task.
  // Note: This uses std::function internally (CoreThread::Task), which may allocate.
  // That is acceptable for scaffolding; later we can replace with a fixed-capacity provider_to_core_commands queue.
  if (!core_thread_) {
    return;
  }

  const ProviderToCoreCommandType type = cmd.type;
  uint64_t deferred_trace_id = 0;
  const bool trace_deferred_command = ingress_trace_command(cmd, deferred_trace_id);
  uint64_t frame_stream_id = 0;

  // Preserve a copy of the frame for the failure-to-enqueue path. We must not rely on
  // moved-from ProviderToCoreCommand contents after constructing the posted lambda.
  FrameView fail_frame;
  bool has_fail_frame = false;
  if (is_frame_command_(type)) {
    auto& frame_payload = std::get<CmdProviderFrame>(cmd.payload);
    fail_frame = frame_payload.frame;
    frame_stream_id = frame_payload.frame.stream_id;
    has_fail_frame = true;
    global_resource_aggregate_telemetry().lease_created(make_framebuffer_lease_scoped_resource_telemetry_key(
        frame_payload.frame.stream_id,
        frame_payload.frame.acquisition_session_id));
  }

  // NOTE: sink_ is copied into the posted lambda. This keeps ingress transport-pure
  // and avoids coupling to any specific dispatcher type.
  auto task = [this, c = std::move(cmd), sink = sink_, frame_stream_id]() mutable {
    if (c.type == ProviderToCoreCommandType::PROVIDER_FRAME) {
      on_frame_ingress_dispatched_(frame_stream_id);
    }
    if (sink) {
      sink(std::move(c));
      return;
    }

    // Defensive fallback (should not be used in normal wiring):
    // Ensure release-on-drop semantics are upheld even if no sink is bound.
    if (c.type == ProviderToCoreCommandType::PROVIDER_FRAME) {
      auto& p = std::get<CmdProviderFrame>(c.payload);
      p.frame.release_now();
      p.frame.release = nullptr;
      p.frame.release_user = nullptr;
    }
  };

  const CoreThread::PostResult r = is_frame_command_(type)
      ? core_thread_->try_post(std::move(task))
      : core_thread_->try_post_essential(std::move(task));

  if (trace_deferred_command) {
    std::fprintf(stderr,
                 "%s provider_callback_ingress post_command type=%s id=%llu post_result=%s essential=%d\n",
                 kDeferredStartTracePrefix,
                 ingress_command_trace_name(type),
                 static_cast<unsigned long long>(deferred_trace_id),
                 ingress_post_result_trace_name(r),
                 is_frame_command_(type) ? 0 : 1);
  }

  if (r == CoreThread::PostResult::Enqueued) {
    return;
  }

  auto account_command_drop = [this, type](CoreThread::PostResult rr) {
    switch (rr) {
      case CoreThread::PostResult::QueueFull:
        commands_dropped_full_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::Closed:
        commands_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
        if (!is_frame_command_(type)) {
          non_frame_rejected_closed_.fetch_add(1, std::memory_order_relaxed);
        }
        break;
      case CoreThread::PostResult::AllocFail:
        commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
        if (!is_frame_command_(type)) {
          non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
        }
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
        global_resource_aggregate_telemetry().lease_released(make_framebuffer_lease_scoped_resource_telemetry_key(
            frame.stream_id,
            frame.acquisition_session_id));
        frame.release = nullptr;
        frame.release_user = nullptr;
        frames_released_on_drop_full_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::Closed:
        frames_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
        frame.release_now();
        global_resource_aggregate_telemetry().lease_released(make_framebuffer_lease_scoped_resource_telemetry_key(
            frame.stream_id,
            frame.acquisition_session_id));
        frame.release = nullptr;
        frame.release_user = nullptr;
        frames_released_on_drop_closed_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::AllocFail:
        frames_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
        frame.release_now();
        global_resource_aggregate_telemetry().lease_released(make_framebuffer_lease_scoped_resource_telemetry_key(
            frame.stream_id,
            frame.acquisition_session_id));
        frame.release = nullptr;
        frame.release_user = nullptr;
        frames_released_on_drop_allocfail_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::Enqueued:
        break;
    }
  };

  // Failure path: command never entered the core thread.
  // Frames remain pressure-droppable on the ordinary bounded queue. Non-frame
  // provider facts use CoreThread's essential queue, so QueueFull is not an
  // expected failure reason for lifecycle/native/error/capture-terminal truth.
  account_command_drop(r);

  if (has_fail_frame) {
    on_frame_ingress_failed_(frame_stream_id);
    account_frame_drop_and_release(r, fail_frame);
  }
}

void ProviderCallbackIngress::on_device_opened(uint64_t device_instance_id) {
  if (device_instance_id == kDeferredStartTraceDeviceInstanceId) {
    std::fprintf(stderr,
                 "%s provider_callback_ingress on_device_opened callback_post device_instance_id=%llu\n",
                 kDeferredStartTracePrefix,
                 static_cast<unsigned long long>(device_instance_id));
  }
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_DEVICE_OPENED;
  cmd.payload = CmdProviderDeviceOpened{device_instance_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_device_closed(uint64_t device_instance_id) {
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_DEVICE_CLOSED;
  cmd.payload = CmdProviderDeviceClosed{device_instance_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_stream_created(uint64_t stream_id) {
  if (stream_id == kDeferredStartTraceStreamId) {
    std::fprintf(stderr,
                 "%s provider_callback_ingress on_stream_created callback_post stream_id=%llu\n",
                 kDeferredStartTracePrefix,
                 static_cast<unsigned long long>(stream_id));
  }
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_STREAM_CREATED;
  cmd.payload = CmdProviderStreamCreated{stream_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_stream_destroyed(uint64_t stream_id) {
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_STREAM_DESTROYED;
  cmd.payload = CmdProviderStreamDestroyed{stream_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_stream_started(uint64_t stream_id) {
  if (stream_id == kDeferredStartTraceStreamId) {
    std::fprintf(stderr,
                 "%s provider_callback_ingress on_stream_started callback_post stream_id=%llu\n",
                 kDeferredStartTracePrefix,
                 static_cast<unsigned long long>(stream_id));
  }
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_STREAM_STARTED;
  cmd.payload = CmdProviderStreamStarted{stream_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_stream_stopped(uint64_t stream_id, ProviderError error_or_ok) {
  if (error_or_ok != ProviderError::OK) {
    timeline_teardown_trace_emit("fail StopStream stream_id=%llu reason=provider_error_%u",
                                 static_cast<unsigned long long>(stream_id),
                                 static_cast<unsigned>(error_or_ok));
  }
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_STREAM_STOPPED;
  cmd.payload = CmdProviderStreamStopped{stream_id, static_cast<uint32_t>(error_or_ok)};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_capture_started(uint64_t capture_id, uint64_t device_instance_id) {
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_CAPTURE_STARTED;
  cmd.payload = CmdProviderCaptureStarted{capture_id, device_instance_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_capture_completed(uint64_t capture_id, uint64_t device_instance_id) {
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_CAPTURE_COMPLETED;
  cmd.payload = CmdProviderCaptureCompleted{capture_id, device_instance_id};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_capture_failed(uint64_t capture_id,
                                                uint64_t device_instance_id,
                                                ProviderError error) {
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_CAPTURE_FAILED;
  cmd.payload = CmdProviderCaptureFailed{capture_id, device_instance_id, static_cast<uint32_t>(error)};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_frame(const FrameView& frame) {
  // FrameView is a provider-owned view. Ownership is returned to the provider only when
  // core calls frame.release_now(). The core dispatcher MUST ensure release-on-drop.
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_FRAME;
  (void)on_frame_ingress_enqueued_(frame.stream_id);
  cmd.payload = CmdProviderFrame{frame}; // copies the view (not the buffer)
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_device_error(uint64_t device_instance_id, ProviderError error) {
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_DEVICE_ERROR;
  cmd.payload = CmdProviderDeviceError{device_instance_id, static_cast<uint32_t>(error)};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_stream_error(uint64_t stream_id, ProviderError error) {
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_STREAM_ERROR;
  cmd.payload = CmdProviderStreamError{stream_id, static_cast<uint32_t>(error)};
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_native_object_created(const NativeObjectCreateInfo& info) {
  CmdProviderNativeObjectCreated p{};
  p.native_id = info.native_id;
  p.type = info.type;
  p.root_id = info.root_id;
  p.owner_device_instance_id = info.owner_device_instance_id;
  p.owner_acquisition_session_id = info.owner_acquisition_session_id;
  p.owner_stream_id = info.owner_stream_id;
  p.owner_provider_native_id = info.owner_provider_native_id;
  p.owner_rig_id = info.owner_rig_id;
  p.bytes_allocated = info.bytes_allocated;
  p.buffers_in_use = info.buffers_in_use;
  p.has_created_ns = info.has_created_ns;
  p.created_ns = info.created_ns;

  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_CREATED;
  cmd.payload = p;
  post_command(std::move(cmd));
}

void ProviderCallbackIngress::on_native_object_destroyed(const NativeObjectDestroyInfo& info) {
  ProviderToCoreCommand cmd;
  cmd.type = ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_DESTROYED;
  cmd.payload = CmdProviderNativeObjectDestroyed{info.native_id, info.has_destroyed_ns, info.destroyed_ns};
  post_command(std::move(cmd));
}

} // namespace cambang
