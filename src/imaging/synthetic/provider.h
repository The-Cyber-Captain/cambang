#pragma once

#include "imaging/api/icamera_provider.h"

namespace cambang {

// Minimal contract-compliant synthetic provider (plumbing seam only).
// Future phases add Pattern Module pixels + scenario replay.
class SyntheticProvider final : public ICameraProvider {
public:
  const char* provider_name() const override { return "SyntheticProvider"; }

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

private:
  IProviderCallbacks* callbacks_ = nullptr;
};

} // namespace cambang
