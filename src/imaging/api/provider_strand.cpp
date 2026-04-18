#include "imaging/api/provider_strand.h"

#include <cassert>

namespace cambang {

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
      // Deterministic backpressure: frames are droppable.
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
          return EventClass::Frame;
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
          callbacks_->on_capture_started(e.id, e.device_instance_id);
        } else if constexpr (std::is_same_v<T, EvCaptureCompleted>) {
          callbacks_->on_capture_completed(e.id, e.device_instance_id);
        } else if constexpr (std::is_same_v<T, EvCaptureFailed>) {
          callbacks_->on_capture_failed(e.id, e.device_instance_id, e.err);
        } else if constexpr (std::is_same_v<T, EvFrame>) {
          callbacks_->on_frame(e.frame);
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
  post(EvCaptureStarted{capture_id, device_instance_id});
}
void CBProviderStrand::post_capture_completed(uint64_t capture_id, uint64_t device_instance_id) {
  post(EvCaptureCompleted{capture_id, device_instance_id});
}
void CBProviderStrand::post_capture_failed(uint64_t capture_id,
                                           uint64_t device_instance_id,
                                           ProviderError error) {
  post(EvCaptureFailed{capture_id, device_instance_id, error});
}

void CBProviderStrand::post_frame(const FrameView& frame) { post(EvFrame{frame}); }

void CBProviderStrand::post_device_error(uint64_t device_instance_id, ProviderError error) {
  post(EvDeviceError{device_instance_id, error});
}
void CBProviderStrand::post_stream_error(uint64_t stream_id, ProviderError error) { post(EvStreamError{stream_id, error}); }

void CBProviderStrand::post_native_object_created(const NativeObjectCreateInfo& info) { post(EvNativeCreated{info}); }
void CBProviderStrand::post_native_object_destroyed(const NativeObjectDestroyInfo& info) { post(EvNativeDestroyed{info}); }

} // namespace cambang
