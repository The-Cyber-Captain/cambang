#pragma once

// CamBANG android_camera2 platform provider.
//
// Family: android_camera2 (docs/provider_architecture.md §2.2.x). The backend
// is the Camera2 NDK surface: ACameraManager/ACameraDevice/
// ACameraCaptureSession (libcamera2ndk) with AImageReader (libmediandk)
// delivering YUV_420_888 images. Device identity is the Camera2 camera id
// string reported by ACameraManager_getCameraIdList. This translation unit
// requires the Android NDK; the build compiles it only for Android GDE
// targets.
//
// Three backend properties shape everything below and are worth knowing
// before changing anything here:
//
//   - Pixel format. Camera2 guarantees YUV_420_888, JPEG and PRIVATE outputs;
//     RGBA_8888 from the camera HAL is not a guaranteed capability. Core's
//     profiles are packed RGBA/BGRA, so the provider configures AImageReader
//     as YUV_420_888 and converts (see convert_yuv420_to_packed). The
//     conversion is the price of working on every device rather than the
//     subset that happens to expose RGBA.
//
//   - Outputs are fixed at session creation. A Camera2 capture session
//     declares its whole output set up front; adding an output means tearing
//     the session down and rebuilding it, which cancels any repeating
//     request. start_stream therefore provisions the still output alongside
//     the stream output at the *same* geometry, so still capture while
//     streaming works at the stream's geometry, and a capture that needs a
//     different geometry while a stream is producing is refused
//     (ERR_PLATFORM_CONSTRAINT) rather than glitching the live stream. That
//     refusal is a session-configuration constraint, not a shortcut.
//
//   - Metadata is genuinely realized. Camera2 capture *result* metadata
//     reports what the sensor actually did for that exact frame, not the
//     request's set-point, so exposure time, sensitivity, aperture, focal
//     length and focus distance are posted as per-image facts when the
//     device reports them (brief §8). Static characteristics are read once at
//     open and cached, which is why admission can answer bracket support
//     promptly and authoritatively without any live control read.
//
// Contract source: docs/provider_implementation_brief.md. This provider is an
// adapter to that contract; it defines no contract semantics of its own.
//
// Threading model (see brief §2):
// - Every mutating ICameraProvider entry arrives core-thread-serialized via
//   ProviderBroker; provider state is still guarded by state_mutex_ across
//   check-then-act windows.
// - Every blocking Camera2 call (openCamera, createCaptureSession, session
//   close) runs on a single provider-owned control thread with a bounded
//   wait, so a wedged camera HAL degrades to a deterministic ERR_TIMEOUT
//   instead of wedging the core thread (brief §2 enforcement ladder).
// - AImageReader listeners and ACameraCaptureSession capture callbacks fire
//   on NDK-owned threads; every provider->core fact is funneled through
//   CBProviderStrand (the single serialized callback context).
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

namespace camera2_detail {

// Single provider-owned thread that executes backend (blocking Camera2) jobs
// FIFO. Callers wait with a deadline; on timeout the job is abandoned: it
// still runs to completion eventually and must self-release anything it
// acquired (the shared AbandonToken tells it the caller has given up).
//
// Deliberately a second copy of the WinRT provider's BoundedControlExecutor
// rather than a shared primitive. Extracting one would mean editing the
// shipped Windows provider, which cannot be rebuilt or revalidated from this
// tranche's toolchain; a hoist belongs in a tranche that can prove both
// providers still pass. Keep the two in sync by hand until then.
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

// Opaque holder for per-device Camera2 objects + frame routing state.
// Defined in the .cpp so no platform headers leak into this header.
struct DeviceBackend;

} // namespace camera2_detail

class Camera2CameraProvider final : public ICameraProvider {
public:
  Camera2CameraProvider() = default;
  ~Camera2CameraProvider() override;

  // Startup readiness preflight: no UI, no permission prompt, no hardware
  // open. Lists camera ids through ACameraManager only.
  //
  // The CAMERA runtime permission is a Java/framework concept with no NDK
  // query, so this check cannot observe it: id enumeration succeeds whether
  // or not the app holds the permission. A denial therefore surfaces
  // truthfully at open_device() time as ACAMERA_ERROR_PERMISSION_DENIED
  // (mapped to ERR_PLATFORM_CONSTRAINT) rather than being guessed at here.
  static ProviderAccessStatus check_access_readiness() noexcept;

