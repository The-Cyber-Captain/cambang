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

#include "imaging/api/acquisition_seam_claims.h"
#include "imaging/api/icamera_provider.h"
#include "imaging/api/capture_sequence_settlement.h"
#include "imaging/api/outstanding_payload_ledger.h"
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

  // Camera2 delivers YUV_420_888, whose concrete member the device decides at
  // runtime. Both the semi-planar and fully planar members are advertised
  // because either may arrive and CamBANG converts both; the packed formats
  // remain available via the provider's own conversion.
  ProducerFormatCapabilities stream_format_capabilities(
      const CaptureProfile& profile,
      const PictureConfig& picture) const noexcept override {
    (void)profile;
    (void)picture;
    ProducerFormatCapabilities caps{};
    caps.add(FOURCC_NV12);
    caps.add(FOURCC_I420);
    caps.can_emit_packed_rgb = true;
    return caps;
  }

  // Still capture takes the same YUV_420_888 output as streams, so it can
  // deliver the planar family unconverted too.
  ProducerFormatCapabilities capture_format_capabilities(
      const CaptureRequest& req) const noexcept override {
    (void)req;
    ProducerFormatCapabilities caps{};
    caps.add(FOURCC_NV12);
    caps.add(FOURCC_I420);
    caps.can_emit_packed_rgb = true;
    return caps;
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

  ProviderResult sync_capture_parent_priming(const CaptureRequest& req) override;
  ProviderResult release_capture_parent_priming(uint64_t device_instance_id) override;

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
  // Grace allowed after the platform closes a capture's sequence for buffers
  // whose delivery callback is already running. Camera2 orders the buffer and
  // sequence-completed callbacks per-camera, not by contract: Galaxy S20+
  // camera 0 fires completion 7-13ms after the buffer callback begins, and
  // settling on completion alone lost 21 of 30 captures there. 50ms is ~4x the
  // widest measured window, and is paid only by a capture that is genuinely
  // short -- one that already has every member never waits.
  static constexpr uint32_t kSequenceEndInFlightGraceMs = 50;
  // Session creation is the slowest bounded step (the HAL allocates buffers
  // for every output), so this is deliberately larger than the WinRT
  // provider's equivalent.
  static constexpr uint32_t kControlJobTimeoutMs = 5000;
  // Bound on waiting for auto-focus to settle after a lock trigger. Sized to
  // contain a lens that never converges, not to pace one that does: a normal
  // convergence reports a locked AF state in a few frames and the wait returns
  // as soon as it does, so this is never spent in the common case.
  static constexpr uint32_t kAfLockWaitMs = 1500;

  // Pilot stream: a brief repeating flow run before a still on a device
  // that has no stream of its own, so the pipeline is producing when the still
  // is submitted.
  //
  // NAMING. This is deliberately not called "metering". CamBANG already uses
  // that word for a still-bundle member role
  // (CaptureStillImageMemberRole::DEFAULT_METERED, the provider's
  // purpose=member_ae traces), and Camera2 uses "precapture metering sequence"
  // for something specific and narrower, driven by
  // ACAMERA_CONTROL_AE_PRECAPTURE_TRIGGER. We do not set that trigger, so this
  // is not a precapture sequence and must not borrow its name. Nor is it a
  // keep-alive or a warm-up: it runs for one capture and stops.
  //
  // WHY. Camera2's documented still-imaging stream configurations pair a
  // preview target with the still target, and its still flow assumes 3A has
  // frames to work from. A still-only session asked for a still cold is
  // outside that shape, and both handsets tested fail in exactly that
  // configuration -- differently, which is why this is a pilot stream and not a fix
  // for any single vendor's behaviour.
  static constexpr uint32_t kPilotStreamWaitMs = 1200;
  // Frames the pilot stream waits for before submitting the still.
  //
  // The wait is on FRAMES ARRIVING, not on AE convergence. AE_STATE_INACTIVE
  // means the exposure routine is off or reset -- not that it is still
  // converging -- and a device that never leaves INACTIVE (Quest passthrough)
  // offers no convergence to wait for, so an AE predicate degenerates into
  // burning the full timeout. Frames arriving is the thing every device
  // reports, and it is what the pilot stream exists to produce.
  //
  // Two, not one: the first frame out of an idle pipeline can be the one the
  // sensor already had.
  static constexpr uint32_t kPilotStreamFrames = 0;
  // DIAGNOSTIC: lock AE on the still request, where the device offers it.
  // The pilot converges AE first, so this pins the converged values for the
  // still instead of letting AE keep evaluating between the pilot and the
  // capture. Test switch, not a decision.
  static constexpr bool kLockAeForStillTest = false;
  // DIAGNOSTIC: also lock AF before the still when the pilot is the frame
  // source. The existing focus-lock path is gated on a caller's stream because
  // that was the only frame source AF could converge on; the pilot is now
  // another. Test switch, not a decision.
  static constexpr bool kLockAfForStillTest = false;
  // DIAGNOSTIC: hold the lens still for the duration of a test run.
  //
  // AF_MODE_OFF alone is not enough: Camera2 says "the auto-focus routine does
  // not control the lens; LENS_FOCUS_DISTANCE is controlled by the
  // application", so without a distance the lens goes wherever the request
  // template leaves it. Both are set, on the pilot AND the still, so no part of
  // a capture cycle can move the lens.
  //
  // This is NOT a focus lock and not the bracket path: it disables AF rather
  // than triggering and locking it, so no extra capture is submitted anywhere.
  // Focus quality is deliberately sacrificed -- the point is a stationary lens
  // while delivery is measured, not a sharp picture.
  static constexpr bool kFreezeLensForTest = false;
  // Dioptres. 0.0 is optical infinity.
  //
  // MEASURED: setting this does NOT move the lens on either S20+ camera.
  // realized_focus_diopters stayed at 0.1000 with the freeze on and off, so the
  // distance is ignored or clamped; what the freeze reliably achieves is AF
  // MODE off (af_mode 4 -> 0, af_state -> INACTIVE), not a chosen position.
  //
  // ALSO UNCHECKED, if this is ever made more than a diagnostic: nothing reads
  // ACAMERA_CONTROL_AF_AVAILABLE_MODES to confirm OFF is offered, nothing reads
  // LENS_INFO_MINIMUM_FOCUS_DISTANCE to range-check the distance, and unlike
  // the focus-lock path it does not consult chars.fixed_focus_at_infinity. It
  // happened to apply cleanly on both test cameras; on a third it is a blind
  // write.
  static constexpr float kFrozenLensFocusDistance = 0.0f;
  // Pilot geometry is resolved per device from advertised YUV sizes at open
  // (StaticCharacteristics::pilot_width/height), not fixed here. A constant
  // was wrong: it assumed a size the device may not offer, and nothing checked.
  // Arm selector for the pilot stream versus continuous-stream comparison.
  static constexpr bool kPilotStreamEnabled = true;
  // DIAGNOSTIC: keep the pilot running for the life of the seam instead of
  // starting and stopping it per capture.
  //
  // Persistent was measured WORSE with a YUV pilot -- mean request->exposure
  // ~47ms against ~16ms per-capture -- but that comparison was made while every
  // discarded pilot frame was a CPU-readable YUV conversion. With a PRIVATE
  // pilot that per-frame cost is largely gone, so the comparison is worth
  // remaking before per-capture is treated as settled.
  static constexpr bool kPersistentPilotTest = false;
  // AImageReader maxImages for the repeating stream (a caller's stream, or the
  // pilot). Must exceed the number of images the converter can hold at once;
  // the provider copies out and deletes each AImage immediately, so this is
  // headroom, not a pipeline depth.
  //
  // Do not reduce this on the reasoning that we only hold one image at a time.
  // That was tried (4 -> 2, on the argument that acquireLatestImage discards
  // older frames anyway so a full queue costs nothing) and it regressed real
  // hardware: Galaxy S20+ camera 1 dropped to 2 of 6 stills delivered, twice,
  // and returned to 6 of 6 the moment this went back to 4. maxImages does not
  // only bound what WE hold -- it bounds how many buffers exist for the
  // PRODUCER to fill, and starving the producer on this reader costs the
  // still, not the pilot frame that was discarded anyway.
  static constexpr int32_t kStreamReaderMaxImages = 4;
  static constexpr size_t kStreamPoolSlots = 8;
  // Provider-side admission cap for still_image_bundle size on this provider.
  // This is a deliberate POLICY cap, not a device/hardware limit: it is the same
  // constant on every device (so all devices refuse >5 identically), and it
  // bounds capture_admission_watchdog_timeout_ns() above to a derived worst case
  // (kColdSetupChainMs + kMaxBracketMembers*kPerMemberMs + ...) rather than an
  // open-ended guess. 5 covers realistic bracket UX (3- and 5-shot) with
  // headroom over this repo's 3-member reference bundle; 7-shot is refused with
  // ERR_NOT_SUPPORTED by design. Raising this is a two-part change, not a
  // one-liner: widen the latency budget above AND raise kStillReaderMaxImages
  // (the still-reader depth would otherwise bottleneck the larger burst), then
  // re-verify a full-count bracket on real hardware.
  static constexpr uint32_t kMaxBracketMembers = 5;

  // Still AImageReader depth, DERIVED from the bracket cap rather than chosen.
  //
  // The provider copies out and deletes each AImage immediately, so it too
  // holds only one at a time -- but unlike the stream reader this one uses
  // AImageReader_acquireNextImage and every frame is a bundle member. A frame
  // lost to a full queue is a lost member, not a discarded preview frame. The
  // queue must therefore absorb a whole burst while we drain it one at a time,
  // and the largest burst admissible is kMaxBracketMembers.
  //
  // Deriving it makes the coupling structural. It was previously 2 with a
  // comment instructing whoever raised kMaxBracketMembers to raise this in
  // step; the cap had since reached 5 and this had not moved, so a full-size
  // bundle was relying on drain speed rather than on depth.
  //
  // NOT verified on hardware: no multi-member bundle has been exercised. Quest
  // passthrough reports burst=no and takes the sequential member_ae path; the
  // S20+ advertises BURST_CAPTURE and is where this becomes testable.
  // Still AImageReader depth. The provider copies out and deletes each AImage
  // immediately, so a burst of N members can drain through a shallow reader --
  // but only up to a point: a bundle whose members outrun this depth will
  // starve the reader mid-burst. This depth therefore couples to
  // kMaxBracketMembers: raising the bracket cap REQUIRES raising this in step
  // (and re-verifying on device), or the extra members bottleneck here.
  //
  // Left at 2 deliberately. Deriving it from kMaxBracketMembers was tried and
  // is arguably more correct, but nothing has exercised the case it protects:
  // every capture measured has been single-member, so 2 and 5 are
  // indistinguishable. Quest passthrough reports burst=no; the S20+ advertises
  // BURST_CAPTURE and is where a multi-member bundle would actually test this.
  static constexpr int32_t kStillReaderMaxImages = 2;

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
    // Resolved at admission, when the acquisition-seam capture reference is
    // taken. Carried on the job so the worker releases against the same
    // backend it was admitted for, rather than re-resolving a device that may
    // have been closed and reopened in between.
    std::shared_ptr<camera2_detail::DeviceBackend> backend;
  };

  struct InFlightKey {
    uint64_t capture_id = 0;
    uint64_t device_instance_id = 0;
    bool operator<(const InFlightKey& o) const noexcept {
      if (capture_id != o.capture_id) return capture_id < o.capture_id;
      return device_instance_id < o.device_instance_id;
    }
  };

  // AcquisitionSession reference lifecycle (lifecycle_model.md section 2).
  // The seam is retained while stream and/or capture references remain live.
  // Both a profile-set (priming) and a capture attempt on a device without a
  // seam are first-class creation points; neither is a fallback for the other.
  //
  // Which claimant a seam reference belongs to. The three are independent:
  // release must decrement the same claimant that retained, never "any".
  // Shared with the other platform-backed providers so the decisions taken
  // from these counts live in one place (imaging/api/acquisition_seam_claims.h)
  // and are exercised host-native.
  using SeamClaimant = cambang::SeamClaimant;

  // Callable from any thread and under any of the provider's locks: the counts
  // live on DeviceBackend as atomics precisely so session realization, which
  // runs under configure_mutex and may not take state_mutex_, can consult them.
  // See the DeviceBackend declaration for that ordering constraint.
  void retain_acquisition_seam_for_capture_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend);
  void retain_acquisition_seam_for_stream_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend);
  void retain_acquisition_seam_for_capture_parent_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend);
  void release_acquisition_seam_for_capture_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend);
  void release_acquisition_seam_for_stream_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend);
  void release_acquisition_seam_for_capture_parent_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend);

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
  // Rebuilding a session destroys the seam and everything running on it, so a
  // rebuild is refused while a claimant OTHER than the requester holds it:
  // ERR_PLATFORM_CONSTRAINT against a live stream (whose repeating request
  // would be cancelled), ERR_BUSY against another device capture. The
  // requester's own reference is discounted -- a capture reference is taken at
  // admission, so by the time the worker asks for realization the claim it is
  // realizing for is already counted.
  //
  // Priming never blocks: it is an anticipatory claim, and yielding the
  // geometry to real work is exactly what it is for.
  // flow_is_pilot selects the flow output's FORMAT. The pilot's frames are
  // acquired and deleted without ever being read, and Camera2 says an
  // AIMAGE_FORMAT_PRIVATE reader "is more efficient, compared with ... 
  // AIMAGE_FORMAT_YUV_420_888" when software access is not needed. A caller's
  // stream must stay YUV: its pixels are read and delivered to Core.
  //
  // It also changes which guaranteed stream combination the session is: two YUV
  // targets is only guaranteed at FULL level or with BURST capability, whereas
  // PRIV+YUV is guaranteed from LEGACY upward.
  ProviderResult ensure_session_configured_(
      SeamClaimant requester,
      bool flow_is_pilot,
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
  // purpose/capture_id are telemetry only: the burst-collect trace previously
  // carried neither a device nor a purpose, so a two-member rig emitted
  // indistinguishable lines and metering could not be told from members.
  bool capture_burst_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend,
      uint32_t width,
      uint32_t height,
      uint32_t format_fourcc,
      const std::vector<MemberRequestSpec>& specs,
      std::vector<CapturedMemberFrame>& out_frames,
      const char* purpose,
      uint64_t capture_id) noexcept;

  // Single auto-exposed capture whose realized exposure/sensitivity become the
  // frozen reference every manual bracket member is derived from. Its image is
  // discarded: including it in the bundle would put one member a full
  // submission round-trip away from the rest, which is the temporal spread the
  // burst exists to remove.
  bool meter_manual_baseline_(
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend,
      uint64_t capture_id,
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

  // Clears the observed AF state so a wait cannot be satisfied by a settled
  // value left over from a previous capture.
  // The one place a seam's output set is decided.
  //
  // Every realization goes through here so the output set is a property of the
  // device's state, not of whichever claimant happened to ask first. Before
  // this existed, engage realized a template-geometry seam, the retained
  // profile replaced it, and the first capture replaced that again -- three
  // native ACameraCaptureSessions for one capture, two of them alive for
  // milliseconds.
  //
  // The set is always: PILOT + STILL, plus the caller's stream when one exists.
  // The pilot is unconditional because an AcquisitionSession that cannot
  // produce frames cannot serve a still capture (measured on two vendors), so
  // a seam without one is not an acquisition seam.
  // flow_width/flow_height override the flow output's geometry. Used by the
  // stream path, which realizes the seam BEFORE its StreamProduction exists and
  // so cannot be discovered from device state.
  ProviderResult ensure_seam_realized_(
      SeamClaimant claimant,
      const std::shared_ptr<camera2_detail::DeviceBackend>& backend,
      uint32_t still_width,
      uint32_t still_height,
      uint32_t flow_width = 0,
      uint32_t flow_height = 0);

  // Runs the pilot stream flow and stops it again. Returns whether AE
  // reported convergence within the wait -- which some devices never do; see
  // the caller for why that is not treated as failure.
  bool start_pilot_stream_(const std::shared_ptr<camera2_detail::DeviceBackend>& backend,
                             uint64_t capture_id) noexcept;

  void reset_observed_af_state_(
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
