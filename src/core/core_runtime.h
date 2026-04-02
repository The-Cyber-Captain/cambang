// src/core/core_runtime.h
#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <string>

#include "core/core_dispatcher.h"
#include "core/core_device_registry.h"
#include "core/core_native_object_registry.h"
#include "core/core_rig_registry.h"
#include "core/core_runtime_state.h"
#include "core/core_spec_state.h"
#include "core/core_stream_registry.h"
#include "core/core_thread.h"
#include "core/core_frame_sink.h"
#include "core/i_state_snapshot_publisher.h"
#include "core/provider_callback_ingress.h"

#include "core/snapshot/snapshot_builder.h"

#if defined(CAMBANG_ENABLE_DEV_NODES)
#include "core/latest_frame_mailbox.h"
#endif

#include "imaging/api/icamera_provider.h"
#if !defined(CAMBANG_INTERNAL_SMOKE)
#include "imaging/broker/banner_info.h"
#endif

namespace cambang {

enum class TrySetStreamPictureStatus : uint8_t {
  OK = 0,
  NotSupported = 1,
  Busy = 2,
  InvalidArgument = 3,
};

enum class TryCreateStreamStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
};

enum class TryStartStreamStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
};

enum class TryStopStreamStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
};

enum class TryDestroyStreamStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
};

enum class TryOpenDeviceStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
};

enum class TryCloseDeviceStatus : uint8_t {
  OK = 0,
  Busy = 1,
  InvalidArgument = 2,
};

  class CoreRuntime final : private CoreThread::IHooks {
  private:
    enum class ShutdownPhase : uint8_t;  // forward declaration

  public:
    struct Stats {
    uint64_t publish_requests_coalesced = 0;
    uint64_t publish_requests_dropped_full = 0;
    uint64_t publish_requests_dropped_closed = 0;
    uint64_t publish_requests_dropped_allocfail = 0;
  };

  CoreRuntime();
  ~CoreRuntime();

  CoreRuntime(const CoreRuntime&) = delete;
  CoreRuntime& operator=(const CoreRuntime&) = delete;

  bool start();
  void stop();

  bool is_running() const { return core_thread_.is_running(); }

  // Tick-bounded publication bridge support.
  //
  // Core may publish multiple snapshots between Godot ticks. Godot-facing code
  // needs an O(1) marker to detect "something changed" since the previous tick,
  // without polling/coalescing in user code.
  //
  // - published_seq() increments once per successful core snapshot publish.
  // - published_topology_sig() is the core-computed topology signature of the
  //   latest published snapshot, suitable for boundary-side topology diffing.
  uint64_t published_seq() const noexcept {
    return published_seq_.load(std::memory_order_acquire);
  }

  uint64_t published_topology_sig() const noexcept {
    return published_topology_sig_.load(std::memory_order_acquire);
  }

  CoreRuntimeState state_copy() const noexcept {
    return state_.load(std::memory_order_acquire);
  }

  void post(CoreThread::Task task);

  CoreThread::PostResult try_post(CoreThread::Task task);

  // Dev/internal stream lifecycle surfaces.
  // Defaulting is performed by core using provider->stream_template().
  // profile_version ownership is core-authoritative for this ingress:
  // pass profile_version=0 to request core-assigned lineage.
  // These are non-blocking and may return Busy if the core mailbox is full.
  TryCreateStreamStatus try_create_stream(
      uint64_t stream_id,
      uint64_t device_instance_id,
      StreamIntent intent,
      const CaptureProfile* request_profile,
      const PictureConfig* request_picture,
      uint64_t profile_version) noexcept;

  TryStartStreamStatus try_start_stream(uint64_t stream_id) noexcept;

  TryStopStreamStatus try_stop_stream(uint64_t stream_id) noexcept;

  TryDestroyStreamStatus try_destroy_stream(uint64_t stream_id) noexcept;

  TryOpenDeviceStatus try_open_device(
      const std::string& hardware_id,
      uint64_t device_instance_id,
      uint64_t root_id) noexcept;

  TryCloseDeviceStatus try_close_device(uint64_t device_instance_id) noexcept;

  // Stream-scoped picture update path.
  // Non-blocking: enqueues the provider call onto the core thread.
  TrySetStreamPictureStatus try_set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) noexcept;

