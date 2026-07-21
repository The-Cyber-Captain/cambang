#pragma once

// CamBANG windows_winrt platform provider.
//
// Family: windows_winrt (docs/provider_architecture.md §2.2.x). The backend
// is the WinRT capture surface via C++/WinRT: Windows.Media.Capture
// (MediaCapture) with Windows.Media.Capture.Frames (MediaFrameReader)
// delivering Bgra8 software bitmaps. Device identity is the WinRT
// DeviceInformation Id (the device-interface symbolic link). This
// translation unit requires a C++/WinRT-capable toolchain (MSVC + Windows
// SDK); the build compiles it only for MSVC Windows GDE targets.
//
// Contract source: docs/provider_implementation_brief.md. This provider is an
// adapter to that contract; it defines no contract semantics of its own.
//
// Threading model (see brief §2):
// - Every mutating ICameraProvider entry arrives core-thread-serialized via
//   ProviderBroker; provider state is still guarded by state_mutex_ across
//   check-then-act windows.
// - All WinRT object creation/configuration/release (every awaited async op)
//   runs on a single provider-owned control thread with a bounded wait, so a
//   wedged camera driver degrades to a deterministic ERR_TIMEOUT instead of
//   wedging the core thread (brief §2 enforcement ladder).
// - FrameArrived events fire on WinRT threadpool threads; every
//   provider->core fact is funneled through CBProviderStrand (the single
//   serialized callback context).
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

// Single provider-owned thread that executes backend (WinRT async) jobs
// FIFO. Callers wait with a deadline; on timeout the job is abandoned:
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

