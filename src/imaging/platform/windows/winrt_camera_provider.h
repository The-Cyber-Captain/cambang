#pragma once

// CamBANG windows_winrt platform provider.
//
// Family: windows_winrt (docs/provider_architecture.md §2.2.x). The backend
// surface is the Windows Media Foundation capture stack (MFEnumDeviceSources +
// IMFSourceReader), which is the engine underneath WinRT MediaCapture. The
// C++/WinRT projection and the Windows.Media.Capture ABI headers are not
// available under this repo's mandated MinGW toolchain; Media Foundation is
// the supported Windows camera API surface there and preserves identical
// device identity (symbolic links) and frame semantics.
//
// Contract source: docs/provider_implementation_brief.md. This provider is an
// adapter to that contract; it defines no contract semantics of its own.
//
// Threading model (see brief §2):
// - Every mutating ICameraProvider entry arrives core-thread-serialized via
//   ProviderBroker; provider state is still guarded by state_mutex_ across
//   check-then-act windows.
// - All Media Foundation object creation/configuration/release runs on a
//   single provider-owned control thread with a bounded wait, so a wedged
//   camera driver degrades to a deterministic ERR_TIMEOUT instead of wedging
//   the core thread (brief §2 enforcement ladder).
// - MF delivers samples on its own worker threads; every provider->core fact
//   is funneled through CBProviderStrand (the single serialized callback
//   context).
// - Still captures execute on a small bounded worker pool with generation-
//   based cancellation; saturation is an admission failure (ERR_BUSY).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imaging/api/icamera_provider.h"
#include "imaging/api/provider_access_status.h"
#include "imaging/api/provider_strand.h"

namespace cambang {

namespace winrt_detail {

// Single provider-owned thread that executes backend (COM/Media Foundation)
// jobs FIFO. Callers wait with a deadline; on timeout the job is abandoned:
// it still runs to completion eventually and must self-release anything it
// acquired (the shared AbandonToken tells it the caller has given up).
class BoundedControlExecutor final {
public:
  struct AbandonToken {
    std::atomic<bool> abandoned{false};
  };

  BoundedControlExecutor() = default;
  ~BoundedControlExecutor();

  BoundedControlExecutor(const BoundedControlExecutor&) = delete;
  BoundedControlExecutor& operator=(const BoundedControlExecutor&) = delete;

  bool start() noexcept;
  void stop() noexcept;
  bool running() const noexcept { return running_.load(std::memory_order_acquire); }

  // Runs job on the control thread and waits up to timeout_ms for it.
  // Returns false on timeout/stopped executor (job may still run later; it
  // must consult the token and self-clean when abandoned).
  bool run_bounded(std::function<void(const AbandonToken&)> job,
                   std::shared_ptr<AbandonToken> token,
                   uint32_t timeout_ms) noexcept;

private:
  void thread_main_() noexcept;

  struct Entry {
    std::function<void(const AbandonToken&)> job;
    std::shared_ptr<AbandonToken> token;
    std::shared_ptr<std::promise<void>> done;
  };

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Entry> q_;
  bool stop_requested_ = false;
  std::atomic<bool> running_{false};
  std::thread worker_;
};

// Opaque holder for per-device Media Foundation objects + sample routing
// state. Defined in the .cpp so no platform headers leak into this header.
struct DeviceBackend;

} // namespace winrt_detail

class WinrtCameraProvider final : public ICameraProvider {
public:
  WinrtCameraProvider() = default;
  ~WinrtCameraProvider() override;

  // Startup readiness preflight: no UI, no permission prompt, no hardware
  // open. Consults the Windows camera consent store only.
  static ProviderAccessStatus check_access_readiness() noexcept;

  const char* provider_name() const override { return "WinrtCameraProvider"; }
  ProviderKind provider_kind() const noexcept override {
    return ProviderKind::platform_backed;
  }

  StreamTemplate stream_template() const override;
  CaptureTemplate capture_template() const override;
  bool supports_stream_picture_updates() const noexcept override { return false; }
  bool supports_capture_picture_updates() const noexcept override { return false; }
  bool supports_multi_image_still_sequence() const noexcept override { return false; }

  ProducerBackingCapabilities stream_backing_capabilities(
      const CaptureProfile& profile,
      const PictureConfig& picture) const noexcept override;
  ProducerBackingCapabilities capture_backing_capabilities(
      const CaptureRequest& req) const noexcept override;

  ProviderResult initialize(IProviderCallbacks* callbacks) override;
  ProviderResult enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) override;

  ProviderResult open_device(
      const std::string& hardware_id,
      uint64_t device_instance_id,
      uint64_t root_id) override;
  ProviderResult close_device(uint64_t device_instance_id) override;

  ProviderResult create_stream(const StreamRequest& req) override;
  ProviderResult destroy_stream(uint64_t stream_id) override;

  ProviderResult start_stream(
      uint64_t stream_id,
      const CaptureProfile& profile,
      const PictureConfig& picture) override;
  ProviderResult stop_stream(uint64_t stream_id) override;
  ProviderResult update_stream_retained_production_plan(
      uint64_t stream_id,
      CoreRetainedProductionPlan requested_retained_plan) override;