#if defined(CAMBANG_INTERNAL_SMOKE)
  CoreThread::PostResult try_post_core_thread_unchecked(CoreThread::Task task) {
    return core_thread_.try_post(std::move(task));
  }
#endif

  void request_publish();

  Stats stats_copy() const noexcept;

  ProviderCallbackIngress::Stats ingress_stats_copy() const noexcept { return ingress_.stats_copy(); }

  struct ShutdownDiag {
    uint8_t phase_code = 0;
    uint64_t phase_changes = 0;
  };

  ShutdownDiag shutdown_diag_copy() const noexcept;

#if defined(CAMBANG_INTERNAL_SMOKE)
  // Smoke-only: avoid hard-coded numeric coupling in tests.
  static constexpr uint8_t shutdown_phase_exit_code() noexcept {
    return static_cast<uint8_t>(ShutdownPhase::EXIT);
  }
#endif

  void set_snapshot_publisher(IStateSnapshotPublisher* publisher) noexcept {
    snapshot_publisher_.store(publisher, std::memory_order_release);
  }

  [[nodiscard]] CoreDispatchStats dispatcher_stats() const noexcept { return dispatcher_.stats(); }

  const CoreStreamRegistry::StreamRecord* stream_record(uint64_t stream_id) const noexcept {
    return streams_.find(stream_id);
  }

  IProviderCallbacks* provider_callbacks() { return &ingress_; }

#if defined(CAMBANG_ENABLE_DEV_NODES)
  const LatestFrameMailbox& latest_frame_mailbox() const noexcept { return latest_frame_mailbox_; }
#endif

  void attach_provider(ICameraProvider* provider) noexcept {
    provider_.store(provider, std::memory_order_release);

    // Ensure the core loop observes the attachment promptly so the
    // core-loop banner can print even if no other work is scheduled.
    // Safe to call from any thread.
    if (provider != nullptr) {
      core_thread_.request_timer_tick();
    }
  }

  // Non-owning access to the currently attached provider. Intended for
  // Godot-side orchestration (e.g. virtual_time pumping) without leaking
  // provider type knowledge into Core.
  ICameraProvider* attached_provider() const noexcept {
    return provider_.load(std::memory_order_acquire);
  }

  // Core-owned identity/spec truth retention hooks.
  // These are internal runtime surfaces used by orchestrators that issue provider calls.
  void retain_device_identity(uint64_t device_instance_id, const std::string& hardware_id);
  void retain_camera_spec_version(const std::string& hardware_id, uint64_t camera_spec_version);
  void retain_device_capture_profile(uint64_t device_instance_id,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t format,
                                     uint64_t capture_profile_version);
  void retain_rig_capture_profile(uint64_t rig_id,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t format,
                                  uint64_t capture_profile_version);
  void retain_imaging_spec_version(uint64_t imaging_spec_version);

  // Internal dev visibility: allow the Godot main thread to echo a core-thread
  // banner line via UtilityFunctions::print for environments where stdout isn't
  // reliably visible (e.g. Godot editor output on Windows).
  bool take_core_banner_line(char* out, size_t cap) noexcept {
    if (!out || cap == 0) {
      return false;
    }
    if (!core_banner_line_pending_.exchange(false, std::memory_order_acq_rel)) {
      return false;
    }
    // Core thread writes the buffer before setting the pending flag.
    std::strncpy(out, core_banner_line_, cap - 1);
    out[cap - 1] = '\0';
    return true;
  }

private:
  void on_core_start() override;
  void on_core_timer_tick() override;
  void on_core_stop() override;

  void enqueue_provider_fact(CoreCommand&& cmd);
  void enqueue_request(CoreThread::Task task);
  void request_publish_from_core_unchecked();

