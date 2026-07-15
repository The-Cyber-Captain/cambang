#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <variant>

#include "imaging/api/icamera_provider.h"

namespace cambang {

// Internal utility implementing the "single serialized provider callback context" rule.
//
// Providers MUST route all Provider→core facts (IProviderCallbacks::on_*) through this strand.
// Core-issued sync services (allocate_native_id, core_monotonic_now_ns) remain direct calls.
class CBProviderStrand final {
public:
  enum class EventClass : uint8_t {
    Lifecycle,
    NativeObject,
    Error,
    Frame,
  };

  CBProviderStrand() = default;
  ~CBProviderStrand();

  CBProviderStrand(const CBProviderStrand&) = delete;
  CBProviderStrand& operator=(const CBProviderStrand&) = delete;

  bool start(IProviderCallbacks* callbacks, const char* debug_name = "provider_strand", size_t capacity = 4096) noexcept;

  // Deterministic barrier: all events posted before flush() are guaranteed delivered before it returns.
  bool flush();

  // Stop the worker and drop any remaining queued events deterministically.
  void stop();

  bool running() const noexcept { return running_.load(std::memory_order_acquire); }

#if defined(CAMBANG_INTERNAL_SMOKE)
  void smoke_set_flush_timeout_ms(uint32_t timeout_ms) noexcept;
  void smoke_fail_start_once() noexcept;
  void smoke_break_next_barrier_once() noexcept;
#endif

  // ---- Fact posting helpers ----
  void post_device_opened(uint64_t device_instance_id);
  void post_device_closed(uint64_t device_instance_id);

  void post_stream_created(uint64_t stream_id);
  void post_stream_destroyed(uint64_t stream_id);
  void post_stream_started(uint64_t stream_id);
  void post_stream_stopped(uint64_t stream_id, ProviderError error_or_ok);

  void post_capture_started(uint64_t capture_id, uint64_t device_instance_id);
  void post_capture_completed(uint64_t capture_id, uint64_t device_instance_id);
  void post_capture_failed(uint64_t capture_id, uint64_t device_instance_id, ProviderError error);
  void post_camera_static_facts(uint64_t device_instance_id, ProviderCameraFacts facts);
  void post_capture_image_facts(uint64_t capture_id,
                                uint64_t device_instance_id,
                                uint32_t image_member_index,
                                ProviderCaptureImageFacts facts);

  void post_frame(const FrameView& frame);

  void post_device_error(uint64_t device_instance_id, ProviderError error);
  void post_stream_error(uint64_t stream_id, ProviderError error);

  void post_native_object_created(const NativeObjectCreateInfo& info);
  void post_native_object_destroyed(const NativeObjectDestroyInfo& info);

private:
  struct EvDeviceOpened { uint64_t id; };
  struct EvDeviceClosed { uint64_t id; };

  struct EvStreamCreated { uint64_t id; };
  struct EvStreamDestroyed { uint64_t id; };
  struct EvStreamStarted { uint64_t id; };
  struct EvStreamStopped { uint64_t id; ProviderError err; };

  struct EvCaptureStarted { uint64_t id; uint64_t device_instance_id; uint64_t queued_ns; };
  struct EvCaptureCompleted { uint64_t id; uint64_t device_instance_id; uint64_t queued_ns; };
  struct EvCaptureFailed { uint64_t id; uint64_t device_instance_id; ProviderError err; uint64_t queued_ns; };
  struct EvCameraStaticFacts { uint64_t device_instance_id; ProviderCameraFacts facts; };
  struct EvCaptureImageFacts {
    uint64_t capture_id;
    uint64_t device_instance_id;
    uint32_t image_member_index;
    ProviderCaptureImageFacts facts;
  };

  struct EvFrame { FrameView frame; uint64_t queued_ns; };

  struct EvDeviceError { uint64_t id; ProviderError err; };
  struct EvStreamError { uint64_t id; ProviderError err; };

  struct EvNativeCreated { NativeObjectCreateInfo info; };
  struct EvNativeDestroyed { NativeObjectDestroyInfo info; };

  struct EvBarrier { std::shared_ptr<std::promise<bool>> done; };

  enum class FlushResult : uint8_t {
    Completed = 0,
    Failed,
    TimedOut,
  };

  using Event = std::variant<
      EvDeviceOpened,
      EvDeviceClosed,
      EvStreamCreated,
      EvStreamDestroyed,
      EvStreamStarted,
      EvStreamStopped,
      EvCaptureStarted,
      EvCaptureCompleted,
      EvCaptureFailed,
      EvCameraStaticFacts,
      EvCaptureImageFacts,
      EvFrame,
      EvDeviceError,
      EvStreamError,
      EvNativeCreated,
      EvNativeDestroyed,
      EvBarrier>;

  void post(Event ev);
  static EventClass classify_(const Event& ev);
  void thread_main_();
  void deliver_(Event& ev);
  void drop_(Event& ev);
  void set_barrier_result_(EvBarrier& barrier, bool ok) noexcept;
  FlushResult flush_result_() noexcept;
  void notify_fatal_(ProviderTransportFailure reason) noexcept;
  bool admission_closed_unsafe_() const noexcept;
  std::chrono::milliseconds flush_timeout_() const noexcept;

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Event> q_;
  size_t capacity_ = 0;

  IProviderCallbacks* callbacks_ = nullptr;
  const char* debug_name_ = nullptr;

  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<uint8_t> fatal_reason_{static_cast<uint8_t>(ProviderTransportFailure::None)};
  std::thread worker_;
#if defined(CAMBANG_INTERNAL_SMOKE)
  std::atomic<uint32_t> smoke_flush_timeout_ms_{0};
  std::atomic<bool> smoke_fail_start_once_{false};
  std::atomic<bool> smoke_break_next_barrier_once_{false};
#endif
};

} // namespace cambang
