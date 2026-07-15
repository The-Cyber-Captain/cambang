#include "imaging/api/provider_strand.h"

#include <cassert>
#include <chrono>

#include "imaging/api/frame_release_utils.h"

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

} // namespace

CBProviderStrand::~CBProviderStrand() { stop(); }

bool CBProviderStrand::start(IProviderCallbacks* callbacks, const char* debug_name, size_t capacity) noexcept {
  if (running()) {
    return true;
  }
  callbacks_ = callbacks;
  debug_name_ = debug_name;
  capacity_ = capacity;
  stop_requested_.store(false, std::memory_order_release);
  fatal_reason_.store(static_cast<uint8_t>(ProviderTransportFailure::None), std::memory_order_release);
  running_.store(true, std::memory_order_release);
  try {
#if defined(CAMBANG_INTERNAL_SMOKE)
    if (smoke_fail_start_once_.exchange(false, std::memory_order_acq_rel)) {
      throw std::runtime_error("CBProviderStrand smoke start failure");
    }
#endif
    worker_ = std::thread([this]() {
      try {
        thread_main_();
      } catch (...) {
        notify_fatal_(ProviderTransportFailure::CallbackException);
      }
    });
  } catch (...) {
    callbacks_ = nullptr;
    debug_name_ = nullptr;
    capacity_ = 0;
    stop_requested_.store(false, std::memory_order_release);
    fatal_reason_.store(static_cast<uint8_t>(ProviderTransportFailure::None), std::memory_order_release);
    running_.store(false, std::memory_order_release);
    return false;
  }
  return true;
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

bool CBProviderStrand::flush() { return flush_result_() == FlushResult::Completed; }

CBProviderStrand::FlushResult CBProviderStrand::flush_result_() noexcept {
  if (!running()) {
    return FlushResult::Failed;
  }
  std::shared_ptr<std::promise<bool>> p;
  std::future<bool> f;
  try {
    p = std::make_shared<std::promise<bool>>();
    f = p->get_future();
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
    return FlushResult::Failed;
  }
  const auto timeout = flush_timeout_();
  {
    std::unique_lock<std::mutex> lk(mu_);
    if (!cv_.wait_for(lk, timeout, [this]() {
          return admission_closed_unsafe_() || capacity_ == 0 || q_.size() < capacity_;
        })) {
      return FlushResult::TimedOut;
    }
    if (admission_closed_unsafe_()) {
      return FlushResult::Failed;
    }
    try {
      q_.push_back(EvBarrier{std::move(p)});
    } catch (...) {
      lk.unlock();
      notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
      return FlushResult::Failed;
    }
  }
  cv_.notify_one();
  try {
    if (f.wait_for(timeout) != std::future_status::ready) {
      return FlushResult::TimedOut;
    }
    return f.get() ? FlushResult::Completed : FlushResult::Failed;
  } catch (...) {
    return FlushResult::Failed;
  }
}

void CBProviderStrand::post(Event ev) {
  if (!running() || stop_requested_.load(std::memory_order_acquire) ||
      fatal_reason_.load(std::memory_order_acquire) !=
          static_cast<uint8_t>(ProviderTransportFailure::None)) {
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
    while (capacity_ > 0 && q_.size() >= capacity_) {
      bool reclaimed = false;
      for (auto it = q_.begin(); it != q_.end(); ++it) {
        if (classify_(*it) == EventClass::Frame) {
          Event dropped = std::move(*it);
          q_.erase(it);
          lk.unlock();
          drop_(dropped);
          lk.lock();
          reclaimed = true;
          break;
        }
      }
      if (!reclaimed) {
        lk.unlock();
        notify_fatal_(ProviderTransportFailure::AuthoritativeQueueFull);
        drop_(ev);
        return;
      }
    }
  }
  try {
    q_.push_back(std::move(ev));
  } catch (...) {
    lk.unlock();
    if (cls == EventClass::Frame) {
      drop_(ev);
    } else {
      notify_fatal_(ProviderTransportFailure::AuthoritativeAllocFail);
      drop_(ev);
    }
    return;
  }
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
  try {
    while (true) {
      Event ev;
      bool fatal = false;
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
        fatal = fatal_reason_.load(std::memory_order_acquire) !=
            static_cast<uint8_t>(ProviderTransportFailure::None);
        cv_.notify_all();
      }

      if (fatal) {
        drop_(ev);
        continue;
      }

      deliver_(ev);
    }
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackException);
  }
}

void CBProviderStrand::deliver_(Event& ev) {
  if (!callbacks_) {
    drop_(ev);
    return;
  }

  try {
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
            set_barrier_result_(e, true);
          } else {
            static_assert(sizeof(T) == 0, "Unhandled CBProviderStrand event");
          }
        },
        ev);
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackException);
    if (!std::holds_alternative<EvFrame>(ev)) {
      drop_(ev);
    }
  }
}

void CBProviderStrand::drop_(Event& ev) {
  // Only frames require deterministic release.
  std::visit(
      [&](auto& e) {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, EvFrame>) {
          (void)release_owned_frame_once(e.frame, [this]() noexcept {
            notify_fatal_(ProviderTransportFailure::CallbackException);
          });
        } else if constexpr (std::is_same_v<T, EvBarrier>) {
          set_barrier_result_(e, false);
        }
      },
      ev);
}