  const char* provider_name() const override { return "Camera2CameraProvider"; }
  ProviderKind provider_kind() const noexcept override {
    return ProviderKind::platform_backed;
  }

  StreamTemplate stream_template() const override;
  CaptureTemplate capture_template() const override;
  bool supports_stream_picture_updates() const noexcept override { return false; }
  bool supports_capture_picture_updates() const noexcept override { return false; }
  // Real per-member exposure-compensation bracketing (see
  // run_device_capture_job_). Camera2 carries the compensation on the capture
  // request itself, so each member is a genuinely separate exposure program
  // rather than a device-global control write. Per-device support is still
  // enforced deterministically at admission
  // (validate_and_admit_submission_locked_) from characteristics cached at
  // open: a device whose ACAMERA_CONTROL_AE_COMPENSATION_RANGE is empty or
  // degenerate, or a bundle exceeding kMaxBracketMembers, is refused with
  // ERR_NOT_SUPPORTED.
  bool supports_multi_image_still_sequence() const noexcept override { return true; }

  // Derived from the bounded per-step timeouts below (never a guess, per the
  // doc comment on the base declaration).
  //
  // Cold setup is two bounded control jobs -- session realization (readers,
  // outputs, ACameraDevice_createCaptureSession) and request construction.
  // Each member then pays one still capture bounded by kCaptureSampleWaitMs
  // plus a control-thread queueing allowance, because the capture submission
  // runs on the shared bounded executor.
  //
  // This exceeds Core's 30s default, which sizes only for a single-image
  // capture.
  uint64_t capture_admission_watchdog_timeout_ns() const noexcept override {
    constexpr uint64_t kColdSetupChainMs = 2ull * kControlJobTimeoutMs;
    constexpr uint64_t kPerMemberMs =
        static_cast<uint64_t>(kCaptureSampleWaitMs) + kControlJobTimeoutMs;
    constexpr uint64_t kSafetyMarginMs = 2000ull;
    constexpr uint64_t kWorstCaseMs =
        kColdSetupChainMs + static_cast<uint64_t>(kMaxBracketMembers) * kPerMemberMs +
        kSafetyMarginMs;
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
  // Worst-case admitted-capture latency must stay under the derived watchdog
  // above: (queue/workers + 1) * sample-wait + control job bounds.
  static constexpr size_t kCaptureWorkerCount = 2;
  static constexpr size_t kCaptureQueueCapacity = 6;
  // A still through a freshly configured session can wait on AE/AF
  // convergence on real hardware; 5s contains a wedged HAL without failing a
  // camera that is merely still converging.
  static constexpr uint32_t kCaptureSampleWaitMs = 5000;
  // Session creation is the slowest bounded step (the HAL allocates buffers
  // for every output), so this is deliberately larger than the WinRT
  // provider's equivalent.
  static constexpr uint32_t kControlJobTimeoutMs = 5000;
  // Bound on waiting for auto-focus to settle after a lock trigger. Sized to
  // contain a lens that never converges, not to pace one that does: a normal
  // convergence reports a locked AF state in a few frames and the wait returns
  // as soon as it does, so this is never spent in the common case.
  static constexpr uint32_t kAfLockWaitMs = 1500;
  // AImageReader maxImages for the repeating stream. Must exceed the number
  // of images the converter can hold at once; the provider copies out and
  // deletes each AImage immediately, so this is headroom, not a pipeline
  // depth.
  static constexpr int32_t kStreamReaderMaxImages = 4;
  static constexpr int32_t kStillReaderMaxImages = 2;
  static constexpr size_t kStreamPoolSlots = 8;
  // Provider-side admission cap for still_image_bundle size on this provider.
  // Bounded (not unlimited) so capture_admission_watchdog_timeout_ns() above
  // can be a derived worst case rather than an open-ended guess; 5 covers
  // realistic bracket UX (common exposure-bracket counts are 3/5/7) with
  // headroom over this repo's 3-member reference bundle.
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
    // ACameraCaptureSession) lives on the backend, which owns its own locking
    // so capture workers never hold state_mutex_ across bounded backend jobs.
    std::shared_ptr<camera2_detail::DeviceBackend> backend;
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

  // Realizes (or rebuilds) the device's capture session so it carries exactly
  // the requested output set, and emits the AcquisitionSession native-created
  // fact on each realization. Serialized per device via the backend's
  // configure mutex; must NOT be called while holding a backend's inner
  // mutex. Lock order: state_mutex_ (optional, core-thread entries only) ->
  // backend configure mutex -> backend inner mutex; nothing re-acquires
  // state_mutex_ inside the configure mutex.
  //
  // Rebuilding cancels any repeating request, so a config change is refused
  // with ERR_PLATFORM_CONSTRAINT while the stream is producing.
  ProviderResult ensure_session_configured_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend,
      bool want_stream,
      uint32_t stream_width,
      uint32_t stream_height,
      bool want_still,
      uint32_t still_width,
      uint32_t still_height);

