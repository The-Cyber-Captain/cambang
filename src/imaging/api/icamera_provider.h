// src/provider/icamera_provider.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/camera_fact_types.h"
#include "imaging/api/acquisition_coexistence.h"
#include "provider_contract_datatypes.h"

namespace cambang {

// Default for ICameraProvider::capture_admission_watchdog_timeout_ns() (30s).
// Deliberately generous; see that method's doc comment.
inline constexpr uint64_t kDefaultCaptureAdmissionWatchdogTimeoutNs =
    30ull * 1000ull * 1000ull * 1000ull;

// Default for ICameraProvider::stream_reprovision_resume_timeout_ns() (5s).
// A session swap is a bounded control operation rather than an exposure, so the
// generosity here is for a loaded device and a slow HAL, not for the work
// itself. See that method's doc comment.
inline constexpr uint64_t kDefaultStreamReprovisionResumeTimeoutNs =
    5ull * 1000ull * 1000ull * 1000ull;

// Provider->core callback sink.
//
// ============================================================================
// CONTRACT (non-negotiable): Provider MUST invoke every method on this
// interface from a single serialized callback context -- i.e. never call two
// IProviderCallbacks methods concurrently from two different threads, and
// always preserve the Provider's own real event order for calls whose
// relative order matters (e.g. on_capture_started() before the matching
// on_capture_completed()/on_capture_failed(), or on_device_opened() before
// any on_frame()/on_capture_*() for that device).
//
// WHY THIS IS LOAD-BEARING, NOT A SUGGESTION:
// Every method below is individually thread-safe at the transport layer
// (ProviderCallbackIngress marshals each call into a self-contained posted
// command; nothing here will crash or corrupt if called from multiple
// threads). But CoreThread's posted-task queues are FIFO only relative to a
// single POSTING thread -- concurrent posts from two different provider
// threads race for CoreThread's internal queue lock, and the relative order
// they land in the queue (and therefore the order Core processes them) is
// UNDEFINED. Core has no mechanism to detect or repair a misordering after
// the fact.
//
// CONCRETE FAILURE THIS CAUSES: suppose a real hardware SDK delivers
// completion notifications on one internal thread and streamed frames on a
// separate capture thread (extremely common: Camera2, V4L2, DirectShow, and
// most vendor SDKs all do this). If the Provider wires both straight through
// to IProviderCallbacks without first funneling them through its own single
// serialized dispatch queue, on_capture_completed() can race ahead of the
// on_capture_started() (or the frame delivery) it depends on. Core will then
// process a terminal capture fact for a capture it never saw admitted --
// this will not crash, will not log an error, and will not be caught by the
// existing synthetic-only test suite (SyntheticProvider disciplines itself
// through its own single-threaded CBProviderStrand delivery and therefore
// never exercises this path). It will only surface as silent, hard-to-
// reproduce state corruption under real hardware timing.
//
// WHAT PROVIDERS MUST DO: if your underlying SDK/driver delivers callbacks
// from more than one thread, you must serialize them yourself (e.g. through
// your own single-consumer strand/dispatch queue, the same pattern
// CBProviderStrand implements for the synthetic provider) BEFORE calling into
// any IProviderCallbacks method. "Serialized" means logically serialized
// (one call fully completes before the next begins, in your own real event
// order) -- it does not have to be the same OS thread ID for every call.
// ============================================================================
class IProviderCallbacks {
public:
  virtual ~IProviderCallbacks() = default;

  // ---- Core-issued services (sync, thread-safe) ----
  // Native object IDs MUST be issued by core to avoid clashes across provider instances.
  // Providers must store returned IDs and reuse them for later destroy events.
  //
  // This call is synchronous and must be safe to invoke from any provider thread.
  virtual uint64_t allocate_native_id(NativeObjectType type) = 0;

  // Core monotonic timebase in nanoseconds (session-relative), aligned with core snapshot
  // timestamps. This is OPTIONAL for providers; synthetic must not depend on wall-clock.
  //
  // Domain: CORE_MONOTONIC (see docs/provider_architecture.md §7.x).
  // This call is synchronous and must be safe to invoke from any provider thread.
  virtual uint64_t core_monotonic_now_ns() = 0;

