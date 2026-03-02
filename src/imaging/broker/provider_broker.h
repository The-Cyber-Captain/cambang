#pragma once

#include <memory>

#include "imaging/api/icamera_provider.h"
#include "imaging/broker/mode.h"

namespace cambang {

// ProviderBroker is the single Core-bound facade provider.
// It selects exactly one concrete provider implementation based on RuntimeMode
// (platform_backed vs synthetic) and delegates the full ICameraProvider contract.
//
// Policy (locked for this phase):
// - Runtime mode is resolved before initialize() completes.
// - Mode is frozen once initialize() succeeds (no switching while running).
// - Switching in future must occur across a Core stop->drain boundary (not implemented here).
class ProviderBroker final : public ICameraProvider {
public:
  ProviderBroker();
  ~ProviderBroker() override;

  ProviderBroker(const ProviderBroker&) = delete;
  ProviderBroker& operator=(const ProviderBroker&) = delete;

  const char* provider_name() const override;

  ProviderResult initialize(IProviderCallbacks* callbacks) override;
  ProviderResult enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) override;
  ProviderResult open_device(const std::string& hardware_id, uint64_t device_instance_id, uint64_t root_id) override;
  ProviderResult close_device(uint64_t device_instance_id) override;
  ProviderResult create_stream(const StreamRequest& req) override;
  ProviderResult destroy_stream(uint64_t stream_id) override;
  ProviderResult start_stream(uint64_t stream_id) override;
  ProviderResult stop_stream(uint64_t stream_id) override;
  ProviderResult trigger_capture(const CaptureRequest& req) override;
  ProviderResult abort_capture(uint64_t capture_id) override;
  ProviderResult apply_camera_spec_patch(const std::string& hardware_id,
                                        uint64_t new_camera_spec_version,
                                        SpecPatchView patch) override;
  ProviderResult apply_imaging_spec_patch(uint64_t new_imaging_spec_version,
                                         SpecPatchView patch) override;
  ProviderResult shutdown() override;

#if defined(CAMBANG_ENABLE_DEV_NODES)
  // Dev-only helper used by Godot dev nodes when stub is the selected provider.
  ProviderResult dev_emit_test_frames(uint64_t stream_id, uint32_t count);
#endif

private:
  ProviderResult ensure_selected_and_initialized_(IProviderCallbacks* callbacks);
  ProviderResult select_provider_(RuntimeMode mode);

  std::unique_ptr<ICameraProvider> active_;
  RuntimeMode active_mode_ = RuntimeMode::platform_backed;
  bool initialized_ = false;

  // A stable name string for logging.
  const char* name_ = "ProviderBroker";
};

} // namespace cambang
