#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

// One native plane of a still image member, for the GPU compute sidecar
// (FrameView::gpu_compute_sidecar). `data` points at the plane's first row;
// rows are `stride_bytes` apart and at least shape.width * bytes-per-sample
// wide.
struct SyntheticGpuComputeSidecarPlane final {
  const uint8_t* data = nullptr;
  uint32_t stride_bytes = 0;
  GpuBackingPlaneDescriptor shape{};
};

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
      uint64_t stream_id,
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
  bool (*can_materialize_to_image)(const std::shared_ptr<void>& backing) noexcept = nullptr;
  bool (*take_update_timing_stats)(
      uint64_t& upload_copy_calls,
      uint64_t& upload_copy_total_ns,
      uint64_t& upload_copy_max_ns,
      uint64_t& texture_update_calls,
      uint64_t& texture_update_total_ns,
      uint64_t& texture_update_max_ns,
      uint64_t& texture_update_skipped) noexcept = nullptr;
  bool (*peek_update_timing_stats)(
      uint64_t& upload_copy_calls,
      uint64_t& upload_copy_total_ns,
      uint64_t& upload_copy_max_ns,
      uint64_t& texture_update_calls,
      uint64_t& texture_update_total_ns,
      uint64_t& texture_update_max_ns,
      uint64_t& texture_update_skipped) noexcept = nullptr;
  // Frozen per-plane GPU copy of a still image member. Appended last so every
  // existing positional table keeps its meaning; absent means no sidecar.
  std::shared_ptr<void> (*create_capture_gpu_compute_sidecar)(
      const SyntheticGpuComputeSidecarPlane* planes,
      uint32_t plane_count) noexcept = nullptr;
};

// The table is non-owning and must remain alive until a matching clear has
// returned. Installation safely drains a different active table before
// publishing the replacement. Clear closes admission first, then waits for
// every call already admitted through this seam to return.
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
    uint64_t stream_id,
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
bool synthetic_gpu_backing_can_materialize_to_image(const std::shared_ptr<void>& backing) noexcept;
std::shared_ptr<void> synthetic_gpu_backing_create_capture_gpu_compute_sidecar(
    const SyntheticGpuComputeSidecarPlane* planes,
    uint32_t plane_count) noexcept;
bool synthetic_gpu_backing_take_update_timing_stats(
    uint64_t& upload_copy_calls,
    uint64_t& upload_copy_total_ns,
    uint64_t& upload_copy_max_ns,
    uint64_t& texture_update_calls,
    uint64_t& texture_update_total_ns,
    uint64_t& texture_update_max_ns,
    uint64_t& texture_update_skipped) noexcept;
bool synthetic_gpu_backing_peek_update_timing_stats(
    uint64_t& upload_copy_calls,
    uint64_t& upload_copy_total_ns,
    uint64_t& upload_copy_max_ns,
    uint64_t& texture_update_calls,
    uint64_t& texture_update_total_ns,
    uint64_t& texture_update_max_ns,
    uint64_t& texture_update_skipped) noexcept;

} // namespace cambang