  // Stream display-demand lease query (diagnostic-gated synthetic GPU update policy).
  // Returns true when a recent display-view access lease is active for stream_id.
  // This call is synchronous and must be safe to invoke from any provider thread.
  virtual bool is_stream_display_demand_active(uint64_t stream_id) = 0;

  // ---- Device lifecycle confirmations ----
  virtual void on_device_opened(uint64_t device_instance_id) = 0;
  virtual void on_device_closed(uint64_t device_instance_id) = 0;

  // ---- Stream lifecycle confirmations ----
  virtual void on_stream_created(uint64_t stream_id) = 0;
  virtual void on_stream_destroyed(uint64_t stream_id) = 0;

  virtual void on_stream_started(uint64_t stream_id) = 0;

  // If provider stops due to internal/platform failure, report a non-OK error.
  // Core determines public stop_reason based on its intent + this signal.
  virtual void on_stream_stopped(uint64_t stream_id, ProviderError error_or_ok) = 0;

  // ---- Still capture lifecycle ----
  virtual void on_capture_started(uint64_t capture_id, uint64_t device_instance_id) = 0;
  virtual void on_capture_completed(uint64_t capture_id, uint64_t device_instance_id) = 0;
  virtual void on_capture_failed(uint64_t capture_id, uint64_t device_instance_id, ProviderError error) = 0;

  // Optional source-neutral fact ingress. Providers may report facts after
  // the corresponding device/capture identity has become known to Core.
  virtual void on_camera_static_facts(uint64_t device_instance_id,
                                      ProviderCameraFacts facts) {
    (void)device_instance_id;
    (void)facts;
  }
  virtual void on_capture_image_facts(uint64_t capture_id,
                                      uint64_t device_instance_id,
                                      uint32_t image_member_index,
                                      ProviderCaptureImageFacts facts) {
    (void)capture_id;
    (void)device_instance_id;
    (void)image_member_index;
    (void)facts;
  }

  // ---- Frame delivery ----
  virtual void on_frame(const FrameView& frame) = 0;

  // ---- Error reporting (scoped) ----
  virtual void on_device_error(uint64_t device_instance_id, ProviderError error) = 0;
  virtual void on_stream_error(uint64_t stream_id, ProviderError error) = 0;

  // ---- Native object reporting (snapshot introspection) ----
  virtual void on_native_object_created(const NativeObjectCreateInfo& info) = 0;
  virtual void on_native_object_destroyed(const NativeObjectDestroyInfo& info) = 0;
};


// Runtime kind exposed for lightweight diagnostics (e.g. banners).
// This reflects the provider's active runtime truth without exposing broker internals.
enum class ProviderKind : uint8_t {
  unknown = 0,
  platform_backed = 1,
  synthetic = 2,
};

// Core-facing provider interface (platform backends implement this).
class ICameraProvider {
public:
  virtual ~ICameraProvider() = default;

  // Provider identity (for logs / diagnostics).
  virtual const char* provider_name() const = 0;

  // Provider active kind (for logs / diagnostics).
  virtual ProviderKind provider_kind() const noexcept = 0;

  // Provider default stream template (profile + picture). Core uses this for
  // stream creation-time defaulting.
  virtual StreamTemplate stream_template() const = 0;
  // Provider default capture template (profile + picture). Core/host uses this for
  // capture request defaulting.
  virtual CaptureTemplate capture_template() const = 0;

  // Whether stream-scoped picture updates are supported.
  // If false, core should return NotSupported deterministically without calling into the provider.
  virtual bool supports_stream_picture_updates() const noexcept = 0;
  // Whether capture-scoped picture updates are supported.
  virtual bool supports_capture_picture_updates() const noexcept = 0;
  // Whether provider can execute a still-capture sequence that includes
  // non-default images beyond the required default image.
  // This is an internal execution-capability seam for future multi-image still
  // requests; default-only still capture continues through trigger_capture().
  virtual bool supports_multi_image_still_sequence() const noexcept = 0;

