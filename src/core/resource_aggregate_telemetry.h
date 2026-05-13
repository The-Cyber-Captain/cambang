#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace cambang { class CoreStreamRegistry; class CoreAcquisitionSessionRegistry; class CoreDeviceRegistry; class CoreNativeObjectRegistry; }

namespace cambang {

enum class TelemetryScope : uint32_t {
  STREAM = 0,
  ACQUISITION_SESSION = 1,
  DEVICE = 2,
  PROVIDER = 3,
  UNKNOWN = 4,
};

struct ScopedResourceTelemetryKey final {
  uint32_t phase = 1; // LIVE
  uint64_t creation_gen = 0;
  uint64_t created_ns = 0;
  uint64_t destroyed_ns = 0;
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
  void lease_created(const ScopedResourceTelemetryKey& key) noexcept;
  void lease_released(const ScopedResourceTelemetryKey& key) noexcept;
  void retained_gpu_backing_created(const ScopedResourceTelemetryKey& key) noexcept;
  void retained_gpu_backing_released(const ScopedResourceTelemetryKey& key) noexcept;
  std::vector<ScopedResourceTelemetryKey> snapshot() const noexcept;
  void reconcile_lifecycle(uint64_t now_ns,
                           uint64_t current_gen,
                           const CoreStreamRegistry* streams,
                           const CoreAcquisitionSessionRegistry* acquisition_sessions,
                           const CoreDeviceRegistry* devices,
                           const CoreNativeObjectRegistry* native_objects) noexcept;
  size_t retire_destroyed_older_than(uint64_t now_ns, uint64_t retention_window_ns) noexcept;
  std::optional<uint64_t> next_retirement_delay_ns(uint64_t now_ns, uint64_t retention_window_ns) const noexcept;
  void clear() noexcept;

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
    uint32_t phase = 1; // LIVE
    uint64_t creation_gen = 0;
    uint64_t created_ns = 0;
    uint64_t destroyed_ns = 0;
    uint64_t destroyed_integration_ns = 0;
  };

  static Key make_key(const ScopedResourceTelemetryKey& key) noexcept;
  static void update_peak(std::atomic<uint64_t>& peak, uint64_t current) noexcept;
  static void decrement_if_positive(std::atomic<uint64_t>& current) noexcept;
  Bucket& get_or_create_bucket(const Key& key) noexcept;

  mutable std::mutex mutex_;
  std::map<Key, Bucket> buckets_;
};

ScopedResourceTelemetryKey make_stream_scoped_resource_telemetry(uint64_t stream_id) noexcept;
ScopedResourceTelemetryKey make_acquisition_session_scoped_resource_telemetry(uint64_t acquisition_session_id) noexcept;
ScopedResourceTelemetryKey make_unknown_scoped_resource_telemetry() noexcept;
ScopedResourceTelemetryKey make_framebuffer_lease_scoped_resource_telemetry_key(
    uint64_t stream_id,
    uint64_t acquisition_session_id) noexcept;
ResourceAggregateTelemetry& global_resource_aggregate_telemetry() noexcept;

} // namespace cambang
