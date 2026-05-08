#include "core/resource_aggregate_telemetry.h"

namespace cambang {

void ResourceAggregateTelemetry::update_peak(std::atomic<uint64_t>& peak, uint64_t current) noexcept {
  uint64_t prev = peak.load(std::memory_order_relaxed);
  while (current > prev && !peak.compare_exchange_weak(prev, current, std::memory_order_relaxed)) {
  }
}

void ResourceAggregateTelemetry::decrement_if_positive(std::atomic<uint64_t>& current) noexcept {
  uint64_t prev = current.load(std::memory_order_relaxed);
  while (prev > 0 && !current.compare_exchange_weak(prev, prev - 1, std::memory_order_relaxed)) {
  }
}

void ResourceAggregateTelemetry::lease_created() noexcept {
  const uint64_t current = framebuffer_lease_current_.fetch_add(1, std::memory_order_relaxed) + 1;
  framebuffer_lease_total_created_.fetch_add(1, std::memory_order_relaxed);
  update_peak(framebuffer_lease_peak_current_, current);
}

void ResourceAggregateTelemetry::lease_released() noexcept {
  decrement_if_positive(framebuffer_lease_current_);
  framebuffer_lease_total_released_.fetch_add(1, std::memory_order_relaxed);
}

void ResourceAggregateTelemetry::retained_gpu_backing_created() noexcept {
  const uint64_t current = retained_gpu_backing_current_.fetch_add(1, std::memory_order_relaxed) + 1;
  retained_gpu_backing_total_created_.fetch_add(1, std::memory_order_relaxed);
  update_peak(retained_gpu_backing_peak_current_, current);
}

void ResourceAggregateTelemetry::retained_gpu_backing_released() noexcept {
  decrement_if_positive(retained_gpu_backing_current_);
  retained_gpu_backing_total_released_.fetch_add(1, std::memory_order_relaxed);
}

ResourceAggregateSnapshot ResourceAggregateTelemetry::snapshot() const noexcept {
  ResourceAggregateSnapshot s;
  s.framebuffer_lease_current = framebuffer_lease_current_.load(std::memory_order_relaxed);
  s.framebuffer_lease_total_created = framebuffer_lease_total_created_.load(std::memory_order_relaxed);
  s.framebuffer_lease_total_released = framebuffer_lease_total_released_.load(std::memory_order_relaxed);
  s.framebuffer_lease_peak_current = framebuffer_lease_peak_current_.load(std::memory_order_relaxed);
  s.retained_gpu_backing_current = retained_gpu_backing_current_.load(std::memory_order_relaxed);
  s.retained_gpu_backing_total_created = retained_gpu_backing_total_created_.load(std::memory_order_relaxed);
  s.retained_gpu_backing_total_released = retained_gpu_backing_total_released_.load(std::memory_order_relaxed);
  s.retained_gpu_backing_peak_current = retained_gpu_backing_peak_current_.load(std::memory_order_relaxed);
  return s;
}

ResourceAggregateTelemetry& global_resource_aggregate_telemetry() noexcept {
  static ResourceAggregateTelemetry telemetry;
  return telemetry;
}

} // namespace cambang
