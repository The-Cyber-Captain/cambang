#include "imaging/api/provider_strand.h"

#include <cassert>
#include <cstdio>
#include <exception>
namespace cambang {

CBProviderStrand::~CBProviderStrand() { stop(); }

bool CBProviderStrand::start(
    IProviderCallbacks* callbacks,
    const char* debug_name,
    size_t capacity) noexcept try {
  if (!callbacks || running()) {
    return false;
  }
  callbacks_ = callbacks;
  debug_name_ = debug_name;
  capacity_ = capacity;
  {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = false;
  }
  stop_requested_.store(false, std::memory_order_release);
  try {
    worker_ = std::thread([this]() { thread_main_(); });
  } catch (...) {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = true;
    callbacks_ = nullptr;
    debug_name_ = nullptr;
    capacity_ = 0;
    return false;
  }
  running_.store(true, std::memory_order_release);
  return true;
} catch (...) {
  callbacks_ = nullptr;
  debug_name_ = nullptr;
  capacity_ = 0;
  running_.store(false, std::memory_order_release);
  return false;
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

  // Close admission and drain atomically under mu_. Setting closed_ here,
  // in the same critical section as the drain, closes the race where post()
  // could otherwise observe stop_requested_==false, fall through its
  // outside-the-lock fast-path check, and push an event after this drain has
  // already run — an event that would then never be delivered (the worker
  // thread has already exited above) and never accounted as dropped either.
  // post() re-checks closed_ under this same mutex before pushing, so the two
  // critical sections cannot interleave: any post() call is fully ordered
  // before or after this one.
  {
    std::lock_guard<std::mutex> lk(mu_);
    closed_ = true;
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
    // Best-effort fast-path exit for the common already-stopped/stopping
    // case; not the correctness guarantee. stop_requested_ is set outside
    // mu_, so this check alone cannot close the race with stop()'s drain
    // (see closed_ check below, which is authoritative).
    drop_(ev);
    return;
  }

  std::unique_lock<std::mutex> lk(mu_);
  if (closed_) {
    // Authoritative check: stop()'s drain has already run (or is running) in
    // the same critical section that sets closed_. Dropping here rather than
    // pushing keeps admission-close deterministic relative to drain.
    lk.unlock();
    drop_(ev);
    return;
  }
  const EventClass cls = classify_(ev);
  if (capacity_ > 0 && q_.size() >= capacity_) {
    if (cls == EventClass::Frame) {
      // Deterministic backpressure: repeating stream frames are droppable.
      lk.unlock();
      drop_(ev);
      return;
    }

    // Non-lossy classes must not be silently dropped once admitted
    // (docs/architecture/provider_strand_model.md: lifecycle/native-object/
    // error events "must never be dropped"). Prefer reclaiming space from an
    // already-queued frame; if none is available, admission proceeds past
    // capacity_ rather than violating the non-lossy contract.
    bool evicted_frame = false;
    for (auto it = q_.begin(); it != q_.end(); ++it) {
      if (classify_(*it) == EventClass::Frame) {
        Event dropped = std::move(*it);
        q_.erase(it);
        lk.unlock();
        drop_(dropped);
        lk.lock();
        evicted_frame = true;
        break;
      }
    }
    if (!evicted_frame) {
      // Diagnosable record of sustained non-lossy pressure exceeding
      // capacity_; this is the only visibility gap in otherwise-correct
      // non-lossy admission.
      non_lossy_over_capacity_count_.fetch_add(1, std::memory_order_relaxed);
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

    // deliver_() invokes arbitrary IProviderCallbacks virtual methods. An
    // uncaught exception escaping this thread's entry function is UB and
    // terminates the whole process, so it must not propagate past this point.
    try {
      deliver_(ev);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[CamBANG][CBProviderStrand] uncaught exception in deliver_: %s\n", e.what());
    } catch (...) {
      std::fprintf(stderr, "[CamBANG][CBProviderStrand] uncaught non-standard exception in deliver_\n");
    }
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
        } else if constexpr (std::is_same_v<T, EvCameraStaticFacts>) {
          callbacks_->on_camera_static_facts(e.device_instance_id, std::move(e.facts));
        } else if constexpr (std::is_same_v<T, EvCaptureImageFacts>) {
          callbacks_->on_capture_image_facts(
              e.capture_id,
              e.device_instance_id,
              e.image_member_index,
              std::move(e.facts));
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

void CBProviderStrand::post_camera_static_facts(
    uint64_t device_instance_id, ProviderCameraFacts facts) {
  post(EvCameraStaticFacts{device_instance_id, std::move(facts)});
}

void CBProviderStrand::post_capture_image_facts(
    uint64_t capture_id,
    uint64_t device_instance_id,
    uint32_t image_member_index,
    ProviderCaptureImageFacts facts) {
  post(EvCaptureImageFacts{
      capture_id, device_instance_id, image_member_index, std::move(facts)});
}

void CBProviderStrand::post_frame(const FrameView& frame) { post(EvFrame{frame}); }

void CBProviderStrand::post_device_error(uint64_t device_instance_id, ProviderError error) {
  post(EvDeviceError{device_instance_id, error});
}
void CBProviderStrand::post_stream_error(uint64_t stream_id, ProviderError error) { post(EvStreamError{stream_id, error}); }

void CBProviderStrand::post_native_object_created(const NativeObjectCreateInfo& info) { post(EvNativeCreated{info}); }
void CBProviderStrand::post_native_object_destroyed(const NativeObjectDestroyInfo& info) { post(EvNativeDestroyed{info}); }

} // namespace cambang
