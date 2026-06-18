// src/provider/icamera_provider.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "provider_contract_datatypes.h"

namespace cambang {

// Provider→core callback sink.
// Provider MUST invoke these on a single serialized callback context.
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

  // Internal parent-context capability truth used by chooser/evaluator logic.
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
  // Core maintains the "one repeating stream per device instance" invariant.
  virtual ProviderResult create_stream(const StreamRequest& req) = 0;
  virtual ProviderResult destroy_stream(uint64_t stream_id) = 0;

  // Start/stop repeating flow for an existing stream.
  // Core supplies the effective profile + picture at start.
  virtual ProviderResult start_stream(
      uint64_t stream_id,
      const CaptureProfile& profile,
      const PictureConfig& picture) = 0;
  virtual ProviderResult stop_stream(uint64_t stream_id) = 0;

  // Narrow internal seam for Core-owned retained-plan evaluation.
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

  // Trigger a still capture for a device instance. A successful return is
  // admission/ownership transfer: the provider will later report terminal
  // capture success or failure through the provider callback/strand path.
  virtual ProviderResult trigger_capture(const CaptureRequest& req) = 0;

  // Trigger a grouped still-capture submission. Providers that do not override
  // this retain legacy per-device submission behaviour; providers with
  // coordinated multi-device capture support should override it so all member
  // device work is accepted as one provider submission.
  virtual ProviderResult trigger_capture_submission(const CaptureSubmission& submission) {
    if (submission.capture_id == 0 || submission.device_requests.empty()) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    for (const CaptureRequest& req : submission.device_requests) {
      const ProviderResult pr = trigger_capture(req);
      if (!pr.ok()) {
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
