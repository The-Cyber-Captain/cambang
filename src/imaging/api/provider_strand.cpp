#include "imaging/api/provider_strand.h"

#include <cassert>
#include <chrono>
#include <cstdarg>
#include <cstdio>

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

// BEGIN TEMPORARY CAPTURE LATENCY DIAGNOSTICS
uint64_t capture_latency_trace_now_ns() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

void capture_latency_trace_printf(const char* format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  capture_latency_trace_diagnostics::print_line(buffer);
}
// END TEMPORARY CAPTURE LATENCY DIAGNOSTICS

} // namespace

CBProviderStrand::~CBProviderStrand() { stop(); }

void CBProviderStrand::start(IProviderCallbacks* callbacks, const char* debug_name, size_t capacity) {
  if (running()) {
    return;
  }
  callbacks_ = callbacks;
  debug_name_ = debug_name;
  capacity_ = capacity;
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  worker_ = std::thread([this]() { thread_main_(); });
}

void CBProviderStrand::stop() {
  if (!running()) {
    return;
  }

  stop_requested_.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(mu_);
    cv_.notify_all();
  }

  if (worker_.joinable()) {
    worker_.join();
  }

  // Any residual queued events are dropped deterministically (release frames).
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& ev : q_) {
      drop_(ev);
    }
    q_.clear();
  }

  callbacks_ = nullptr;
  running_.store(false, std::memory_order_release);
}

void CBProviderStrand::flush() {
  if (!running()) {
    return;
  }
  auto p = std::make_shared<std::promise<void>>();
  auto f = p->get_future();
  post(EvBarrier{p});
  f.wait();
}

void CBProviderStrand::post(Event ev) {
  if (!running() || stop_requested_.load(std::memory_order_acquire)) {
    drop_(ev);
    return;
  }

  std::unique_lock<std::mutex> lk(mu_);
  const EventClass cls = classify_(ev);
  if (capacity_ > 0 && q_.size() >= capacity_) {
    if (cls == EventClass::Frame) {
      // Deterministic backpressure: repeating stream frames are droppable.
      lk.unlock();
      drop_(ev);
      return;
    }

    // Non-lossy classes must not be silently dropped once admitted.
    // Prefer reclaiming space from an already-queued frame.
    for (auto it = q_.begin(); it != q_.end(); ++it) {
      if (classify_(*it) == EventClass::Frame) {
        Event dropped = std::move(*it);
        q_.erase(it);
        lk.unlock();
        drop_(dropped);
        lk.lock();
        break;
      }
    }
  }
  q_.push_back(std::move(ev));
  lk.unlock();
  cv_.notify_one();
}

CBProviderStrand::EventClass CBProviderStrand::classify_(const Event& ev) {
  return std::visit(
      [](const auto& e) -> EventClass {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, EvFrame>) {
          // Still-capture frames are exact capture facts and must stay ordered
          // with terminal capture lifecycle facts; only repeating stream frames
          // (capture_id == 0) are latest-state/droppable frame work.
          return e.frame.capture_id != 0 ? EventClass::Lifecycle : EventClass::Frame;
        } else if constexpr (std::is_same_v<T, EvNativeCreated> || std::is_same_v<T, EvNativeDestroyed>) {
          return EventClass::NativeObject;
        } else if constexpr (std::is_same_v<T, EvDeviceError> || std::is_same_v<T, EvStreamError>) {
          return EventClass::Error;
        } else {
          return EventClass::Lifecycle;
        }
      },
      ev);
}

void CBProviderStrand::thread_main_() {
  while (true) {
    Event ev;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [&]() {
        return stop_requested_.load(std::memory_order_acquire) || !q_.empty();
      });

      if (stop_requested_.load(std::memory_order_acquire) && q_.empty()) {
        break;
      }

      ev = std::move(q_.front());
      q_.pop_front();
    }

    deliver_(ev);
  }
}