  // Internal producer-backing capability advertisement for stream realization.
  // Backing capability is provider/runtime truth and is distinct from payload kind policy.
  virtual ProducerBackingCapabilities stream_backing_capabilities(
      const CaptureProfile& profile,
      const PictureConfig& picture) const noexcept {
    (void)profile;
    (void)picture;
    return ProducerBackingCapabilities{false, false};
  }

  // Internal producer-backing capability advertisement for still-capture realization.
  // Backing capability is provider/runtime truth and is distinct from payload kind policy.
  virtual ProducerBackingCapabilities capture_backing_capabilities(
      const CaptureRequest& req) const noexcept {
    (void)req;
    return ProducerBackingCapabilities{false, false};
  }

  // Internal native pixel-format capability advertisement.
  //
  // States which formats the provider can emit WITHOUT converting, in its own
  // preference order, and whether it will convert to packed RGBA/BGRA on
  // request. This is acquisition capability truth, distinct from producer
  // backing kind (CPU/GPU) and from result payload-kind policy.
  //
  // The default describes every provider in the tree today: packed RGBA/BGRA
  // native, conversion available. A provider whose backend delivers YUV should
  // override this to say so truthfully -- advertising a native format it does
  // not actually emit is a contract violation, not an optimization hint.
  //
  // Core currently reads these advertisements for format selection only where
  // a path implements the advertised format. Advertising a format does not by
  // itself enable retention, display, or materialization of it.
  virtual ProducerFormatCapabilities stream_format_capabilities(
      const CaptureProfile& profile,
      const PictureConfig& picture) const noexcept {
    (void)profile;
    (void)picture;
    return ProducerFormatCapabilities::packed_rgb_only();
  }

  // The configurations an ENDPOINT will accept.
  //
  // Keyed by hardware_id rather than device instance so it can be answered
  // without opening the camera: a caller choosing a resolution has not opened
  // anything yet, and making them open a device to find out what it supports
  // would invert the order of the decision.
  //
  // Distinct from the format-capability calls above, which answer "given this
  // profile, which formats can you emit". These answer "which profiles are
  // there", a question with no profile to be conditioned on.
  //
  // The default is NOT_THIS_PROVIDER, so a provider that has not implemented
  // enumeration yields no catalog rather than one for hardware that may not be
  // there. Two obligations, and the difference between them is load-bearing:
  //
  //   - NOT_THIS_PROVIDER for a hardware_id you do not own. An ingested
  //     description must not stand in for a camera that does not exist, and
  //     this answer is what stops it.
  //   - CANNOT_ENUMERATE for an endpoint you DO own but cannot list -- a
  //     backend needing the camera open to read its formats, say. A description
  //     may then supply the catalog, which is the point of that state.
  virtual ProviderProfileCatalog stream_profile_catalog(
      const std::string& hardware_id) const {
    (void)hardware_id;
    return ProviderProfileCatalog{};
  }

  virtual ProviderProfileCatalog capture_profile_catalog(
      const std::string& hardware_id) const {
    (void)hardware_id;
    return ProviderProfileCatalog{};
  }
  virtual ProducerFormatCapabilities capture_format_capabilities(
      const CaptureRequest& req) const noexcept {
    (void)req;
    return ProducerFormatCapabilities::packed_rgb_only();
  }

  // Device-scoped format capability.
  //
  // Format availability is a per-device fact, not a provider-wide one: two
  // cameras behind the same provider can offer different sets, so there is no
  // correct provider-wide answer for a heterogeneous provider. The
  // provider-wide call above remains the truthful outer envelope; this narrows
  // it to a specific device, exactly as the parent-context backing calls below
  // narrow backing capability.
  //
  // Defaults to the provider-wide answer, so a provider whose devices are
  // homogeneous need not override it.
  virtual ProducerFormatCapabilities stream_parent_context_format_capabilities(
      uint64_t device_instance_id,
      uint64_t stream_id,
      StreamIntent intent,
      const CaptureProfile& profile,
      const PictureConfig& picture) noexcept {
    (void)device_instance_id;
    (void)stream_id;
    (void)intent;
    return stream_format_capabilities(profile, picture);
  }

