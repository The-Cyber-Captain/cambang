#include "core/resource_aggregate_telemetry.h"

namespace cambang {

bool ResourceAggregateTelemetry::Key::operator<(const Key& other) const noexcept {
  if (telemetry_scope != other.telemetry_scope) {
    return static_cast<uint32_t>(telemetry_scope) < static_cast<uint32_t>(other.telemetry_scope);
  }
  if (provider_native_id != other.provider_native_id) {
    return provider_native_id < other.provider_native_id;
  }
  if (device_instance_id != other.device_instance_id) {
    return device_instance_id < other.device_instance_id;
  }
  if (acquisition_session_id != other.acquisition_session_id) {
    return acquisition_session_id < other.acquisition_session_id;
  }
  return stream_id < other.stream_id;
}

ResourceAggregateTelemetry::Key ResourceAggregateTelemetry::make_key(const ScopedResourceTelemetryKey& key) noexcept {
  return Key{key.telemetry_scope, key.provider_native_id, key.device_instance_id, key.acquisition_session_id, key.stream_id};
}

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

ResourceAggregateTelemetry::Bucket& ResourceAggregateTelemetry::get_or_create_bucket(const Key& key) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return buckets_[key];
}

void ResourceAggregateTelemetry::lease_created(const ScopedResourceTelemetryKey& key) noexcept {
  Bucket& bucket = get_or_create_bucket(make_key(key));
  const uint64_t current = bucket.framebuffer_lease_current.fetch_add(1, std::memory_order_relaxed) + 1;
  bucket.framebuffer_lease_total_created.fetch_add(1, std::memory_order_relaxed);
  update_peak(bucket.framebuffer_lease_peak_current, current);
}

void ResourceAggregateTelemetry::lease_released(const ScopedResourceTelemetryKey& key) noexcept {
  Bucket& bucket = get_or_create_bucket(make_key(key));
  decrement_if_positive(bucket.framebuffer_lease_current);
  bucket.framebuffer_lease_total_released.fetch_add(1, std::memory_order_relaxed);
}

void ResourceAggregateTelemetry::retained_gpu_backing_created(const ScopedResourceTelemetryKey& key) noexcept {
  Bucket& bucket = get_or_create_bucket(make_key(key));
  const uint64_t current = bucket.retained_gpu_backing_current.fetch_add(1, std::memory_order_relaxed) + 1;
  bucket.retained_gpu_backing_total_created.fetch_add(1, std::memory_order_relaxed);
  update_peak(bucket.retained_gpu_backing_peak_current, current);
}

void ResourceAggregateTelemetry::retained_gpu_backing_released(const ScopedResourceTelemetryKey& key) noexcept {
  Bucket& bucket = get_or_create_bucket(make_key(key));
  decrement_if_positive(bucket.retained_gpu_backing_current);
  bucket.retained_gpu_backing_total_released.fetch_add(1, std::memory_order_relaxed);
}

std::vector<ScopedResourceTelemetryKey> ResourceAggregateTelemetry::snapshot() const noexcept {
  std::vector<ScopedResourceTelemetryKey> out;
  std::lock_guard<std::mutex> lock(mutex_);
  out.reserve(buckets_.size());
  for (const auto& [key, b] : buckets_) {
    ScopedResourceTelemetryKey s;
    s.telemetry_scope = key.telemetry_scope;
    s.provider_native_id = key.provider_native_id;
    s.device_instance_id = key.device_instance_id;
    s.acquisition_session_id = key.acquisition_session_id;
    s.stream_id = key.stream_id;
    s.framebuffer_lease_current = b.framebuffer_lease_current.load(std::memory_order_relaxed);
    s.framebuffer_lease_total_created = b.framebuffer_lease_total_created.load(std::memory_order_relaxed);
    s.framebuffer_lease_total_released = b.framebuffer_lease_total_released.load(std::memory_order_relaxed);
    s.framebuffer_lease_peak_current = b.framebuffer_lease_peak_current.load(std::memory_order_relaxed);
    s.retained_gpu_backing_current = b.retained_gpu_backing_current.load(std::memory_order_relaxed);
    s.retained_gpu_backing_total_created = b.retained_gpu_backing_total_created.load(std::memory_order_relaxed);
    s.retained_gpu_backing_total_released = b.retained_gpu_backing_total_released.load(std::memory_order_relaxed);
    s.retained_gpu_backing_peak_current = b.retained_gpu_backing_peak_current.load(std::memory_order_relaxed);
    out.push_back(s);
  }
  return out;
}

ScopedResourceTelemetryKey make_stream_scoped_resource_telemetry(uint64_t stream_id) noexcept {
  ScopedResourceTelemetryKey key;
  key.telemetry_scope = stream_id == 0 ? TelemetryScope::UNKNOWN : TelemetryScope::STREAM;
  key.stream_id = stream_id;
  return key;
}

ScopedResourceTelemetryKey make_acquisition_session_scoped_resource_telemetry(uint64_t acquisition_session_id) noexcept {
  ScopedResourceTelemetryKey key;
  key.telemetry_scope = acquisition_session_id == 0 ? TelemetryScope::UNKNOWN : TelemetryScope::ACQUISITION_SESSION;
  key.acquisition_session_id = acquisition_session_id;
  return key;
}

ScopedResourceTelemetryKey make_framebuffer_lease_scoped_resource_telemetry_key(
    uint64_t stream_id,
    uint64_t acquisition_session_id) noexcept {
  if (stream_id != 0) {
    return make_stream_scoped_resource_telemetry(stream_id);
  }
  if (acquisition_session_id != 0) {
    return make_acquisition_session_scoped_resource_telemetry(acquisition_session_id);
  }
  return make_unknown_scoped_resource_telemetry();
}

ScopedResourceTelemetryKey make_unknown_scoped_resource_telemetry() noexcept {
  ScopedResourceTelemetryKey key;
  key.telemetry_scope = TelemetryScope::UNKNOWN;
  return key;
}

ResourceAggregateTelemetry& global_resource_aggregate_telemetry() noexcept {
  static ResourceAggregateTelemetry telemetry;
  return telemetry;
}

} // namespace cambang
