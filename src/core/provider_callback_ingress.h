#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

#include "core/provider_to_core_commands.h"
#include "core/core_thread.h"
#include "imaging/api/icamera_provider.h"

namespace cambang {

// ProviderCallbackIngress is the ONLY allowed implementation point where provider
// callbacks cross into core execution.
//
// Invariants:
// - Provider threads MUST NOT touch core state directly.
// - Every callback is marshalled into a ProviderToCoreCommand and posted onto CoreThread.
// - No business logic exists here; this is a transport adapter only.
//
// Mailbox hardening (Build slice C):
// - CoreThread posting is best-effort.
// - If posting fails and the command contains a FrameView, ingress MUST release it
//   (release-on-drop) to avoid leaks.
class ProviderCallbackIngress final : public IProviderCallbacks {
public:
  class PendingCommand final {
  public:
    explicit PendingCommand(ProviderToCoreCommand&& command) noexcept
        : command_(std::move(command)) {}

    ProviderToCoreCommand& command() noexcept { return command_; }
    void mark_accepted() noexcept { accepted_ = true; }
    bool accepted() const noexcept { return accepted_; }

  private:
    ProviderToCoreCommand command_;
    bool accepted_ = false;
  };

  enum class SinkResult : uint8_t {
    Rejected = 0,
    Accepted,
    AcceptedWithFailure,
  };

  struct Stats {
    uint64_t commands_dropped_full = 0;
    uint64_t commands_dropped_closed = 0;
    uint64_t commands_dropped_allocfail = 0;

    uint64_t non_frame_rejected_closed = 0;
    uint64_t non_frame_rejected_allocfail = 0;

    uint64_t frames_dropped_full = 0;
    uint64_t frames_dropped_closed = 0;
    uint64_t frames_dropped_allocfail = 0;

    uint64_t frames_released_on_drop_full = 0;
    uint64_t frames_released_on_drop_closed = 0;
    uint64_t frames_released_on_drop_allocfail = 0;
  };

  // sink is invoked ONLY on the core thread.
  // It is responsible for consuming the ProviderToCoreCommand (e.g., dispatching).
  ProviderCallbackIngress(CoreThread* core_thread,
                          std::function<SinkResult(PendingCommand&)> sink,
                          std::function<uint64_t()> core_monotonic_now_ns,
                          std::function<bool(uint64_t)> is_stream_display_demand_active,
                          std::function<void(ProviderTransportFailure)> on_transport_failure,
                          std::function<bool()> transport_accepting);
  ~ProviderCallbackIngress() override = default;

  ProviderCallbackIngress(const ProviderCallbackIngress&) = delete;
  ProviderCallbackIngress& operator=(const ProviderCallbackIngress&) = delete;

  Stats stats_copy() const noexcept;
  uint32_t ingress_depth_for_stream(uint64_t stream_id) const;

  // IProviderCallbacks (core-issued services)
  uint64_t allocate_native_id(NativeObjectType type) override;
  uint64_t core_monotonic_now_ns() override;
  bool is_stream_display_demand_active(uint64_t stream_id) override;
  void on_transport_failure(ProviderTransportFailure failure) override;

  // IProviderCallbacks
  void on_device_opened(uint64_t device_instance_id) override;
  void on_device_closed(uint64_t device_instance_id) override;

  void on_stream_created(uint64_t stream_id) override;
  void on_stream_destroyed(uint64_t stream_id) override;
  void on_stream_started(uint64_t stream_id) override;
  void on_stream_stopped(uint64_t stream_id, ProviderError error_or_ok) override;

  void on_capture_started(uint64_t capture_id, uint64_t device_instance_id) override;
  void on_capture_completed(uint64_t capture_id, uint64_t device_instance_id) override;
  void on_capture_failed(uint64_t capture_id, uint64_t device_instance_id, ProviderError error) override;
  void on_camera_static_facts(uint64_t device_instance_id,
                              ProviderCameraFacts facts) override;
  void on_capture_image_facts(uint64_t capture_id,
                              uint64_t device_instance_id,
                              uint32_t image_member_index,
                              ProviderCaptureImageFacts facts) override;

  void on_frame(const FrameView& frame) override;

  void on_device_error(uint64_t device_instance_id, ProviderError error) override;
  void on_stream_error(uint64_t stream_id, ProviderError error) override;

  void on_native_object_created(const NativeObjectCreateInfo& info) override;
  void on_native_object_destroyed(const NativeObjectDestroyInfo& info) override;

private:
  uint32_t on_frame_ingress_enqueued_(uint64_t stream_id);
  void on_frame_ingress_failed_(uint64_t stream_id);
  void on_frame_ingress_dispatched_(uint64_t stream_id);

  static bool is_frame_command_(ProviderToCoreCommandType type) noexcept;
  static bool is_authoritative_frame_(const FrameView& frame) noexcept;

  void post_command(ProviderToCoreCommand cmd) noexcept;
  void signal_transport_failure_(ProviderTransportFailure failure) noexcept;

  CoreThread* core_thread_ = nullptr; // non-owning
  std::function<SinkResult(PendingCommand&)> sink_;
  std::function<uint64_t()> core_monotonic_now_ns_;
  std::function<bool(uint64_t)> is_stream_display_demand_active_;
  std::function<void(ProviderTransportFailure)> on_transport_failure_;
  std::function<bool()> transport_accepting_;

  std::atomic<uint64_t> native_id_seq_{1};

  std::atomic<uint64_t> commands_dropped_full_{0};
  std::atomic<uint64_t> commands_dropped_closed_{0};
  std::atomic<uint64_t> commands_dropped_allocfail_{0};

  std::atomic<uint64_t> non_frame_rejected_closed_{0};
  std::atomic<uint64_t> non_frame_rejected_allocfail_{0};

  std::atomic<uint64_t> frames_dropped_full_{0};
  std::atomic<uint64_t> frames_dropped_closed_{0};
  std::atomic<uint64_t> frames_dropped_allocfail_{0};

  std::atomic<uint64_t> frames_released_on_drop_full_{0};
  std::atomic<uint64_t> frames_released_on_drop_closed_{0};
  std::atomic<uint64_t> frames_released_on_drop_allocfail_{0};

  mutable std::mutex ingress_mu_;
  std::unordered_map<uint64_t, uint32_t> stream_ingress_depth_;
};

} // namespace cambang
