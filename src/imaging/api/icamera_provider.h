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
  virtual void on_capture_started(uint64_t capture_id) = 0;
  virtual void on_capture_completed(uint64_t capture_id) = 0;
  virtual void on_capture_failed(uint64_t capture_id, ProviderError error) = 0;

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

  // Whether stream-scoped picture updates are supported.
  // If false, core should return NotSupported deterministically without calling into the provider.
  virtual bool supports_stream_picture_updates() const noexcept = 0;

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

  // Stream-scoped picture update path.
  // Providers that do not support this must return ERR_NOT_SUPPORTED.
  virtual ProviderResult set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) = 0;

  // Trigger a still capture for a device instance (device capture or rig capture).
  virtual ProviderResult trigger_capture(const CaptureRequest& req) = 0;

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