// Opaque holder for per-device WinRT capture objects + frame routing state.
// Defined in the .cpp so no platform headers leak into this header.
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
  // Real per-member exposure-compensation bracketing (see run_device_capture_job_).
  // Per-device/per-bundle-size support is still enforced deterministically at
  // admission (validate_and_admit_submission_locked_): a device whose
  // VideoDeviceController has no ExposureCompensationControl, or a bundle
  // exceeding kMaxBracketMembers, is refused with ERR_NOT_SUPPORTED.
  bool supports_multi_image_still_sequence() const noexcept override { return true; }

  // Derived from the bounded per-step timeouts below (never a guess, per the
  // doc comment on the base declaration): worst case is a cold reader
  // realize+geometry+start chain (3 * kControlJobTimeoutMs) plus up to
  // kMaxBracketMembers members, each paying one bounded exposure-compensation
  // control job (kExposureControlJobTimeoutMs) and one bounded sample wait
  // (kCaptureSampleWaitMs), plus a fixed safety margin. This exceeds Core's
  // 30s default, which sizes only for a single-image capture.
  uint64_t capture_admission_watchdog_timeout_ns() const noexcept override {
    constexpr uint64_t kColdReaderChainMs = 3ull * kControlJobTimeoutMs;
    constexpr uint64_t kPerMemberMs =
        static_cast<uint64_t>(kExposureControlJobTimeoutMs) + kCaptureSampleWaitMs;
    constexpr uint64_t kSafetyMarginMs = 2000ull;
    constexpr uint64_t kWorstCaseMs =
        kColdReaderChainMs + static_cast<uint64_t>(kMaxBracketMembers) * kPerMemberMs + kSafetyMarginMs;
    return kWorstCaseMs * 1'000'000ull;
  }

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
  // Setting/reading a UVC device control is near-instant on real hardware;
  // this bound exists only to contain a wedged driver, matching the same
  // enforcement posture as kControlJobTimeoutMs.
  static constexpr uint32_t kExposureControlJobTimeoutMs = 2000;
  // Provider-side admission cap for still_image_bundle size on this
  // provider. Bounded (not unlimited) so capture_admission_watchdog_timeout_ns()
  // above can be a derived worst case rather than an open-ended guess; 5
  // covers realistic bracket UX (common exposure-bracket counts are 3/5/7)
  // with headroom over this repo's 3-member reference bundle.
  static constexpr uint32_t kMaxBracketMembers = 5;

  struct DeviceState {
    std::string hardware_id;
    uint64_t device_instance_id = 0;
    uint64_t root_id = 0;
    bool open = false;
    uint64_t native_id = 0;
    // Core enforces one repeating stream per device instance; tracked defensively.
    uint64_t stream_id = 0;
    // AcquisitionSession native truth (the concretely realized
    // MediaFrameReader) lives on the backend, which owns its own locking so
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

  // Realizes the device's MediaFrameReader (control thread) and emits the
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
  // Starts the realized frame reader (idempotent). Same locking rules as
  // ensure_reader_realized_.
  ProviderResult ensure_reader_started_(
      const std::shared_ptr<winrt_detail::DeviceBackend>& backend);

  // Best-effort, device-level static camera facts (facing, sensor mounting
  // orientation) from DeviceInformation.EnclosureLocation. Runs its own
  // bounded control-thread lookup and never fails/blocks open_device(): a
  // lookup failure, timeout, or a device with no reported enclosure location
  // simply means no static facts are posted (never fabricated), logged via
  // log_line for diagnosability only. Posts through the strand after
  // on_device_opened, per brief §8 (static facts key by opened device
  // identity).
  void post_static_camera_facts_best_effort_(
      uint64_t device_instance_id, const std::string& hardware_id);

  // Capture executor.
  bool start_capture_executor_() noexcept;
  void stop_and_join_capture_executor_() noexcept;
  void capture_worker_main_() noexcept;
  void run_device_capture_job_(const DeviceCaptureJob& job) noexcept;
  ProviderResult validate_and_admit_submission_locked_(
      const CaptureSubmission& submission,
      std::vector<DeviceCaptureJob>& out_jobs);

  // Bracket-capture support. Reads of Supported/Min/Max/Step/Value are plain
  // WinRT property gets (not awaited async ops) and are safe to call
  // synchronously from the core thread during admission; only SetValueAsync
  // needs the bounded control executor.
  //
  // Returns false (device has no usable exposure-compensation control) when
  // unsupported or on any WinRT exception -- callers must treat that as a
  // deterministic "bracket not supported for this device", not an error.
  static bool query_exposure_compensation_range_(
      const std::shared_ptr<winrt_detail::DeviceBackend>& backend,
      float& out_min, float& out_max, float& out_step);

  // Realized focus state, reported only where it maps onto CamBANG's
  // FocusState model with zero unit-fabrication risk. WinRT's continuous
  // FocusControl.Value() has no confirmed physical unit (its own vocabulary
  // is preset/enum-based: ManualFocusDistance{Infinity,Hyperfocal,Nearest}),
  // so only the AutoInfinity preset -- the one case expressible without
  // guessing a distance -- is surfaced; every other preset/mode is left
  // unreported rather than approximated.
  static bool query_focus_state_at_infinity_(
      const std::shared_ptr<winrt_detail::DeviceBackend>& backend);

  // Bounded (control-thread) apply of one exposure-compensation value.
  // Converts requested_milli_ev to EV, clamps and rounds to the device's
  // actual step grid, applies it, then reads back the real post-set value as
  // the truthful realized EV (never the requested value re-stated as if
  // verified). Returns false on timeout/WinRT failure.
  bool apply_exposure_compensation_bounded_(
      const std::shared_ptr<winrt_detail::DeviceBackend>& backend,
      int32_t requested_milli_ev,
      int32_t& out_realized_milli_ev,
      ProviderError& out_error) noexcept;

  struct CapturedMemberFrame {
    bool ok = false;
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    bool has_sample_time = false;
    int64_t sample_time_100ns = 0;

    // Optional per-image camera intrinsics/distortion, read from the
    // delivered frame's VideoMediaFrame.CameraIntrinsics() when the
    // device/driver genuinely supplies it (commonly null on plain UVC
    // webcams; real on some depth/professional cameras). Never fabricated:
    // has_intrinsics stays false when WinRT reported none.
    bool has_intrinsics = false;
    float focal_length_x = 0.0f;
    float focal_length_y = 0.0f;
    float principal_point_x = 0.0f;
    float principal_point_y = 0.0f;
    float radial_k1 = 0.0f, radial_k2 = 0.0f, radial_k3 = 0.0f;
    float tangential_p1 = 0.0f, tangential_p2 = 0.0f;
    uint32_t intrinsics_reference_width = 0;
    uint32_t intrinsics_reference_height = 0;
  };
  // Captures exactly one still frame at the given geometry/format via the
  // waiter mechanism. Shared by the default-metered member and every
  // additional-bracket member -- this is the same single-frame wait
  // single-image capture already used, just made reusable per member.
  CapturedMemberFrame capture_one_member_frame_(
      const std::shared_ptr<winrt_detail::DeviceBackend>& backend,
      uint32_t width, uint32_t height, uint32_t format_fourcc) noexcept;

  CBProviderStrand strand_;
  IProviderCallbacks* callbacks_ = nullptr;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutting_down_{false};

  winrt_detail::BoundedControlExecutor control_;

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
