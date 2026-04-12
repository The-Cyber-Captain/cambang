#pragma once
// ProviderBroker is the Core-bound facade type for camera provisioning.
//
// Locked invariants:
// - Core binds to exactly one provider instance (the broker).
// - Runtime selection between platform-backed and synthetic occurs via provider_mode.
// - Switching provider_mode requires teardown/restart (selection is latched at initialize()).
// - No ABI changes: ICameraProvider remains unchanged; broker implements it.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "imaging/api/icamera_provider.h"
#include "imaging/broker/mode.h"
#include "imaging/synthetic/config.h"
#include "imaging/synthetic/builtin_scenario_library.h"
#include "imaging/synthetic/scenario_model.h"
#include "imaging/synthetic/scenario.h"

namespace cambang {

class ProviderBroker final : public ICameraProvider {
public:
  ProviderBroker();
  ~ProviderBroker() override;

  // Single authoritative build-capability query for runtime mode selection.
  // CamBANGServer should call this proactively when users request a mode,
  // and again defensively on start().
  static ProviderResult check_mode_supported_in_build(RuntimeMode mode) noexcept;

  // Set the requested runtime mode for the next initialize() call.
  // Must be called before initialize(); initialize() latches the mode.
  ProviderResult set_runtime_mode_requested(RuntimeMode mode) noexcept;
  ProviderResult set_synthetic_role_requested(SyntheticRole role) noexcept;
  ProviderResult set_synthetic_timing_driver_requested(TimingDriver timing_driver) noexcept;
  ProviderResult set_synthetic_timeline_reconciliation_requested(TimelineReconciliation reconciliation) noexcept;

  const char* provider_name() const override;
  ProviderKind provider_kind() const noexcept override;


  StreamTemplate stream_template() const override;
  CaptureTemplate capture_template() const override;
  bool supports_stream_picture_updates() const noexcept override;
  bool supports_capture_picture_updates() const noexcept override;

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

  ProviderResult set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) override;
  ProviderResult set_capture_picture_config(uint64_t device_instance_id, const PictureConfig& picture) override;

  ProviderResult trigger_capture(const CaptureRequest& req) override;
  ProviderResult abort_capture(uint64_t capture_id) override;

  ProviderResult apply_camera_spec_patch(
      const std::string& hardware_id,
      uint64_t new_camera_spec_version,
      SpecPatchView patch) override;

  ProviderResult apply_imaging_spec_patch(
      uint64_t new_imaging_spec_version,
      SpecPatchView patch) override;

  ProviderResult shutdown() override;

  // ---- Virtual-time helper (not part of ICameraProvider) ----
  // Drives virtual time for backends that require an external pump (stub, synthetic
  // virtual_time). Returns true if the active backend consumed the tick.
  bool try_tick_virtual_time(uint64_t dt_ns);
  ProviderResult set_timeline_scenario_for_host(const SyntheticTimelineScenario& scenario);
  ProviderResult set_timeline_canonical_scenario_for_host(const SyntheticCanonicalScenario& scenario);
  ProviderResult select_timeline_builtin_scenario_for_host(const std::string& scenario_name);
  ProviderResult load_timeline_canonical_scenario_from_json_text_for_host(const std::string& text, std::string* error = nullptr);
  ProviderResult load_timeline_canonical_scenario_from_json_file_for_host(const std::string& path, std::string* error = nullptr);
  ProviderResult start_timeline_scenario_for_host();
  ProviderResult stop_timeline_scenario_for_host();
  ProviderResult set_timeline_scenario_paused_for_host(bool paused);
  ProviderResult advance_timeline_for_host(uint64_t dt_ns);
  ProviderResult set_timeline_reconciliation_for_host(TimelineReconciliation reconciliation);
  void set_synthetic_timeline_request_dispatch_hook(std::function<void(const SyntheticScheduledEvent&)> hook);

  RuntimeMode runtime_mode_latched() const noexcept { return mode_latched_; }
  SyntheticRole synthetic_role_latched() const noexcept { return synthetic_role_latched_; }
  TimingDriver synthetic_timing_driver_latched() const noexcept { return timing_driver_latched_; }
  TimelineReconciliation synthetic_timeline_reconciliation_latched() const noexcept { return timeline_reconciliation_latched_; }

private:
  ProviderBroker(const ProviderBroker&) = delete;
  ProviderBroker& operator=(const ProviderBroker&) = delete;

  ProviderResult ensure_initialized_or_err_() const;
  ProviderResult ensure_active_or_err_() const;


  std::unique_ptr<ICameraProvider> active_;
  IProviderCallbacks* callbacks_ = nullptr; // non-owning
  bool initialized_ = false;

  RuntimeMode mode_requested_ = RuntimeMode::platform_backed;
  RuntimeMode mode_latched_ = RuntimeMode::platform_backed;
  SyntheticRole synthetic_role_requested_ = SyntheticRole::Nominal;
  TimingDriver timing_driver_requested_ = TimingDriver::VirtualTime;
  TimelineReconciliation timeline_reconciliation_requested_ = TimelineReconciliation::CompletionGated;
  SyntheticRole synthetic_role_latched_ = SyntheticRole::Nominal;
  TimingDriver timing_driver_latched_ = TimingDriver::VirtualTime;
  TimelineReconciliation timeline_reconciliation_latched_ = TimelineReconciliation::CompletionGated;
  std::function<void(const SyntheticScheduledEvent&)> synthetic_timeline_request_dispatch_hook_{};
};

} // namespace cambang
