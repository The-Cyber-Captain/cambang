#pragma once
// ProviderBroker is the Core-bound facade type for camera provisioning.
//
// Locked invariants:
// - Core binds to exactly one provider instance (the broker).
// - Runtime selection between platform-backed and synthetic occurs via provider_mode.
// - Switching provider_mode requires teardown/restart (selection is latched at initialize()).
// - No public Godot API changes: broker implements the internal provider interface.

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "imaging/api/icamera_provider.h"
#include "imaging/api/provider_access_status.h"
#include "imaging/broker/mode.h"

// Waiver (cpp_code_quality_policy.md Waivers section): ProviderBroker does
// NOT depend on the concrete SyntheticProvider class (that coupling was
// removed in an earlier ledger item) -- host_snapshot_types.h exists
// specifically to keep that class out of this header, see its own doc
// comment. What remains is type-level: SyntheticRole/TimingDriver/
// TimelineReconciliation/SyntheticProducerOutputFormMode (config.h) are
// used as data-member types with in-class default initializers below (not
// just function parameters), and stream_capability_downgrade_conditions_
// requested_/_latched_ and their capture-side counterparts are
// std::vector<SyntheticStreamCapabilityDowngradeCondition>/
// std::vector<SyntheticCaptureCapabilityDowngradeCondition> data members.
// The enums could be forward-declared (fixed underlying type), but the two
// vector members require those struct types to stay complete wherever
// ProviderBroker's implicit destructor is instantiated -- removing the
// includes would mean restructuring which members live in this header at
// all (e.g. moving their default-initialization into the out-of-line
// constructor), not just adding forward declarations. That is a real
// structural change to a header the whole GDE build depends on, not
// justified by this coupling alone. Revisit only alongside a real reason to
// touch this header's member layout, not as a standalone cleanup.
#include "imaging/synthetic/config.h"
#include "imaging/synthetic/host_snapshot_types.h"
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

  // Internal startup readiness preflight.  This answers whether a compiled and
  // selected provider may become operational now; it must not request OS
  // permission, show UI, or open camera hardware.
  static ProviderAccessStatus check_mode_access_readiness(RuntimeMode mode) noexcept;

  // Set the requested runtime mode for the next initialize() call.
  // Must be called before initialize(); initialize() latches the mode.
  ProviderResult set_runtime_mode_requested(RuntimeMode mode) noexcept;
  ProviderResult set_synthetic_role_requested(SyntheticRole role) noexcept;
  ProviderResult set_synthetic_timing_driver_requested(TimingDriver timing_driver) noexcept;
  ProviderResult set_synthetic_timeline_reconciliation_requested(TimelineReconciliation reconciliation) noexcept;
  ProviderResult set_synthetic_producer_output_form_mode_requested(SyntheticProducerOutputFormMode mode) noexcept;
  ProviderResult set_synthetic_stream_capability_downgrade_conditions_requested(
      std::vector<SyntheticStreamCapabilityDowngradeCondition> conditions) noexcept;
  ProviderResult set_synthetic_capture_capability_downgrade_conditions_requested(
      std::vector<SyntheticCaptureCapabilityDowngradeCondition> conditions) noexcept;

  const char* provider_name() const override;
  ProviderKind provider_kind() const noexcept override;


  StreamTemplate stream_template() const override;
  CaptureTemplate capture_template() const override;
  bool supports_stream_picture_updates() const noexcept override;
  bool supports_capture_picture_updates() const noexcept override;
  bool supports_multi_image_still_sequence() const noexcept override;

  ProducerBackingCapabilities stream_backing_capabilities(
      const CaptureProfile& profile,
      const PictureConfig& picture) const noexcept override;
  ProducerBackingCapabilities capture_backing_capabilities(
      const CaptureRequest& req) const noexcept override;
  ProducerBackingCapabilities stream_parent_context_backing_capabilities(
      uint64_t device_instance_id,
      uint64_t stream_id,
      StreamIntent intent,
      const CaptureProfile& profile,
      const PictureConfig& picture) noexcept override;
  ProducerBackingCapabilities capture_parent_context_backing_capabilities(
      uint64_t device_instance_id,
      const CaptureRequest& req) noexcept override;
  uint64_t stream_backing_plan_evaluation_settle_delay_ns() const noexcept override;
  uint64_t capture_backing_plan_evaluation_settle_delay_ns() const noexcept override;

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
  bool get_synthetic_metrics_snapshot_for_host(SyntheticMetricsSnapshot& out) const;
  bool get_synthetic_staged_rig_topology_for_host(std::vector<SyntheticStagedRigTopology>& out) const;

  // These latched values are written under active_provider_mutex_ inside
  // initialize(); readers on other threads must take the same lock rather
  // than reading the field directly (matches CoreCaptureCohortRegistry's
  // established noexcept-lock_guard accessor pattern elsewhere in the codebase).
  RuntimeMode runtime_mode_latched() const noexcept {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    return mode_latched_;
  }
  SyntheticRole synthetic_role_latched() const noexcept {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    return synthetic_role_latched_;
  }
  TimingDriver synthetic_timing_driver_latched() const noexcept {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    return timing_driver_latched_;
  }
  TimelineReconciliation synthetic_timeline_reconciliation_latched() const noexcept {
    std::lock_guard<std::mutex> lock(active_provider_mutex_);
    return timeline_reconciliation_latched_;
  }