  // Tears the session down (session close, outputs, readers) on the control
  // thread and emits the AcquisitionSession destruction fact. Caller must
  // hold the backend's configure mutex, or be past the point where any other
  // caller can reach the backend.
  void teardown_session_locked_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend);

  // Best-effort, device-level static camera facts (facing, sensor mounting
  // orientation, and any genuinely device-constant optical quantity) from the
  // ACameraMetadata characteristics cached at open. A device that reports
  // none of them simply gets no static facts posted -- never fabricated.
  // Posts through the strand after on_device_opened, per brief §8 (static
  // facts key by opened device identity).
  void post_static_camera_facts_best_effort_(
      uint64_t device_instance_id,
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend);

  // Capture executor.
  bool start_capture_executor_() noexcept;
  void stop_and_join_capture_executor_() noexcept;
  void capture_worker_main_() noexcept;
  void run_device_capture_job_(const DeviceCaptureJob& job) noexcept;
  ProviderResult validate_and_admit_submission_locked_(
      const CaptureSubmission& submission,
      std::vector<DeviceCaptureJob>& out_jobs);

  struct CapturedMemberFrame;
  // How one member's exposure is to be programmed. Either a manual
  // exposure/sensitivity pair (AE off) or an auto-exposure compensation bias.
  struct MemberRequestSpec;

  // Submits every spec as ONE ACameraCaptureSession_capture() call and waits
  // for all of their images plus result metadata.
  //
  // A burst is the whole point of the manual path: the members land on
  // consecutive sensor frames (~33ms apart at 30fps) instead of the ~234ms
  // per member that separate submissions cost, which is what makes the
  // members describe the same scene closely enough to combine. Images are
  // paired to their result metadata by ACAMERA_SENSOR_TIMESTAMP, the only
  // correlation Camera2 offers once several captures are in flight together.
  //
  // Returns false only when the submission itself failed; per-member failures
  // are reported in out_frames.
  bool capture_burst_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend,
      uint32_t width,
      uint32_t height,
      uint32_t format_fourcc,
      const std::vector<MemberRequestSpec>& specs,
      std::vector<CapturedMemberFrame>& out_frames) noexcept;

  // Single auto-exposed capture whose realized exposure/sensitivity become the
  // frozen reference every manual bracket member is derived from. Its image is
  // discarded: including it in the bundle would put one member a full
  // submission round-trip away from the rest, which is the temporal spread the
  // burst exists to remove.
  bool meter_manual_baseline_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend,
      uint32_t width,
      uint32_t height,
      uint32_t format_fourcc,
      double& out_exposure_ns,
      double& out_sensitivity,
      ProviderError& out_error) noexcept;

  // Submits a single request carrying only an AF trigger, to lock or release
  // the lens. Fire-and-forget: the delivered image is dropped by the still
  // listener because no collector is installed, which is cheaper than
  // threading a discard path through the collector.
  void submit_af_trigger_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend,
      uint8_t af_trigger) noexcept;

  // Waits, bounded, for the AF algorithm to reach a locked state as observed
  // on the repeating request's results. Returns false on timeout or when no
  // AF state is being observed at all.
  bool wait_for_af_lock_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend) noexcept;

  CBProviderStrand strand_;
  IProviderCallbacks* callbacks_ = nullptr;
  std::atomic<bool> initialized_{false};
  std::atomic<bool> shutting_down_{false};

  camera2_detail::BoundedControlExecutor control_;
  // ACameraManager, owned for the provider's whole lifetime. Opaque here.
  std::shared_ptr<void> manager_;

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