void CBProviderStrand::set_barrier_result_(EvBarrier& barrier, bool ok) noexcept {
  if (!barrier.done) {
    return;
  }
#if defined(CAMBANG_INTERNAL_SMOKE)
  if (smoke_break_next_barrier_once_.exchange(false, std::memory_order_acq_rel)) {
    barrier.done.reset();
    return;
  }
#endif
  try {
    barrier.done->set_value(ok);
  } catch (...) {
  }
}

void CBProviderStrand::notify_fatal_(ProviderTransportFailure reason) noexcept {
  uint8_t expected = static_cast<uint8_t>(ProviderTransportFailure::None);
  if (!fatal_reason_.compare_exchange_strong(
          expected,
          static_cast<uint8_t>(reason),
          std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return;
  }
  stop_requested_.store(true, std::memory_order_release);
  IProviderCallbacks* callbacks = callbacks_;
  if (callbacks) {
    try {
      callbacks->on_transport_failure(reason);
    } catch (...) {
    }
  }
  cv_.notify_all();
}

bool CBProviderStrand::admission_closed_unsafe_() const noexcept {
  return stop_requested_.load(std::memory_order_acquire) ||
         fatal_reason_.load(std::memory_order_acquire) !=
             static_cast<uint8_t>(ProviderTransportFailure::None);
}

std::chrono::milliseconds CBProviderStrand::flush_timeout_() const noexcept {
#if defined(CAMBANG_INTERNAL_SMOKE)
  const uint32_t smoke_timeout_ms =
      smoke_flush_timeout_ms_.load(std::memory_order_acquire);
  if (smoke_timeout_ms != 0) {
    return std::chrono::milliseconds(smoke_timeout_ms);
  }
#endif
  return std::chrono::seconds(2);
}

#if defined(CAMBANG_INTERNAL_SMOKE)
void CBProviderStrand::smoke_set_flush_timeout_ms(uint32_t timeout_ms) noexcept {
  smoke_flush_timeout_ms_.store(timeout_ms, std::memory_order_release);
}

void CBProviderStrand::smoke_fail_start_once() noexcept {
  smoke_fail_start_once_.store(true, std::memory_order_release);
}

void CBProviderStrand::smoke_break_next_barrier_once() noexcept {
  smoke_break_next_barrier_once_.store(true, std::memory_order_release);
}
#endif

// ---- helpers ----

void CBProviderStrand::post_device_opened(uint64_t device_instance_id) {
  try {
    post(EvDeviceOpened{device_instance_id});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}
void CBProviderStrand::post_device_closed(uint64_t device_instance_id) {
  try {
    post(EvDeviceClosed{device_instance_id});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void CBProviderStrand::post_stream_created(uint64_t stream_id) {
  try {
    post(EvStreamCreated{stream_id});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}
void CBProviderStrand::post_stream_destroyed(uint64_t stream_id) {
  try {
    post(EvStreamDestroyed{stream_id});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}
void CBProviderStrand::post_stream_started(uint64_t stream_id) {
  try {
    post(EvStreamStarted{stream_id});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}
void CBProviderStrand::post_stream_stopped(uint64_t stream_id, ProviderError error_or_ok) {
  try {
    post(EvStreamStopped{stream_id, error_or_ok});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void CBProviderStrand::post_capture_started(uint64_t capture_id, uint64_t device_instance_id) {
  try {
    post(EvCaptureStarted{capture_id, device_instance_id, capture_latency_trace_now_ns()});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}
void CBProviderStrand::post_capture_completed(uint64_t capture_id, uint64_t device_instance_id) {
  try {
    post(EvCaptureCompleted{capture_id, device_instance_id, capture_latency_trace_now_ns()});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}
void CBProviderStrand::post_capture_failed(uint64_t capture_id,
                                           uint64_t device_instance_id,
                                           ProviderError error) {
  try {
    post(EvCaptureFailed{capture_id, device_instance_id, error, capture_latency_trace_now_ns()});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void CBProviderStrand::post_camera_static_facts(
    uint64_t device_instance_id, ProviderCameraFacts facts) {
  try {
    post(EvCameraStaticFacts{device_instance_id, std::move(facts)});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void CBProviderStrand::post_capture_image_facts(
    uint64_t capture_id,
    uint64_t device_instance_id,
    uint32_t image_member_index,
    ProviderCaptureImageFacts facts) {
  try {
    post(EvCaptureImageFacts{
        capture_id, device_instance_id, image_member_index, std::move(facts)});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void CBProviderStrand::post_frame(const FrameView& frame) {
  try {
    FrameView owned = frame;
    adopt_singular_frame_release_owner(owned);
    post(EvFrame{std::move(owned), capture_latency_trace_now_ns()});
  } catch (...) {
    FrameView owned = frame;
    (void)release_owned_frame_once(owned, [this]() noexcept {
      notify_fatal_(ProviderTransportFailure::CallbackException);
    });
  }
}

void CBProviderStrand::post_device_error(uint64_t device_instance_id, ProviderError error) {
  try {
    post(EvDeviceError{device_instance_id, error});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}
void CBProviderStrand::post_stream_error(uint64_t stream_id, ProviderError error) {
  try {
    post(EvStreamError{stream_id, error});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}

void CBProviderStrand::post_native_object_created(const NativeObjectCreateInfo& info) {
  try {
    post(EvNativeCreated{info});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}
void CBProviderStrand::post_native_object_destroyed(const NativeObjectDestroyInfo& info) {
  try {
    post(EvNativeDestroyed{info});
  } catch (...) {
    notify_fatal_(ProviderTransportFailure::CallbackAllocFail);
  }
}

} // namespace cambang