  virtual ProducerFormatCapabilities capture_parent_context_format_capabilities(
      uint64_t device_instance_id,
      const CaptureRequest& req) noexcept {
    (void)device_instance_id;
    return capture_format_capabilities(req);
  }

  // Internal parent-context capability truth used by parent-scoped backing-plan
  // evaluation.
  // These default to the provider/runtime envelope above; providers that can
  // narrow a specific owning context without changing the truthful outer
  // envelope should override them.
  virtual ProducerBackingCapabilities stream_parent_context_backing_capabilities(
      uint64_t device_instance_id,
      uint64_t stream_id,
      StreamIntent intent,
      const CaptureProfile& profile,
      const PictureConfig& picture) noexcept {
    (void)device_instance_id;
    (void)stream_id;
    (void)intent;
    return stream_backing_capabilities(profile, picture);
  }

  virtual ProducerBackingCapabilities capture_parent_context_backing_capabilities(
      uint64_t device_instance_id,
      const CaptureRequest& req) noexcept {
    (void)device_instance_id;
    return capture_backing_capabilities(req);
  }

  // What this device's backend can serve concurrently.
  //
  // Core asks before it arbitrates between a triggered capture and a repeating
  // stream, at profile-set, at stream start and at capture admission. The
  // provider answers what its backend can do; who yields is Core's decision and
  // is not encoded in the answer. See imaging/api/acquisition_coexistence.h for
  // the verdicts and why the question is asked of a set.
  //
  // MUST NOT touch the backend. Answer from characteristics cached at device
  // open; brief §2 forbids I/O in a capability query on the core thread.
  //
  // MUST agree with what the provider then does. A provider answering Coexist
  // and then refusing the capture is in violation, not merely unhelpful -- Core
  // has no other way to learn that a conflict existed, and a refusal it did not
  // predict is one it cannot attribute or report. provider_compliance_verify
  // binds this.
  //
  // The default answers Coexist unconditionally, which is truthful for every
  // provider with no backend constraint -- Synthetic and Stub, whose whole point
  // is to be the permissive reference. A provider whose backend does constrain
  // concurrent use must override it and say so; advertising coexistence it
  // cannot deliver is a contract violation, exactly as with format capabilities
  // above.
  virtual AcquisitionCoexistence acquisition_coexistence(
      uint64_t device_instance_id,
      const AcquisitionUseSet& proposed) noexcept {
    (void)device_instance_id;
    (void)proposed;
    return AcquisitionCoexistence::coexist();
  }

  // Small bounded delay before a newly realized or newly switched backing-plan
  // measurement is treated as representative for this provider/runtime.
  virtual uint64_t stream_backing_plan_evaluation_settle_delay_ns() const noexcept {
    return 0;
  }

  virtual uint64_t capture_backing_plan_evaluation_settle_delay_ns() const noexcept {
    return 0;
  }

  // Worst-case time Core should wait, after permitting a reprovision that
  // reported CoexistenceVerdict::Reconfigure, for that stream's frames to
  // resume before Core declares the stream failed.
  //
  // A reprovision gaps a stream deliberately and reports nothing while it does,
  // which is truthful -- and is also exactly how a permanent hang looks. Core
  // arms this bound so a reprovision that never restores the flow becomes a
  // reported failure rather than a stream that reads FLOWING forever and
  // delivers nothing. Nothing else in Core watches frame cadence, so without it
  // that outcome is invisible to every gate, snapshot and caller.
  //
  // Same rule as the capture watchdog below, for the same reason: only override
  // this with a value backed by real measured worst-case latency, never a guess.
  // Too short converts a slow-but-working reprovision into a false failure,
  // which is worse than waiting.
  //
  // Only consulted for a stream Core permitted a reprovision for. It is not a
  // general cadence watchdog and must not be treated as one.
  virtual uint64_t stream_reprovision_resume_timeout_ns() const noexcept {
    return kDefaultStreamReprovisionResumeTimeoutNs;
  }

