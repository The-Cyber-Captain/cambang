#pragma once
// ProviderBroker is the Core-bound facade type for camera provisioning.
//
// Locked invariants:
// - Core binds to exactly one provider instance (the broker).
// - Runtime selection between platform-backed and synthetic occurs via provider_mode.
// - Switching provider_mode requires teardown/restart (selection is latched at initialize()).
// - No ABI changes: ICameraProvider remains unchanged; broker implements it.

#include <cstdint>
#include <memory>

#include "imaging/api/icamera_provider.h"
#include "imaging/broker/mode.h"

namespace cambang {

class ProviderBroker final : public ICameraProvider {
public:
  ProviderBroker();
  ~ProviderBroker() override;

  const char* provider_name() const override;

  ProviderResult initialize(IProviderCallbacks* callbacks) override;
  ProviderResult enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) override;

  ProviderResult open_device(
      const std::string& hardware_id,
      uint64_t device_instance_id,
      uint64_t root_id) override;

  ProviderResult close_device(uint64_t device_instance_id) override;

  ProviderResult create_stream(const StreamRequest& req) override;
  ProviderResult destroy_stream(uint64_t stream_id) override;

  ProviderResult start_stream(uint64_t stream_id) override;
  ProviderResult stop_stream(uint64_t stream_id) override;

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

  RuntimeMode runtime_mode_latched() const noexcept { return mode_latched_; }

private:
  ProviderBroker(const ProviderBroker&) = delete;
  ProviderBroker& operator=(const ProviderBroker&) = delete;

  ProviderResult ensure_initialized_or_err_() const;
  ProviderResult ensure_active_or_err_() const;

  static RuntimeMode read_provider_mode_from_env_();

  std::unique_ptr<ICameraProvider> active_;
  IProviderCallbacks* callbacks_ = nullptr; // non-owning
  bool initialized_ = false;

  RuntimeMode mode_latched_ = RuntimeMode::platform_backed;
};

} // namespace cambang
