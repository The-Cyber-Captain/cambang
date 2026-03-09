#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "core/core_mailbox.h"
#include "core/core_thread.h"
#include "imaging/api/icamera_provider.h"

namespace cambang {

// ProviderCallbackIngress is the ONLY allowed implementation point where provider
// callbacks cross into core execution.
//
// Invariants:
// - Provider threads MUST NOT touch core state directly.
// - Every callback is marshalled into a CoreCommand and posted onto CoreThread.
// - No business logic exists here; this is a transport adapter only.
//
// Mailbox hardening (Build slice C):
// - CoreThread posting is best-effort.
// - If posting fails and the command contains a FrameView, ingress MUST release it
//   (release-on-drop) to avoid leaks.
class ProviderCallbackIngress final : public IProviderCallbacks {
public:
  struct Stats {
    uint64_t commands_dropped_full = 0;
    uint64_t commands_dropped_closed = 0;
    uint64_t commands_dropped_allocfail = 0;

    uint64_t frames_dropped_full = 0;
    uint64_t frames_dropped_closed = 0;
    uint64_t frames_dropped_allocfail = 0;

    uint64_t frames_released_on_drop_full = 0;
    uint64_t frames_released_on_drop_closed = 0;
    uint64_t frames_released_on_drop_allocfail = 0;
  };

  // sink is invoked ONLY on the core thread.
  // It is responsible for consuming the CoreCommand (e.g., dispatching).
  ProviderCallbackIngress(CoreThread* core_thread,
                          std::function<void(CoreCommand&&)> sink,
                          std::function<uint64_t()> core_monotonic_now_ns);
  ~ProviderCallbackIngress() override = default;

  ProviderCallbackIngress(const ProviderCallbackIngress&) = delete;
  ProviderCallbackIngress& operator=(const ProviderCallbackIngress&) = delete;

  Stats stats_copy() const noexcept;

  // IProviderCallbacks (core-issued services)
  uint64_t allocate_native_id(NativeObjectType type) override;
  uint64_t core_monotonic_now_ns() override;

  // IProviderCallbacks
  void on_device_opened(uint64_t device_instance_id) override;
  void on_device_closed(uint64_t device_instance_id) override;

  void on_stream_created(uint64_t stream_id) override;
  void on_stream_destroyed(uint64_t stream_id) override;
  void on_stream_started(uint64_t stream_id) override;
  void on_stream_stopped(uint64_t stream_id, ProviderError error_or_ok) override;

  void on_capture_started(uint64_t capture_id) override;
  void on_capture_completed(uint64_t capture_id) override;
  void on_capture_failed(uint64_t capture_id, ProviderError error) override;

  void on_frame(const FrameView& frame) override;

  void on_device_error(uint64_t device_instance_id, ProviderError error) override;
  void on_stream_error(uint64_t stream_id, ProviderError error) override;

  void on_native_object_created(const NativeObjectCreateInfo& info) override;
  void on_native_object_destroyed(const NativeObjectDestroyInfo& info) override;

private:
  uint32_t on_frame_ingress_enqueued_(uint64_t stream_id);
  void on_frame_ingress_failed_(uint64_t stream_id);
  void on_frame_ingress_dispatched_(uint64_t stream_id);

  void post_command(CoreCommand cmd);

  CoreThread* core_thread_ = nullptr; // non-owning
  std::function<void(CoreCommand&&)> sink_;
  std::function<uint64_t()> core_monotonic_now_ns_;

  std::atomic<uint64_t> native_id_seq_{1};

  std::atomic<uint64_t> commands_dropped_full_{0};
  std::atomic<uint64_t> commands_dropped_closed_{0};
  std::atomic<uint64_t> commands_dropped_allocfail_{0};

  std::atomic<uint64_t> frames_dropped_full_{0};
  std::atomic<uint64_t> frames_dropped_closed_{0};
  std::atomic<uint64_t> frames_dropped_allocfail_{0};

  std::atomic<uint64_t> frames_released_on_drop_full_{0};
  std::atomic<uint64_t> frames_released_on_drop_closed_{0};
  std::atomic<uint64_t> frames_released_on_drop_allocfail_{0};

  std::mutex ingress_mu_;
  std::unordered_map<uint64_t, uint32_t> stream_ingress_depth_;
};

} // namespace cambang