void CBProviderStrand::deliver_(Event& ev) {
  if (!callbacks_) {
    drop_(ev);
    return;
  }

  std::visit(
      [&](auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, EvDeviceOpened>) {
          callbacks_->on_device_opened(e.id);
        } else if constexpr (std::is_same_v<T, EvDeviceClosed>) {
          callbacks_->on_device_closed(e.id);
        } else if constexpr (std::is_same_v<T, EvStreamCreated>) {
          callbacks_->on_stream_created(e.id);
        } else if constexpr (std::is_same_v<T, EvStreamDestroyed>) {
          callbacks_->on_stream_destroyed(e.id);
        } else if constexpr (std::is_same_v<T, EvStreamStarted>) {
          callbacks_->on_stream_started(e.id);
        } else if constexpr (std::is_same_v<T, EvStreamStopped>) {
          callbacks_->on_stream_stopped(e.id, e.err);
        } else if constexpr (std::is_same_v<T, EvCaptureStarted>) {
          const uint64_t deliver_begin_ns = capture_latency_trace_now_ns();
          callbacks_->on_capture_started(e.id, e.device_instance_id);
          const uint64_t deliver_end_ns = capture_latency_trace_now_ns();
          capture_latency_trace_printf(
              "strand_deliver_capture_started capture_id=%llu device_id=%llu fact_type=started queue_delay_us=%llu callback_us=%llu",
              static_cast<unsigned long long>(e.id),
              static_cast<unsigned long long>(e.device_instance_id),
              static_cast<unsigned long long>((deliver_begin_ns - e.queued_ns) / 1000ull),
              static_cast<unsigned long long>((deliver_end_ns - deliver_begin_ns) / 1000ull));
        } else if constexpr (std::is_same_v<T, EvCaptureCompleted>) {
          const uint64_t deliver_begin_ns = capture_latency_trace_now_ns();
          callbacks_->on_capture_completed(e.id, e.device_instance_id);
          const uint64_t deliver_end_ns = capture_latency_trace_now_ns();
          capture_latency_trace_printf(
              "strand_deliver_capture_completed capture_id=%llu device_id=%llu fact_type=completed queue_delay_us=%llu callback_us=%llu",
              static_cast<unsigned long long>(e.id),
              static_cast<unsigned long long>(e.device_instance_id),
              static_cast<unsigned long long>((deliver_begin_ns - e.queued_ns) / 1000ull),
              static_cast<unsigned long long>((deliver_end_ns - deliver_begin_ns) / 1000ull));
        } else if constexpr (std::is_same_v<T, EvCaptureFailed>) {
          const uint64_t deliver_begin_ns = capture_latency_trace_now_ns();
          callbacks_->on_capture_failed(e.id, e.device_instance_id, e.err);
          const uint64_t deliver_end_ns = capture_latency_trace_now_ns();
          capture_latency_trace_printf(
              "strand_deliver_capture_failed capture_id=%llu device_id=%llu fact_type=failed queue_delay_us=%llu callback_us=%llu err=%u",
              static_cast<unsigned long long>(e.id),
              static_cast<unsigned long long>(e.device_instance_id),
              static_cast<unsigned long long>((deliver_begin_ns - e.queued_ns) / 1000ull),
              static_cast<unsigned long long>((deliver_end_ns - deliver_begin_ns) / 1000ull),
              static_cast<unsigned>(e.err));
        } else if constexpr (std::is_same_v<T, EvFrame>) {
          const uint64_t deliver_begin_ns = capture_latency_trace_now_ns();
          callbacks_->on_frame(e.frame);
          const uint64_t deliver_end_ns = capture_latency_trace_now_ns();
          if (e.frame.capture_id != 0) {
            capture_latency_trace_printf(
                "strand_deliver_capture_frame capture_id=%llu device_id=%llu acquisition_session_id=%llu member=%u fact_type=frame queue_delay_us=%llu callback_us=%llu bytes=%llu",
                static_cast<unsigned long long>(e.frame.capture_id),
                static_cast<unsigned long long>(e.frame.device_instance_id),
                static_cast<unsigned long long>(e.frame.acquisition_session_id),
                static_cast<unsigned>(e.frame.capture_image.image_member_index),
                static_cast<unsigned long long>((deliver_begin_ns - e.queued_ns) / 1000ull),
                static_cast<unsigned long long>((deliver_end_ns - deliver_begin_ns) / 1000ull),
                static_cast<unsigned long long>(e.frame.size_bytes));
          }
        } else if constexpr (std::is_same_v<T, EvDeviceError>) {
          callbacks_->on_device_error(e.id, e.err);
        } else if constexpr (std::is_same_v<T, EvStreamError>) {
          callbacks_->on_stream_error(e.id, e.err);
        } else if constexpr (std::is_same_v<T, EvNativeCreated>) {
          callbacks_->on_native_object_created(e.info);
        } else if constexpr (std::is_same_v<T, EvNativeDestroyed>) {
          callbacks_->on_native_object_destroyed(e.info);
        } else if constexpr (std::is_same_v<T, EvBarrier>) {
          e.done->set_value();
        } else {
          static_assert(sizeof(T) == 0, "Unhandled CBProviderStrand event");
        }
      },
      ev);
}

