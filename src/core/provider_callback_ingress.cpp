#include "core/provider_callback_ingress.h"

#include <chrono>
#include <utility>

#include "imaging/api/frame_release_utils.h"
#include "imaging/api/timeline_teardown_trace.h"

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
                                                 std::function<SinkResult(PendingCommand&)> sink,
                                                 std::function<uint64_t()> core_monotonic_now_ns,
                                                 std::function<bool(uint64_t)> is_stream_display_demand_active,
                                                 std::function<void(ProviderTransportFailure)> on_transport_failure,
                                                 std::function<bool()> transport_accepting)
    : core_thread_(core_thread),
      sink_(std::move(sink)),
      core_monotonic_now_ns_(std::move(core_monotonic_now_ns)),
      is_stream_display_demand_active_(std::move(is_stream_display_demand_active)),
      on_transport_failure_(std::move(on_transport_failure)),
      transport_accepting_(std::move(transport_accepting)) {}

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

void ProviderCallbackIngress::on_transport_failure(ProviderTransportFailure failure) {
  signal_transport_failure_(failure);
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

bool ProviderCallbackIngress::is_authoritative_frame_(const FrameView& frame) noexcept {
  return frame.capture_id != 0 || frame.stream_id == 0;
}

void ProviderCallbackIngress::signal_transport_failure_(ProviderTransportFailure failure) noexcept {
  if (on_transport_failure_) {
    try {
      on_transport_failure_(failure);
    } catch (...) {
    }
  }
}

void ProviderCallbackIngress::post_command(ProviderToCoreCommand cmd) noexcept {
  // Transport only: package command into a posted task.
  // Note: This uses std::function internally (CoreThread::Task), which may allocate.
  // That is acceptable for scaffolding; later we can replace with a fixed-capacity provider_to_core_commands queue.
  const ProviderToCoreCommandType type = cmd.type;
  uint64_t trace_capture_id = 0;
  uint64_t trace_device_id = 0;
  uint64_t trace_acquisition_session_id = 0;
  uint32_t trace_member_index = 0;
  if (type == ProviderToCoreCommandType::PROVIDER_CAPTURE_STARTED) {
    const auto& p = std::get<CmdProviderCaptureStarted>(cmd.payload);
    trace_capture_id = p.capture_id;
    trace_device_id = p.device_instance_id;
  } else if (type == ProviderToCoreCommandType::PROVIDER_CAPTURE_COMPLETED) {
    const auto& p = std::get<CmdProviderCaptureCompleted>(cmd.payload);
    trace_capture_id = p.capture_id;
    trace_device_id = p.device_instance_id;
  } else if (type == ProviderToCoreCommandType::PROVIDER_CAPTURE_FAILED) {
    const auto& p = std::get<CmdProviderCaptureFailed>(cmd.payload);
    trace_capture_id = p.capture_id;
    trace_device_id = p.device_instance_id;
  }
  uint64_t frame_stream_id = 0;
  bool is_capture_critical_frame = false;
  bool signal_closed_as_transport_failure = false;

  FrameView fail_frame;
  bool has_fail_frame = false;
  try {
    if (is_frame_command_(type)) {
      auto& frame_payload = std::get<CmdProviderFrame>(cmd.payload);
      adopt_singular_frame_release_owner(frame_payload.frame);
      fail_frame = frame_payload.frame;
      frame_stream_id = frame_payload.frame.stream_id;
      trace_capture_id = frame_payload.frame.capture_id;
      trace_device_id = frame_payload.frame.device_instance_id;
      trace_acquisition_session_id = frame_payload.frame.acquisition_session_id;
      trace_member_index = frame_payload.frame.capture_image.image_member_index;
      is_capture_critical_frame = is_authoritative_frame_(frame_payload.frame);
      has_fail_frame = true;
      global_resource_aggregate_telemetry().lease_created(make_framebuffer_lease_scoped_resource_telemetry_key(
          frame_payload.frame.stream_id,
          frame_payload.frame.acquisition_session_id));
    }
  } catch (...) {
    if (is_frame_command_(type) && has_fail_frame) {
      on_frame_ingress_failed_(frame_stream_id);
      (void)release_owned_frame_once(fail_frame, [this]() noexcept {
        signal_transport_failure_(ProviderTransportFailure::CallbackException);
      });
      frames_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      frames_released_on_drop_allocfail_.fetch_add(1, std::memory_order_relaxed);
    } else {
      commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
      signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
    }
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

  auto release_dropped_frame = [this](FrameView& frame) noexcept {
    (void)release_owned_frame_once(frame, [this]() noexcept {
      signal_transport_failure_(ProviderTransportFailure::CallbackException);
    });
  };

  auto account_frame_drop_and_release = [this, release_dropped_frame](CoreThread::PostResult rr, FrameView& frame) {
    switch (rr) {
      case CoreThread::PostResult::QueueFull:
        frames_dropped_full_.fetch_add(1, std::memory_order_relaxed);
        release_dropped_frame(frame);
        frames_released_on_drop_full_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::Closed:
        frames_dropped_closed_.fetch_add(1, std::memory_order_relaxed);
        release_dropped_frame(frame);
        frames_released_on_drop_closed_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::AllocFail:
        frames_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
        release_dropped_frame(frame);
        frames_released_on_drop_allocfail_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CoreThread::PostResult::Enqueued:
        break;
    }
  };

  auto account_post_failure = [&](CoreThread::PostResult r) {
    // Failure path: command never entered the core thread.
    // Repeating stream frames remain pressure-droppable on the ordinary bounded
    // queue. Non-frame provider facts and still-capture frames use CoreThread's
    // essential queue, so QueueFull is not an expected failure reason for
    // lifecycle/native/error/capture-terminal truth or exact capture image facts.
    account_command_drop(r);

    if (has_fail_frame) {
      on_frame_ingress_failed_(frame_stream_id);
      account_frame_drop_and_release(r, fail_frame);
      if (is_capture_critical_frame) {
        if (r == CoreThread::PostResult::QueueFull) {
          signal_transport_failure_(ProviderTransportFailure::CoreEssentialQueueFull);
        } else if (r == CoreThread::PostResult::AllocFail) {
          signal_transport_failure_(ProviderTransportFailure::CoreEssentialAllocFail);
        } else if (r == CoreThread::PostResult::Closed && signal_closed_as_transport_failure) {
          signal_transport_failure_(ProviderTransportFailure::AuthoritativeAdmissionClosed);
        }
      }
    } else {
      if (r == CoreThread::PostResult::QueueFull) {
        signal_transport_failure_(ProviderTransportFailure::CoreEssentialQueueFull);
      } else if (r == CoreThread::PostResult::AllocFail) {
        signal_transport_failure_(ProviderTransportFailure::CoreEssentialAllocFail);
      } else if (r == CoreThread::PostResult::Closed && signal_closed_as_transport_failure) {
        signal_transport_failure_(ProviderTransportFailure::AuthoritativeAdmissionClosed);
      }
    }
  };

  const bool transport_accepting = transport_accepting_ ? transport_accepting_() : true;
  signal_closed_as_transport_failure =
      transport_accepting && (!has_fail_frame || is_capture_critical_frame);

  if (!core_thread_ || !transport_accepting) {
    account_post_failure(CoreThread::PostResult::Closed);
    return;
  }

  // NOTE: sink_ is copied into the posted lambda. This keeps ingress transport-pure
  // and avoids coupling to any specific dispatcher type.
  CoreThread::Task task;
  try {
    std::shared_ptr<std::atomic<bool>> frame_task_executed;
    if (has_fail_frame) {
      frame_task_executed = std::shared_ptr<std::atomic<bool>>(
          new std::atomic<bool>(false),
          [this, frame_stream_id](std::atomic<bool>* executed) {
            const bool was_executed =
                executed->load(std::memory_order_acquire);
            delete executed;
            if (!was_executed && frame_stream_id != 0) {
              on_frame_ingress_failed_(frame_stream_id);
            }
          });
    }
    task = [this,
            c = std::move(cmd),
            sink = sink_,
            frame_task_executed,
            frame_stream_id,
            release_dropped_frame,
            trace_capture_id,
            trace_device_id,
            trace_acquisition_session_id,
            trace_member_index,
            trace_type = type,
            is_capture_critical_frame]() mutable {
    (void)trace_capture_id;
    (void)trace_device_id;
    (void)trace_acquisition_session_id;
    (void)trace_member_index;
    (void)trace_type;
      if (c.type == ProviderToCoreCommandType::PROVIDER_FRAME) {
        on_frame_ingress_dispatched_(frame_stream_id);
        if (frame_task_executed) {
          frame_task_executed->store(true, std::memory_order_release);
        }
      }
      if (transport_accepting_ && !transport_accepting_()) {
        if (c.type == ProviderToCoreCommandType::PROVIDER_FRAME) {
          auto& p = std::get<CmdProviderFrame>(c.payload);
          release_dropped_frame(p.frame);
        }
        return;
      }
      PendingCommand pending(std::move(c));
      SinkResult sink_result = SinkResult::Rejected;
      bool sink_threw = false;
      try {
        if (sink) {
          sink_result = sink(pending);
        }
      } catch (...) {
        sink_threw = true;
      }

      if (!sink || (!pending.accepted() && sink_result == SinkResult::Rejected)) {
        if (pending.command().type == ProviderToCoreCommandType::PROVIDER_FRAME) {
          auto& p = std::get<CmdProviderFrame>(pending.command().payload);
          release_dropped_frame(p.frame);
        }
        return;
      }

      if (sink_threw) {
        if (!pending.accepted() &&
            pending.command().type == ProviderToCoreCommandType::PROVIDER_FRAME) {
          auto& p = std::get<CmdProviderFrame>(pending.command().payload);
          release_dropped_frame(p.frame);
        }
        signal_transport_failure_(ProviderTransportFailure::CallbackException);
        if (!pending.accepted() &&
            pending.command().type == ProviderToCoreCommandType::PROVIDER_FRAME &&
            !is_capture_critical_frame) {
          frames_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
          frames_released_on_drop_allocfail_.fetch_add(1, std::memory_order_relaxed);
        }
        return;
      }

      if (!pending.accepted()) {
        if (pending.command().type == ProviderToCoreCommandType::PROVIDER_FRAME) {
          auto& p = std::get<CmdProviderFrame>(pending.command().payload);
          release_dropped_frame(p.frame);
        }
        return;
      }

      if (sink_result == SinkResult::AcceptedWithFailure) {
        signal_transport_failure_(ProviderTransportFailure::CallbackException);
      }
    };
  } catch (...) {
    account_post_failure(CoreThread::PostResult::AllocFail);
    if (!has_fail_frame || is_capture_critical_frame) {
      signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
    }
    return;
  }

  const CoreThread::PostResult r = (is_frame_command_(type) && !is_capture_critical_frame)
      ? core_thread_->try_post(std::move(task))
      : core_thread_->try_post_essential(std::move(task));

  if (r == CoreThread::PostResult::Enqueued) {
    return;
  }

  account_post_failure(r);
}

void ProviderCallbackIngress::on_device_opened(uint64_t device_instance_id) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_DEVICE_OPENED;
    cmd.payload = CmdProviderDeviceOpened{device_instance_id};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_device_closed(uint64_t device_instance_id) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_DEVICE_CLOSED;
    cmd.payload = CmdProviderDeviceClosed{device_instance_id};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_stream_created(uint64_t stream_id) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_STREAM_CREATED;
    cmd.payload = CmdProviderStreamCreated{stream_id};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_stream_destroyed(uint64_t stream_id) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_STREAM_DESTROYED;
    cmd.payload = CmdProviderStreamDestroyed{stream_id};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_stream_started(uint64_t stream_id) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_STREAM_STARTED;
    cmd.payload = CmdProviderStreamStarted{stream_id};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_stream_stopped(uint64_t stream_id, ProviderError error_or_ok) {
  if (error_or_ok != ProviderError::OK) {
    timeline_teardown_trace_emit("fail StopStream stream_id=%llu reason=provider_error_%u",
                                 static_cast<unsigned long long>(stream_id),
                                 static_cast<unsigned>(error_or_ok));
  }
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_STREAM_STOPPED;
    cmd.payload = CmdProviderStreamStopped{stream_id, static_cast<uint32_t>(error_or_ok)};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_capture_started(uint64_t capture_id, uint64_t device_instance_id) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_CAPTURE_STARTED;
    cmd.payload = CmdProviderCaptureStarted{capture_id, device_instance_id};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_capture_completed(uint64_t capture_id, uint64_t device_instance_id) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_CAPTURE_COMPLETED;
    cmd.payload = CmdProviderCaptureCompleted{capture_id, device_instance_id};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_capture_failed(uint64_t capture_id,
                                                uint64_t device_instance_id,
                                                ProviderError error) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_CAPTURE_FAILED;
    cmd.payload = CmdProviderCaptureFailed{capture_id, device_instance_id, static_cast<uint32_t>(error)};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_camera_static_facts(
    uint64_t device_instance_id, ProviderCameraFacts facts) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_CAMERA_STATIC_FACTS;
    cmd.payload = CmdProviderCameraStaticFacts{device_instance_id, std::move(facts)};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_capture_image_facts(
    uint64_t capture_id,
    uint64_t device_instance_id,
    uint32_t image_member_index,
    ProviderCaptureImageFacts facts) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_CAPTURE_IMAGE_FACTS;
    cmd.payload = CmdProviderCaptureImageFacts{
        capture_id, device_instance_id, image_member_index, std::move(facts)};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_frame(const FrameView& frame) {
  // FrameView is a provider-owned view. Ownership is returned to the provider only when
  // core calls frame.release_now(). The core dispatcher MUST ensure release-on-drop.
  const bool authoritative = is_authoritative_frame_(frame);
  try {
    (void)on_frame_ingress_enqueued_(frame.stream_id);
    FrameView owned_frame = frame;
    adopt_singular_frame_release_owner(owned_frame);
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_FRAME;
    cmd.payload = CmdProviderFrame{std::move(owned_frame)};
    post_command(std::move(cmd));
  } catch (...) {
    on_frame_ingress_failed_(frame.stream_id);
    FrameView owned_frame = frame;
    (void)release_owned_frame_once(owned_frame, [this]() noexcept {
      signal_transport_failure_(ProviderTransportFailure::CallbackException);
    });
    if (authoritative) {
      commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      frames_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      frames_released_on_drop_allocfail_.fetch_add(1, std::memory_order_relaxed);
      signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
    } else {
      commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      frames_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
      frames_released_on_drop_allocfail_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void ProviderCallbackIngress::on_device_error(uint64_t device_instance_id, ProviderError error) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_DEVICE_ERROR;
    cmd.payload = CmdProviderDeviceError{device_instance_id, static_cast<uint32_t>(error)};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_stream_error(uint64_t stream_id, ProviderError error) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_STREAM_ERROR;
    cmd.payload = CmdProviderStreamError{stream_id, static_cast<uint32_t>(error)};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_native_object_created(const NativeObjectCreateInfo& info) {
  try {
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
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void ProviderCallbackIngress::on_native_object_destroyed(const NativeObjectDestroyInfo& info) {
  try {
    ProviderToCoreCommand cmd;
    cmd.type = ProviderToCoreCommandType::PROVIDER_NATIVE_OBJECT_DESTROYED;
    cmd.payload = CmdProviderNativeObjectDestroyed{info.native_id, info.has_destroyed_ns, info.destroyed_ns};
    post_command(std::move(cmd));
  } catch (...) {
    commands_dropped_allocfail_.fetch_add(1, std::memory_order_relaxed);
    non_frame_rejected_allocfail_.fetch_add(1, std::memory_order_relaxed);
    signal_transport_failure_(ProviderTransportFailure::CallbackAllocFail);
  }
}

} // namespace cambang
