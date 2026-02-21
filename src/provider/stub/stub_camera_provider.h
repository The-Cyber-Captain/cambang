#pragma once

#include <map>
#include <string>

#include "provider/icamera_provider.h"

namespace cambang {

class StubCameraProvider final : public ICameraProvider {
public:
  StubCameraProvider() = default;
  ~StubCameraProvider() override = default;

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

private:
  struct DeviceState {
    std::string hardware_id;
    uint64_t root_id = 0;
    bool open = false;
    // Core enforces 1 repeating stream per device instance; we track the current one (0 if none).
    uint64_t stream_id = 0;
  };

  struct StreamState {
    StreamRequest req{};
    bool created = false;
    bool started = false;
  };

  IProviderCallbacks* callbacks_ = nullptr;
  bool initialized_ = false;
  bool shutting_down_ = false;

  // Deterministic storage.
  std::map<uint64_t, DeviceState> devices_;   // key: device_instance_id
  std::map<uint64_t, StreamState> streams_;   // key: stream_id

  static constexpr const char* kStubHardwareId = "stub0";

  bool is_known_hardware_id(const std::string& hardware_id) const;
};

} // namespace cambang