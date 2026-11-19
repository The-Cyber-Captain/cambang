#include "core/resource_aggregate_telemetry.h"
#include "core/core_acquisition_session_registry.h"
#include "core/core_device_registry.h"
#include "core/core_native_object_registry.h"
#include "core/core_stream_registry.h"

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

void ResourceAggregateTelemetry::lease_created(const ScopedResourceTelemetryKey& key) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  Bucket& bucket = buckets_[make_key(key)];
  const uint64_t current = bucket.framebuffer_lease_current.fetch_add(1, std::memory_order_relaxed) + 1;
  bucket.framebuffer_lease_total_created.fetch_add(1, std::memory_order_relaxed);
  update_peak(bucket.framebuffer_lease_peak_current, current);
}

void ResourceAggregateTelemetry::lease_released(const ScopedResourceTelemetryKey& key) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  Bucket& bucket = buckets_[make_key(key)];
  decrement_if_positive(bucket.framebuffer_lease_current);
  bucket.framebuffer_lease_total_released.fetch_add(1, std::memory_order_relaxed);
}

void ResourceAggregateTelemetry::retained_gpu_backing_created(const ScopedResourceTelemetryKey& key) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  Bucket& bucket = buckets_[make_key(key)];
  const uint64_t current = bucket.retained_gpu_backing_current.fetch_add(1, std::memory_order_relaxed) + 1;
  bucket.retained_gpu_backing_total_created.fetch_add(1, std::memory_order_relaxed);
  update_peak(bucket.retained_gpu_backing_peak_current, current);
}

void ResourceAggregateTelemetry::retained_gpu_backing_released(const ScopedResourceTelemetryKey& key) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  Bucket& bucket = buckets_[make_key(key)];
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
    s.phase = b.phase;
    s.creation_gen = b.creation_gen;
    s.created_ns = b.created_ns;
    s.destroyed_ns = b.destroyed_ns;
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

void ResourceAggregateTelemetry::reconcile_lifecycle(
    uint64_t now_ns,
    uint64_t current_gen,
    const CoreStreamRegistry* streams,
    const CoreAcquisitionSessionRegistry* acquisition_sessions,
    const CoreDeviceRegistry* devices,
    const CoreNativeObjectRegistry* native_objects) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [key, b] : buckets_) {
    if (b.creation_gen == 0) {
      b.creation_gen = current_gen;
      b.created_ns = now_ns;
      b.phase = 1;
    }
    const uint64_t fbl_cur = b.framebuffer_lease_current.load(std::memory_order_relaxed);
    const uint64_t fbl_new = b.framebuffer_lease_total_created.load(std::memory_order_relaxed);
    const uint64_t fbl_rel = b.framebuffer_lease_total_released.load(std::memory_order_relaxed);
    const uint64_t gpu_cur = b.retained_gpu_backing_current.load(std::memory_order_relaxed);
    const uint64_t gpu_new = b.retained_gpu_backing_total_created.load(std::memory_order_relaxed);
    const uint64_t gpu_rel = b.retained_gpu_backing_total_released.load(std::memory_order_relaxed);
    const bool balanced = (fbl_cur == 0 && gpu_cur == 0 && fbl_new == fbl_rel && gpu_new == gpu_rel);
    const bool owner_ended = [&]() {
      switch (key.telemetry_scope) {
        case TelemetryScope::STREAM: {
          if (!streams || key.stream_id == 0) return false;
          const auto* rec = streams->find(key.stream_id);
          return rec == nullptr || !rec->created;
        }
        case TelemetryScope::ACQUISITION_SESSION: {
          if (!acquisition_sessions || key.acquisition_session_id == 0) return false;
          const auto& all = acquisition_sessions->all();
          auto it = all.find(key.acquisition_session_id);
          return it == all.end() || it->second.phase == CBLifecyclePhase::DESTROYED;
        }
        case TelemetryScope::DEVICE: {
          if (!devices || key.device_instance_id == 0) return false;
          const auto* rec = devices->find(key.device_instance_id);
          return rec == nullptr || !rec->open;
        }
        case TelemetryScope::PROVIDER: {
          if (!native_objects || key.provider_native_id == 0) return false;
          const auto& all = native_objects->all();
          auto it = all.find(key.provider_native_id);
          return it == all.end() || it->second.destroyed;
        }
        case TelemetryScope::UNKNOWN:
        default:
          return false;
      }
    }();
    if (owner_ended && balanced) {
      b.phase = 3;
      b.destroyed_ns = now_ns;
      if (b.destroyed_integration_ns == 0) {
        b.destroyed_integration_ns = now_ns;
      }
    } else {
      b.phase = 1;
      b.destroyed_ns = 0;
      b.destroyed_integration_ns = 0;
    }
  }
}