  ProviderResult set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) override;
  ProviderResult set_capture_picture_config(uint64_t device_instance_id, const PictureConfig& picture) override;

  ProviderResult trigger_capture(const CaptureRequest& req) override;
  ProviderResult trigger_capture_submission(const CaptureSubmission& submission) override;
  ProviderResult abort_capture(uint64_t capture_id) override;

  ProviderResult apply_camera_spec_patch(
      const std::string& hardware_id,
      uint64_t new_camera_spec_version,
      SpecPatchView patch) override;
  ProviderResult apply_imaging_spec_patch(
      uint64_t new_imaging_spec_version,
      SpecPatchView patch) override;

  ProviderResult shutdown() override;

private:
  // ---- Bounds (admission saturation is ERR_BUSY, never hidden queue growth).
  // Worst-case admitted-capture latency must stay under Core's 30s default
  // capture-admission watchdog: (queue/workers + 1) * sample-wait + control
  // job bounds. 6/2 queue/workers with a 5s sample wait and 3.5s control jobs
  // keeps the worst case near 20s.
  static constexpr size_t kCaptureWorkerCount = 2;
  static constexpr size_t kCaptureQueueCapacity = 6;
  static constexpr uint32_t kCaptureSampleWaitMs = 5000;
  static constexpr uint32_t kControlJobTimeoutMs = 3500;
  static constexpr size_t kStreamPoolSlots = 8;

  struct DeviceState {
    std::string hardware_id;
    uint64_t device_instance_id = 0;
    uint64_t root_id = 0;
    bool open = false;
    uint64_t native_id = 0;
    // Core enforces one repeating stream per device instance; tracked defensively.
    uint64_t stream_id = 0;
    // AcquisitionSession native truth (the concretely realized
    // IMFSourceReader) lives on the backend, which owns its own locking so
    // capture workers never hold state_mutex_ across bounded backend jobs.
    std::shared_ptr<winrt_detail::DeviceBackend> backend;
  };

  struct StreamState {
    StreamRequest req{};
    bool created = false;
    bool started = false;
    uint64_t native_id = 0;
  };

  struct DeviceCaptureJob {
    CaptureRequest request{};
    uint64_t generation = 0;
  };

  struct InFlightKey {
    uint64_t capture_id = 0;
    uint64_t device_instance_id = 0;
    bool operator<(const InFlightKey& o) const noexcept {
      if (capture_id != o.capture_id) return capture_id < o.capture_id;
      return device_instance_id < o.device_instance_id;
    }
  };

  uint64_t alloc_native_id_(NativeObjectType type);
  void emit_native_created_(uint64_t native_id,
                            NativeObjectType type,
                            uint64_t root_id,
                            uint64_t owner_device_id,
                            uint64_t owner_acquisition_session_id,
                            uint64_t owner_stream_id);
  void emit_native_destroyed_(uint64_t native_id);

  // Realizes the device's IMFSourceReader (control thread) and emits the
  // AcquisitionSession native-created fact on first realization. Serialized
  // per device via the backend's configure mutex; must NOT be called while
  // holding a backend's inner mutex. Lock order: state_mutex_ (optional,
  // core-thread entries only) -> backend configure mutex -> backend inner
  // mutex; nothing re-acquires state_mutex_ inside the configure mutex.
  ProviderResult ensure_reader_realized_(
      const std::shared_ptr<winrt_detail::DeviceBackend>& backend);
  // Configures the reader output media type (control thread). Same locking
  // rules as ensure_reader_realized_. Fails deterministically when a started
  // stream already pins a different geometry.
  ProviderResult ensure_reader_geometry_(
      const std::shared_ptr<winrt_detail::DeviceBackend>& backend,
      uint32_t width,
      uint32_t height,
      uint32_t format_fourcc);

  // Capture executor.
  bool start_capture_executor_() noexcept;
  void stop_and_join_capture_executor_() noexcept;
  void capture_worker_main_() noexcept;
  void run_device_capture_job_(const DeviceCaptureJob& job) noexcept;
  ProviderResult validate_and_admit_submission_locked_(
      const CaptureSubmission& submission,
      std::vector<DeviceCaptureJob>& out_jobs);

  CBProviderStrand strand_;
  IProviderCallbacks* callbacks_ = nullptr;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutting_down_{false};

  winrt_detail::BoundedControlExecutor control_;
  bool mf_started_ = false;

  // Provider bookkeeping state. Lock ordering when both are needed:
  // capture_mutex_ before state_mutex_ (matches SyntheticProvider).
  mutable std::mutex state_mutex_;
  std::map<uint64_t, DeviceState> devices_;   // key: device_instance_id
  std::map<uint64_t, StreamState> streams_;   // key: stream_id
  uint64_t provider_native_id_ = 0;

  // Capture executor state.
  mutable std::mutex capture_mutex_;
  std::condition_variable capture_cv_;
  bool capture_admission_closed_ = true;
  bool capture_stop_requested_ = true;
  uint64_t capture_generation_ = 0;
  std::deque<DeviceCaptureJob> capture_queue_;
  size_t capture_active_jobs_ = 0;
  std::map<InFlightKey, uint64_t> in_flight_captures_; // value: generation
  std::vector<std::thread> capture_workers_;
};

} // namespace cambang
