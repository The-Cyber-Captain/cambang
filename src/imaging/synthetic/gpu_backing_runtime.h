#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace cambang {

struct SyntheticGpuBackingRuntimeOps final {
  bool (*is_available)() noexcept = nullptr;
  bool (*realize_rgba8_global_gpu_roundtrip)(
      const uint8_t* src,
      uint32_t width,
      uint32_t height,
      uint32_t stride_bytes,
      std::vector<uint8_t>& out) noexcept = nullptr;
  std::shared_ptr<void> (*retain_primary_gpu_backing_rgba8)(
      const uint8_t* src,
      uint32_t width,
      uint32_t height,
      uint32_t stride_bytes) noexcept = nullptr;
  std::shared_ptr<void> (*create_stream_live_gpu_backing_rgba8)(
      uint32_t width,
      uint32_t height,
      uint32_t stride_bytes) noexcept = nullptr;
  bool (*update_stream_live_gpu_backing_rgba8)(
      const std::shared_ptr<void>& backing,
      const uint8_t* src,
      uint32_t width,
      uint32_t height,
      uint32_t stride_bytes) noexcept = nullptr;
  void (*release_stream_live_gpu_backing)(std::shared_ptr<void>& backing) noexcept = nullptr;
};

void set_synthetic_gpu_backing_runtime_ops(const SyntheticGpuBackingRuntimeOps* ops) noexcept;
void clear_synthetic_gpu_backing_runtime_ops() noexcept;

bool synthetic_gpu_backing_runtime_available() noexcept;
bool synthetic_gpu_backing_realize_rgba8_via_global_gpu(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes,
    std::vector<uint8_t>& out) noexcept;

std::shared_ptr<void> synthetic_gpu_backing_retain_primary_gpu_backing_rgba8(
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept;

std::shared_ptr<void> synthetic_gpu_backing_create_stream_live_gpu_backing_rgba8(
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept;
bool synthetic_gpu_backing_update_stream_live_gpu_backing_rgba8(
    const std::shared_ptr<void>& backing,
    const uint8_t* src,
    uint32_t width,
    uint32_t height,
    uint32_t stride_bytes) noexcept;
void synthetic_gpu_backing_release_stream_live_gpu_backing(std::shared_ptr<void>& backing) noexcept;

} // namespace cambang