size_t ResourceAggregateTelemetry::retire_destroyed_older_than(uint64_t now_ns, uint64_t retention_window_ns) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  size_t retired = 0;
  for (auto it = buckets_.begin(); it != buckets_.end();) {
    auto& b = it->second;
    const uint64_t fbl_cur = b.framebuffer_lease_current.load(std::memory_order_relaxed);
    const uint64_t fbl_new = b.framebuffer_lease_total_created.load(std::memory_order_relaxed);
    const uint64_t fbl_rel = b.framebuffer_lease_total_released.load(std::memory_order_relaxed);
    const uint64_t gpu_cur = b.retained_gpu_backing_current.load(std::memory_order_relaxed);
    const uint64_t gpu_new = b.retained_gpu_backing_total_created.load(std::memory_order_relaxed);
    const uint64_t gpu_rel = b.retained_gpu_backing_total_released.load(std::memory_order_relaxed);
    const bool balanced = (fbl_cur == 0 && gpu_cur == 0 && fbl_new == fbl_rel && gpu_new == gpu_rel);
    if (b.phase != 3 || !balanced || b.destroyed_integration_ns == 0 || b.destroyed_integration_ns > now_ns || (now_ns - b.destroyed_integration_ns) < retention_window_ns) {
      ++it;
      continue;
    }
    it = buckets_.erase(it);
    ++retired;
  }
  return retired;
}

std::optional<uint64_t> ResourceAggregateTelemetry::next_retirement_delay_ns(uint64_t now_ns, uint64_t retention_window_ns) const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  std::optional<uint64_t> min_delay;
  for (const auto& [_, b] : buckets_) {
    if (b.phase != 3 || b.destroyed_integration_ns == 0) continue;
    const uint64_t retire_at = b.destroyed_integration_ns + retention_window_ns;
    const uint64_t delay = retire_at > now_ns ? (retire_at - now_ns) : 0;
    if (!min_delay.has_value() || delay < *min_delay) min_delay = delay;
  }
  return min_delay;
}

void ResourceAggregateTelemetry::clear() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = buckets_.begin(); it != buckets_.end();) {
    const Bucket& bucket = it->second;
    const uint64_t framebuffer_current =
        bucket.framebuffer_lease_current.load(std::memory_order_relaxed);
    const uint64_t framebuffer_created =
        bucket.framebuffer_lease_total_created.load(std::memory_order_relaxed);
    const uint64_t framebuffer_released =
        bucket.framebuffer_lease_total_released.load(std::memory_order_relaxed);
    const uint64_t gpu_current =
        bucket.retained_gpu_backing_current.load(std::memory_order_relaxed);
    const uint64_t gpu_created =
        bucket.retained_gpu_backing_total_created.load(std::memory_order_relaxed);
    const uint64_t gpu_released =
        bucket.retained_gpu_backing_total_released.load(std::memory_order_relaxed);
    const bool balanced = framebuffer_current == 0 && gpu_current == 0 &&
                          framebuffer_created == framebuffer_released &&
                          gpu_created == gpu_released;
    if (balanced) {
      it = buckets_.erase(it);
    } else {
      ++it;
    }
  }
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
