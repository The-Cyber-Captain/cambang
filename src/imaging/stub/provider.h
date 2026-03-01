#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <string>

#include "pixels/pattern/cpu_packed_pattern_renderer.h"

#include "imaging/api/icamera_provider.h"

namespace cambang {

class StubProvider final : public ICameraProvider {
public:
  StubProvider() = default;
  ~StubProvider() override = default;

  const char* provider_name() const override;

// Test instrumentation (thread-safe).
uint64_t frames_emitted() const noexcept { return frames_emitted_.load(std::memory_order_relaxed); }
uint64_t frames_released() const noexcept { return frames_released_.load(std::memory_order_relaxed); }

  // Test-only helpers (not part of provider contract).
  void emit_test_frames(uint64_t stream_id, uint32_t count);
  void emit_fact_stream_stopped(uint64_t stream_id, ProviderError error_or_ok);

  // Introspection for smoke / scaffolding.
  bool shutting_down() const noexcept { return shutting_down_; }


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
    uint64_t frame_index = 0;
  };

  IProviderCallbacks* callbacks_ = nullptr;
  bool initialized_ = false;
  bool shutting_down_ = false;

  // Test instrumentation (thread-safe). Not part of the provider contract.
  std::atomic<uint64_t> frames_emitted_{0};
  std::atomic<uint64_t> frames_released_{0};

  // Deterministic storage.
  std::map<uint64_t, DeviceState> devices_;   // key: device_instance_id
  std::map<uint64_t, StreamState> streams_;   // key: stream_id

  // Pattern renderer (provider-agnostic). Not part of the provider contract.
  CpuPackedPatternRenderer pattern_renderer_;

  static void release_test_frame(void* user, const FrameView* frame);

static constexpr const char* kStubHardwareId = "stub0";

  bool is_known_hardware_id(const std::string& hardware_id) const;
};

} // namespace cambang