#pragma once

#include <atomic>
#include <cstdint>

namespace cambang {

struct ResourceAggregateSnapshot final {
  uint64_t framebuffer_lease_current = 0;
  uint64_t framebuffer_lease_total_created = 0;
  uint64_t framebuffer_lease_total_released = 0;
  uint64_t framebuffer_lease_peak_current = 0;
  uint64_t retained_gpu_backing_current = 0;
  uint64_t retained_gpu_backing_total_created = 0;
  uint64_t retained_gpu_backing_total_released = 0;
  uint64_t retained_gpu_backing_peak_current = 0;
};

class ResourceAggregateTelemetry final {
public:
  void lease_created() noexcept;
  void lease_released() noexcept;
  void retained_gpu_backing_created() noexcept;
  void retained_gpu_backing_released() noexcept;
  ResourceAggregateSnapshot snapshot() const noexcept;

private:
  static void update_peak(std::atomic<uint64_t>& peak, uint64_t current) noexcept;
  static void decrement_if_positive(std::atomic<uint64_t>& current) noexcept;
  std::atomic<uint64_t> framebuffer_lease_current_{0};
  std::atomic<uint64_t> framebuffer_lease_total_created_{0};
  std::atomic<uint64_t> framebuffer_lease_total_released_{0};
  std::atomic<uint64_t> framebuffer_lease_peak_current_{0};
  std::atomic<uint64_t> retained_gpu_backing_current_{0};
  std::atomic<uint64_t> retained_gpu_backing_total_created_{0};
  std::atomic<uint64_t> retained_gpu_backing_total_released_{0};
  std::atomic<uint64_t> retained_gpu_backing_peak_current_{0};
};

ResourceAggregateTelemetry& global_resource_aggregate_telemetry() noexcept;

} // namespace cambang
