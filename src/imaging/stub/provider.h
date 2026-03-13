#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "pixels/pattern/cpu_packed_pattern_renderer.h"

#include "imaging/api/icamera_provider.h"
#include "imaging/api/provider_strand.h"

namespace cambang {

class StubProvider final : public ICameraProvider {
public:
  StubProvider() = default;
  ~StubProvider() override = default;

  const char* provider_name() const override;

  StreamTemplate stream_template() const override;
  bool supports_stream_picture_updates() const noexcept override { return true; }

// Test instrumentation (thread-safe).
uint64_t frames_emitted() const noexcept { return frames_emitted_.load(std::memory_order_relaxed); }
uint64_t frames_released() const noexcept { return frames_released_.load(std::memory_order_relaxed); }

  // Test-only helpers (not part of provider contract).
  void emit_test_frames(uint64_t stream_id, uint32_t count);
  void emit_fact_stream_stopped(uint64_t stream_id, ProviderError error_or_ok);

  // Virtual-time driver (not part of provider contract).
  // In GDE, CamBANGServer pumps this via Godot's _process() loop.
  void advance(uint64_t dt_ns);

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

  ProviderResult start_stream(
      uint64_t stream_id,
      const CaptureProfile& profile,
      const PictureConfig& picture) override;
  ProviderResult stop_stream(uint64_t stream_id) override;

  ProviderResult set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) override;

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
  CBProviderStrand strand_;
  struct DeviceState {
    std::string hardware_id;
    uint64_t root_id = 0;
    bool open = false;
    // Core enforces 1 repeating stream per device instance; we track the current one (0 if none).
    uint64_t stream_id = 0;
    uint64_t native_id = 0;
  };

  struct StreamState {
    StreamRequest req{};
    bool created = false;
    bool started = false;
    uint64_t frame_index = 0;
    uint64_t native_id = 0;
    uint64_t frame_producer_native_id = 0;

    // virtual_time scheduling (ns)
    uint64_t period_ns = 0;
    uint64_t next_frame_ns = 0;

    // Stream-owned picture config + renderer.
    PictureConfig picture{};
    CpuPackedPatternRenderer renderer{};

    struct BufferSlot {
      StubProvider* owner = nullptr;
      uint64_t stream_id = 0;
      std::vector<std::uint8_t> bytes;
      bool in_use = false;
    };
    std::vector<BufferSlot> pool;
    size_t pool_cursor = 0;
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

  // Count of invalid preset requests observed at runtime (e.g. bad enum value).
  std::atomic<uint64_t> invalid_preset_requests_{0};

  // virtual_time clock (ns since provider init). Only advanced via advance().
  uint64_t now_ns_ = 0;

  static void release_test_frame(void* user, const FrameView* frame);

static constexpr const char* kStubHardwareId = "stub0";

  bool is_known_hardware_id(const std::string& hardware_id) const;

  uint64_t alloc_native_id_(NativeObjectType type) const;
  void emit_native_created_(uint64_t native_id, NativeObjectType type, uint64_t root_id, uint64_t owner_device_id, uint64_t owner_stream_id, uint64_t owner_provider_native_id, uint64_t owner_rig_id);
  void emit_native_destroyed_(uint64_t native_id);

  uint64_t provider_native_id_ = 0;
};

} // namespace cambang
