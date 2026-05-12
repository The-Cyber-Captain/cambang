#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace cambang {

enum class TelemetryScope : uint32_t {
  STREAM = 0,
  ACQUISITION_SESSION = 1,
  DEVICE = 2,
  PROVIDER = 3,
  UNKNOWN = 4,
};

struct ScopedResourceTelemetry final {
  TelemetryScope telemetry_scope = TelemetryScope::UNKNOWN;
  uint64_t provider_native_id = 0;
  uint64_t device_instance_id = 0;
  uint64_t acquisition_session_id = 0;
  uint64_t stream_id = 0;

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
  void lease_created(const ScopedResourceTelemetry& key) noexcept;
  void lease_released(const ScopedResourceTelemetry& key) noexcept;
  void retained_gpu_backing_created(const ScopedResourceTelemetry& key) noexcept;
  void retained_gpu_backing_released(const ScopedResourceTelemetry& key) noexcept;
  std::vector<ScopedResourceTelemetry> snapshot() const noexcept;

private:
  struct Key final {
    TelemetryScope telemetry_scope = TelemetryScope::UNKNOWN;
    uint64_t provider_native_id = 0;
    uint64_t device_instance_id = 0;
    uint64_t acquisition_session_id = 0;
    uint64_t stream_id = 0;

    bool operator<(const Key& other) const noexcept;
  };

  struct Bucket final {
    std::atomic<uint64_t> framebuffer_lease_current{0};
    std::atomic<uint64_t> framebuffer_lease_total_created{0};
    std::atomic<uint64_t> framebuffer_lease_total_released{0};
    std::atomic<uint64_t> framebuffer_lease_peak_current{0};
    std::atomic<uint64_t> retained_gpu_backing_current{0};
    std::atomic<uint64_t> retained_gpu_backing_total_created{0};
    std::atomic<uint64_t> retained_gpu_backing_total_released{0};
    std::atomic<uint64_t> retained_gpu_backing_peak_current{0};
  };

  static Key make_key(const ScopedResourceTelemetry& key) noexcept;
  static void update_peak(std::atomic<uint64_t>& peak, uint64_t current) noexcept;
  static void decrement_if_positive(std::atomic<uint64_t>& current) noexcept;
  Bucket& get_or_create_bucket(const Key& key) noexcept;

  mutable std::mutex mutex_;
  std::map<Key, Bucket> buckets_;
};

ScopedResourceTelemetry make_stream_scoped_resource_telemetry(uint64_t stream_id) noexcept;
ScopedResourceTelemetry make_unknown_scoped_resource_telemetry() noexcept;
ResourceAggregateTelemetry& global_resource_aggregate_telemetry() noexcept;

} // namespace cambang