  // Worst-case time Core should wait, after a trigger_capture()/
  // trigger_capture_submission() admission, for that device's terminal
  // capture fact (on_capture_completed/on_capture_failed) to arrive before
  // Core's own capture-admission watchdog declares it timed out
  // (ProviderError::ERR_TIMEOUT). The default is deliberately generous: Core
  // cannot know a specific provider's real latency, and a timeout that is too
  // short would produce a false-positive failure for hardware that is simply
  // still working -- worse than staying silently pending. Only override this
  // with a value backed by real measured worst-case latency, never a guess.
  virtual uint64_t capture_admission_watchdog_timeout_ns() const noexcept {
    return kDefaultCaptureAdmissionWatchdogTimeoutNs;
  }

  // Core supplies callback sink. Provider retains only a raw pointer (no ownership).
  // Provider MUST call callbacks on a single serialized callback context.
  virtual ProviderResult initialize(IProviderCallbacks* callbacks) = 0;

  // Enumerate platform camera endpoints (hardware_ids).
  virtual ProviderResult enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) = 0;

  // Open/close a hardware endpoint into a core-issued runtime instance_id/root_id lineage.
  virtual ProviderResult open_device(
      const std::string& hardware_id,
      uint64_t device_instance_id,
      uint64_t root_id) = 0;

  virtual ProviderResult close_device(uint64_t device_instance_id) = 0;

  // Create/destroy a repeating stream object for a device instance.
  //
  // Core maintains the invariant that at most one stream per device instance is
  // ACTIVE (lifecycle_model.md 13). It does not limit how many stream RECORDS
  // exist: several may be created on one device, and only the transition to
  // started is arbitrated. An earlier wording here -- "one repeating stream per
  // device instance" -- did not say which of the two it meant, and both
  // platform providers read it as the object rather than the flow and refuse a
  // second create_stream() with ERR_BUSY, while SyntheticProvider permits it.
  // That divergence is provider-local and NOT sanctioned by this contract:
  // caller code that creates two streams on one device behaves differently
  // under synthetic and platform backing.
  //
  // A provider MAY still refuse a create its backend genuinely cannot hold,
  // which is a capability answer and belongs with the rest of them
  // (acquisition_coexistence.h) rather than being asserted here as a rule Core
  // is supposed to be keeping.
  virtual ProviderResult create_stream(const StreamRequest& req) = 0;
  virtual ProviderResult destroy_stream(uint64_t stream_id) = 0;

  // Start/stop repeating flow for an existing stream.
  // Core supplies the effective profile + picture at start.
  virtual ProviderResult start_stream(
      uint64_t stream_id,
      const CaptureProfile& profile,
      const PictureConfig& picture) = 0;
  virtual ProviderResult stop_stream(uint64_t stream_id) = 0;

  // Narrow internal seam for Core-owned parent-scoped backing-plan evaluation.
  // A successful return commits the requested retained-production plan for
  // subsequent frames from this created stream; providers must not emit a frame
  // synchronously from this call.
  virtual ProviderResult update_stream_retained_production_plan(
      uint64_t stream_id,
      CoreRetainedProductionPlan requested_retained_plan) {
    (void)stream_id;
    (void)requested_retained_plan;
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }

  // Stream-scoped picture update path.
  // Providers that do not support this must return ERR_NOT_SUPPORTED.
  virtual ProviderResult set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) = 0;
  // Capture-scoped picture update path (device-scoped retained capture picture).
  // Providers that do not support this must return ERR_NOT_SUPPORTED.
  virtual ProviderResult set_capture_picture_config(uint64_t device_instance_id, const PictureConfig& picture) = 0;

  // Optional explicit priming seam for still-capture parents.
  // A successful return means the provider has synchronized any provider-owned
  // primed acquisition-session seam for this device/request shape so Core can
  // avoid paying first-use session realization cost at trigger time.
  //
  // The operation must be idempotent for repeated equivalent requests and must
  // not fabricate capture-completed truth. Providers that cannot support this
  // safely should return ERR_NOT_SUPPORTED.
  virtual ProviderResult sync_capture_parent_priming(const CaptureRequest& req) {
    (void)req;
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }

  // Optional release of provider-owned capture-parent priming for a device.
  // Providers that implement sync_capture_parent_priming() should make this
  // idempotent and safe even when no primed seam is currently held.
  virtual ProviderResult release_capture_parent_priming(uint64_t device_instance_id) {
    (void)device_instance_id;
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }

  // Trigger a still capture for a device instance. A successful return is
  // admission/ownership transfer: the provider will later report terminal
  // capture success or failure through the provider callback/strand path.
  //
  // Abandonment obligation (brief §5.2). Giving up on a capture does not
  // cancel the backend's obligation to produce it: a platform may deliver the
  // payload later, sometimes only when the next request pushes its pipeline. A
  // payload delivered after its capture was abandoned must never be attributed
  // to a later capture on that device. Attribution is by accounting -- see
  // imaging/api/outstanding_payload_ledger.h -- never by acquisition mark,
  // which camera_fact_model.md §12.2 forbids as identity or freshness evidence
  // and which may legitimately be identical across simultaneously triggered
  // devices.
  virtual ProviderResult trigger_capture(const CaptureRequest& req) = 0;

  // Trigger a grouped still-capture submission. Providers that do not override
  // this retain legacy per-device submission behaviour; providers with
  // coordinated multi-device capture support should override it so all member
  // device work is accepted as one provider submission.
  //
  // This default is NOT atomic: it admits devices one at a time via
  // trigger_capture(), each of which is itself an admission/ownership
  // transfer. On a mid-loop failure it attempts a best-effort abort_capture()
  // of the already-admitted prefix before returning, but since abort_capture()
  // is itself best-effort/platform-dependent (may deterministically return
  // ERR_NOT_SUPPORTED), that rollback is not guaranteed. Callers must not
  // treat a failure return here as proof that no device was admitted.
  virtual ProviderResult trigger_capture_submission(const CaptureSubmission& submission) {
    // A device submission is named by capture_id (a Device Capture Id); a rig
    // submission by rig_capture_id, with capture_id 0 and each member carrying
    // its own Device Capture Id. See CaptureSubmission's field comments.
    const bool rig_submission =
        submission.origin == CaptureSubmissionOrigin::RIG_CAPTURE;
    const bool identified =
        rig_submission ? submission.rig_capture_id != 0 : submission.capture_id != 0;
    if (!identified || submission.device_requests.empty()) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    for (size_t i = 0; i < submission.device_requests.size(); ++i) {
      const CaptureRequest& req = submission.device_requests[i];
      const ProviderResult pr = trigger_capture(req);
      if (!pr.ok()) {
        // Roll back the already-admitted prefix under each member's OWN id.
        // Aborting the cohort id would name nothing the provider admitted.
        for (size_t j = 0; j < i; ++j) {
          (void)abort_capture(submission.device_requests[j].capture_id);
        }
        return pr;
      }
    }
    return ProviderResult::success();
  }

  // Best-effort abort for an in-flight capture (platform-dependent).
  // Providers that cannot abort should return ERR_NOT_SUPPORTED deterministically.
  virtual ProviderResult abort_capture(uint64_t capture_id) = 0;

  // Spec patch application hooks (core-validated). May be no-op for some providers.
  virtual ProviderResult apply_camera_spec_patch(
      const std::string& hardware_id,
      uint64_t new_camera_spec_version,
      SpecPatchView patch) = 0;

  virtual ProviderResult apply_imaging_spec_patch(
      uint64_t new_imaging_spec_version,
      SpecPatchView patch) = 0;

  // Deterministic shutdown: stop streams, close devices, release platform resources.
  virtual ProviderResult shutdown() = 0;
};

} // namespace cambang