void CBProviderStrand::drop_(Event& ev) {
  // Only frames require deterministic release.
  std::visit(
      [&](auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, EvFrame>) {
          e.frame.release_now();
        } else if constexpr (std::is_same_v<T, EvBarrier>) {
          // If we drop a barrier, wake the waiter.
          e.done->set_value();
        }
      },
      ev);
}

// ---- helpers ----

void CBProviderStrand::post_device_opened(uint64_t device_instance_id) { post(EvDeviceOpened{device_instance_id}); }
void CBProviderStrand::post_device_closed(uint64_t device_instance_id) { post(EvDeviceClosed{device_instance_id}); }

void CBProviderStrand::post_stream_created(uint64_t stream_id) { post(EvStreamCreated{stream_id}); }
void CBProviderStrand::post_stream_destroyed(uint64_t stream_id) { post(EvStreamDestroyed{stream_id}); }
void CBProviderStrand::post_stream_started(uint64_t stream_id) { post(EvStreamStarted{stream_id}); }
void CBProviderStrand::post_stream_stopped(uint64_t stream_id, ProviderError error_or_ok) {
  post(EvStreamStopped{stream_id, error_or_ok});
}

void CBProviderStrand::post_capture_started(uint64_t capture_id, uint64_t device_instance_id) {
  post(EvCaptureStarted{capture_id, device_instance_id, capture_latency_trace_now_ns()});
}
void CBProviderStrand::post_capture_completed(uint64_t capture_id, uint64_t device_instance_id) {
  post(EvCaptureCompleted{capture_id, device_instance_id, capture_latency_trace_now_ns()});
}
void CBProviderStrand::post_capture_failed(uint64_t capture_id,
                                           uint64_t device_instance_id,
                                           ProviderError error) {
  post(EvCaptureFailed{capture_id, device_instance_id, error, capture_latency_trace_now_ns()});
}

void CBProviderStrand::post_frame(const FrameView& frame) { post(EvFrame{frame, capture_latency_trace_now_ns()}); }

void CBProviderStrand::post_device_error(uint64_t device_instance_id, ProviderError error) {
  post(EvDeviceError{device_instance_id, error});
}
void CBProviderStrand::post_stream_error(uint64_t stream_id, ProviderError error) { post(EvStreamError{stream_id, error}); }

void CBProviderStrand::post_native_object_created(const NativeObjectCreateInfo& info) { post(EvNativeCreated{info}); }
void CBProviderStrand::post_native_object_destroyed(const NativeObjectDestroyInfo& info) { post(EvNativeDestroyed{info}); }

} // namespace cambang
