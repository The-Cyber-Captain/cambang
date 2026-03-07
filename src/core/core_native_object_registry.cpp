#include "core/core_native_object_registry.h"

namespace cambang {

void CoreNativeObjectRegistry::on_native_object_created(uint64_t native_id,
                                                       uint32_t type,
                                                       uint64_t root_id,
                                                       uint64_t owner_device_instance_id,
                                                       uint64_t owner_stream_id,
                                                       uint64_t bytes_allocated,
                                                       uint32_t buffers_in_use,
                                                       uint64_t creation_gen,
                                                       uint64_t created_ns) {
  Record& r = records_[native_id];
  r.native_id = native_id;
  r.type = type;
  r.root_id = root_id;
  r.owner_device_instance_id = owner_device_instance_id;
  r.owner_stream_id = owner_stream_id;
  r.bytes_allocated = bytes_allocated;
  r.buffers_in_use = buffers_in_use;
  r.creation_gen = creation_gen;
  r.created = true;
  if (!r.created_ns_set) {
    r.created_ns = created_ns;
    r.created_ns_set = true;
  }
  if (!r.destroyed) {
    r.destroyed_ns = 0;
    r.destroyed_integration_ns = 0;
  }
  // If a provider reports "created" again for the same native_id, keep first created_ns.
  // (Scaffolding slice: no strict error accounting.)
}

void CoreNativeObjectRegistry::on_native_object_destroyed(uint64_t native_id,
                                                          uint64_t destroyed_ns,
                                                          uint64_t destroyed_integration_ns) {
  auto it = records_.find(native_id);
  if (it == records_.end()) {
    // Truth surface should reflect reality, including orphan destroys.
    // Create an entry so the snapshot can represent the orphan.
    Record r;
    r.native_id = native_id;
    r.destroyed = true;
    r.destroyed_ns = destroyed_ns;
    r.destroyed_integration_ns = destroyed_integration_ns;
    records_.emplace(native_id, r);
    return;
  }
  it->second.destroyed = true;
  it->second.destroyed_ns = destroyed_ns;
  it->second.destroyed_integration_ns = destroyed_integration_ns;
}

size_t CoreNativeObjectRegistry::retire_destroyed_older_than(uint64_t now_ns,
                                                             uint64_t retention_window_ns) {
  size_t retired = 0;
  for (auto it = records_.begin(); it != records_.end();) {
    const Record& r = it->second;
    if (!r.destroyed) {
      ++it;
      continue;
    }
    if (r.destroyed_integration_ns > now_ns) {
      ++it;
      continue;
    }
    const uint64_t age_ns = now_ns - r.destroyed_integration_ns;
    if (age_ns < retention_window_ns) {
      ++it;
      continue;
    }
    it = records_.erase(it);
    ++retired;
  }
  return retired;
}

std::optional<uint64_t> CoreNativeObjectRegistry::next_retirement_delay_ns(
    uint64_t now_ns,
    uint64_t retention_window_ns) const {
  std::optional<uint64_t> min_delay;
  for (const auto& [native_id, r] : records_) {
    (void)native_id;
    if (!r.destroyed) {
      continue;
    }
    const uint64_t retire_at_ns = r.destroyed_integration_ns + retention_window_ns;
    uint64_t delay = 0;
    if (retire_at_ns > now_ns) {
      delay = retire_at_ns - now_ns;
    }
    if (!min_delay.has_value() || delay < *min_delay) {
      min_delay = delay;
    }
  }
  return min_delay;
}

} // namespace cambang