#if defined(CAMBANG_INTERNAL_SMOKE) && CAMBANG_INTERNAL_SMOKE
  ProviderResult install_active_provider_for_smoke(
      std::unique_ptr<ICameraProvider> provider,
      IProviderCallbacks* callbacks);
#endif

private:
  ProviderBroker(const ProviderBroker&) = delete;
  ProviderBroker& operator=(const ProviderBroker&) = delete;

  ProviderResult ensure_initialized_or_err_() const;
  ProviderResult ensure_active_or_err_() const;
  void install_synthetic_timeline_request_dispatch_hook_locked_();
  void dispatch_synthetic_timeline_request_(const SyntheticScheduledEvent& ev);

  mutable std::mutex active_provider_mutex_;
  mutable std::mutex synthetic_timeline_dispatch_mutex_;
  std::vector<SyntheticScheduledEvent> deferred_synthetic_timeline_dispatches_;
  bool deferring_synthetic_timeline_dispatches_ = false;

  std::unique_ptr<ICameraProvider> active_;
  IProviderCallbacks* callbacks_ = nullptr; // non-owning
  bool initialized_ = false;

  RuntimeMode mode_requested_ = RuntimeMode::platform_backed;
  RuntimeMode mode_latched_ = RuntimeMode::platform_backed;
  SyntheticRole synthetic_role_requested_ = SyntheticRole::Nominal;
  TimingDriver timing_driver_requested_ = TimingDriver::VirtualTime;
  TimelineReconciliation timeline_reconciliation_requested_ = TimelineReconciliation::CompletionGated;
  SyntheticProducerOutputFormMode producer_output_form_mode_requested_ = SyntheticProducerOutputFormMode::Auto;
  std::vector<SyntheticStreamCapabilityDowngradeCondition>
      stream_capability_downgrade_conditions_requested_{};
  std::vector<SyntheticCaptureCapabilityDowngradeCondition>
      capture_capability_downgrade_conditions_requested_{};
  SyntheticRole synthetic_role_latched_ = SyntheticRole::Nominal;
  TimingDriver timing_driver_latched_ = TimingDriver::VirtualTime;
  TimelineReconciliation timeline_reconciliation_latched_ = TimelineReconciliation::CompletionGated;
  SyntheticProducerOutputFormMode producer_output_form_mode_latched_ = SyntheticProducerOutputFormMode::Auto;
  std::vector<SyntheticStreamCapabilityDowngradeCondition>
      stream_capability_downgrade_conditions_latched_{};
  std::vector<SyntheticCaptureCapabilityDowngradeCondition>
      capture_capability_downgrade_conditions_latched_{};
  std::function<void(const SyntheticScheduledEvent&)> synthetic_timeline_request_dispatch_hook_{};
};

} // namespace cambang