private:
  CoreThread core_thread_;
  CoreRigRegistry rigs_;
  CoreDeviceRegistry devices_;
  CoreSpecState spec_state_;
  CoreStreamRegistry streams_;
  CoreNativeObjectRegistry native_objects_;

  // Snapshot header counters (schema v1).
  // gen: core generation counter, monotonic across app/server lifetime.
  // version: per-publish within gen.
  // topology_version: structural within gen.
  std::uint64_t gen_counter_ = 0;
  std::uint64_t current_gen_ = 0;
  std::uint64_t version_ = 0;
  std::uint64_t topology_version_ = 0;
  uint64_t last_topology_sig_ = 0;
  bool has_topology_sig_ = false;

  std::atomic<CoreRuntimeState> state_{CoreRuntimeState::CREATED};

  SnapshotBuilder snapshot_builder_;
  std::atomic<IStateSnapshotPublisher*> snapshot_publisher_{nullptr};

  // Core-defined epoch for snapshot timestamp_ns (session-relative monotonic).
  std::chrono::steady_clock::time_point epoch_;

#if defined(CAMBANG_ENABLE_DEV_NODES)
  LatestFrameMailbox latest_frame_mailbox_;
  class LatestFrameMailboxSink final : public ICoreFrameSink {
  public:
    explicit LatestFrameMailboxSink(LatestFrameMailbox* mb) : mb_(mb) {}
    CoreVisibilityPath on_frame(FrameView frame) override {
      if (mb_) {
        return mb_->write_from_core(std::move(frame));
      } else {
        frame.release_now();
        return CoreVisibilityPath::NONE;
      }
    }
  private:
    LatestFrameMailbox* mb_ = nullptr;
  };
  LatestFrameMailboxSink latest_frame_sink_{&latest_frame_mailbox_};
#endif

  CoreDispatcher dispatcher_;
  ProviderCallbackIngress ingress_;

  std::deque<CoreCommand> provider_facts_;
  std::deque<CoreThread::Task> requests_;

  enum class ShutdownPhase : uint8_t {
    NONE = 0,
    STOP_STREAMS,
    AWAIT_STREAMS_STOPPED,
    DESTROY_STREAMS,
    AWAIT_STREAMS_DESTROYED,
    CLOSE_DEVICES,
    AWAIT_DEVICES_CLOSED,
    PROVIDER_SHUTDOWN,
    FINAL_RETENTION_SWEEP,
    FINAL_PUBLISH,
    CLEAR_DESTROYED_RETAINED_NATIVE_OBJECTS,
    EXIT
  };

  bool shutdown_requested_ = false;
  ShutdownPhase shutdown_phase_ = ShutdownPhase::NONE;
  std::atomic<uint8_t> shutdown_phase_code_{0};
  std::atomic<uint64_t> shutdown_phase_changes_{0};
  bool shutdown_final_publish_requested_ = false;
  uint32_t shutdown_wait_ticks_ = 0;

  std::atomic<ICameraProvider*> provider_{nullptr};
  bool provider_banner_printed_ = false; // core-thread only; reset each start()

  // One-line banner echo mailbox (core thread -> Godot thread).
  std::atomic<bool> core_banner_line_pending_{false};
  char core_banner_line_[192] = {0};

  std::atomic<bool> publish_pending_{false};

  // Publish markers (core thread writes; any thread reads).
  // These do not redefine the snapshot schema; they exist to support the
  // Godot-facing tick-bounded truth model cheaply.
  std::atomic<uint64_t> published_seq_{0};
  std::atomic<uint64_t> published_topology_sig_{0};
  std::atomic<uint64_t> create_stream_profile_version_seq_{1};

  std::atomic<uint64_t> publish_requests_coalesced_{0};
  std::atomic<uint64_t> publish_requests_dropped_full_{0};
  std::atomic<uint64_t> publish_requests_dropped_closed_{0};
  std::atomic<uint64_t> publish_requests_dropped_allocfail_{0};

  static constexpr uint64_t kDestroyedNativeObjectRetentionWindowNs = 5ull * 1000ull * 1000ull * 1000ull;
};

} // namespace cambang
