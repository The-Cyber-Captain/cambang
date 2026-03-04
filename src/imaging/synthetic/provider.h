#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "imaging/api/icamera_provider.h"

#include "imaging/synthetic/config.h"
#include "imaging/synthetic/virtual_clock.h"

#include "pixels/pattern/cpu_packed_pattern_renderer.h"

namespace cambang {

// (No provider-wide pattern switching; picture is stream-scoped.)

// Deterministic synthetic provider.
//
// First landing:
// - nominal role only
// - virtual_time driver only (frames emitted only via advance(dt_ns))
//
// Future work:
// - real_time driver (single pump thread)
// - timeline role (scenario executor)
class SyntheticProvider final : public ICameraProvider {
public:
  explicit SyntheticProvider(const SyntheticProviderConfig& cfg);
  ~SyntheticProvider() override = default;

  const char* provider_name() const override { return "SyntheticProvider"; }

  StreamTemplate stream_template() const override;
  bool supports_stream_picture_updates() const noexcept override { return true; }

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

  // Synthetic-only virtual time driver (not part of ICameraProvider).
  // Emits any due frames.
  void advance(uint64_t dt_ns);

private:
  struct DeviceState {
    std::string hardware_id;
    uint64_t device_instance_id = 0;
    uint64_t root_id = 0;
    bool open = false;
    uint64_t native_id = 0;
  };

  struct StreamState {
    StreamRequest req{};
    bool created = false;
    bool started = false;
    bool producing = false;
    uint64_t frame_producer_native_id = 0;
    uint64_t frame_index = 0;
    uint64_t next_due_ns = 0;
    uint64_t native_id = 0;

    PictureConfig picture{};
    CpuPackedPatternRenderer renderer{};

    struct BufferSlot {
      SyntheticProvider* owner = nullptr;
      uint64_t stream_id = 0;
      std::vector<std::uint8_t> bytes;
      std::atomic<bool> in_use{false};
    };
    // Note: BufferSlot contains std::atomic, which is non-movable. Store slots
    // behind unique_ptr so the pool container can resize/move safely.
    std::vector<std::unique_ptr<BufferSlot>> pool;
    size_t pool_cursor = 0;
  };

  static void release_frame_(void* user, const FrameView* frame);
  bool is_known_hardware_id_(const std::string& hardware_id) const;

  uint64_t next_native_id_();
  void emit_native_create_device_(const DeviceState& d);
  void emit_native_destroy_(uint64_t native_id);

  void emit_due_frames_();
  void emit_one_frame_(StreamState& s, uint64_t scheduled_capture_ns);

private:
  SyntheticProviderConfig cfg_{};
  IProviderCallbacks* callbacks_ = nullptr;
  bool initialized_ = false;
  bool shutting_down_ = false;

  SyntheticVirtualClock clock_;

  std::map<uint64_t, DeviceState> devices_; // key: device_instance_id
  std::map<uint64_t, StreamState> streams_; // key: stream_id

  uint64_t native_id_seq_ = 1;
  uint64_t provider_native_id_ = 0;

  // Count of invalid preset requests observed at runtime (e.g. bad enum value).
  std::atomic<uint64_t> invalid_preset_requests_{0};
};

} // namespace cambang
