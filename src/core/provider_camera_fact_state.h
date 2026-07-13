#pragma once

#include <cstdint>
#include <map>
#include <optional>

#include "core/camera_fact_types.h"

namespace cambang {

// Core-thread-only retained provider truth. It deliberately has no relation to
// external camera-description state or capture-admission context.
class ProviderCameraFactState final {
 public:
  struct CaptureImageKey {
    uint64_t capture_id = 0;
    uint64_t device_instance_id = 0;
    uint32_t image_member_index = 0;

    bool operator<(const CaptureImageKey& other) const noexcept {
      if (capture_id != other.capture_id) return capture_id < other.capture_id;
      if (device_instance_id != other.device_instance_id) {
        return device_instance_id < other.device_instance_id;
      }
      return image_member_index < other.image_member_index;
    }
  };

  bool replace_static(uint64_t device_instance_id, ProviderCameraFacts facts);
  bool replace_capture_image(CaptureImageKey key, ProviderCaptureImageFacts facts);
  void erase_device(uint64_t device_instance_id) noexcept;
  void clear() noexcept;

  const ProviderCameraFacts* find_static(uint64_t device_instance_id) const noexcept;
  const ProviderCaptureImageFacts* find_capture_image(CaptureImageKey key) const noexcept;

 private:
  static bool valid(const ProviderCameraFacts& facts) noexcept;
  static bool valid(const ProviderCaptureImageFacts& facts) noexcept;

  std::map<uint64_t, ProviderCameraFacts> static_by_device_;
  std::map<CaptureImageKey, ProviderCaptureImageFacts> capture_images_;
};

} // namespace cambang
