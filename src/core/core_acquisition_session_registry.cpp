#include "core/core_acquisition_session_registry.h"

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

bool CoreAcquisitionSessionRegistry::on_native_object_created(uint64_t native_id,
                                                              uint32_t type,
                                                              uint64_t owner_device_instance_id,
                                                              uint64_t created_ns,
                                                              uint32_t capture_width,
                                                              uint32_t capture_height,
                                                              uint32_t capture_format,
                                                              uint64_t capture_profile_version) {
  if (native_id == 0 || type != static_cast<uint32_t>(NativeObjectType::AcquisitionSession)) {
    return false;
  }

  AcquisitionSessionEntry& entry = sessions_[native_id];
  const bool was_known = (entry.acquisition_session_id != 0);
  const bool was_live = (entry.phase == CBLifecyclePhase::LIVE);
  const uint64_t prev_device_instance_id = entry.device_instance_id;

  bool changed = false;
  if (!was_known) {
    entry.acquisition_session_id = native_id;
    changed = true;
  }
  if (entry.device_instance_id != owner_device_instance_id) {
    entry.device_instance_id = owner_device_instance_id;
    changed = true;
  }
  if (entry.phase != CBLifecyclePhase::LIVE) {
    entry.phase = CBLifecyclePhase::LIVE;
    changed = true;
  }
  if (entry.started_ns == 0 && created_ns != 0) {
    entry.started_ns = created_ns;
    changed = true;
  }
  if (entry.ended_ns != 0) {
    entry.ended_ns = 0;
    changed = true;
  }
  if (entry.capture_width != capture_width) {
    entry.capture_width = capture_width;
    changed = true;
  }
  if (entry.capture_height != capture_height) {
    entry.capture_height = capture_height;
    changed = true;
  }
  if (entry.capture_format != capture_format) {
    entry.capture_format = capture_format;
    changed = true;
  }
  if (entry.capture_profile_version != capture_profile_version) {
    entry.capture_profile_version = capture_profile_version;
    changed = true;
  }

  if (was_live && prev_device_instance_id != 0 && prev_device_instance_id != owner_device_instance_id) {
    rebuild_device_live_index_(prev_device_instance_id);
  }
  rebuild_device_live_index_(owner_device_instance_id);
  return changed;
}

bool CoreAcquisitionSessionRegistry::on_native_object_destroyed(uint64_t native_id, uint64_t destroyed_ns) {
  if (native_id == 0) {
    return false;
  }

  auto session_it = sessions_.find(native_id);
  if (session_it == sessions_.end()) {
    return false;
  }

  bool changed = false;
  if (session_it->second.phase != CBLifecyclePhase::DESTROYED) {
    session_it->second.phase = CBLifecyclePhase::DESTROYED;
    changed = true;
  }
  if (session_it->second.ended_ns != destroyed_ns) {
    session_it->second.ended_ns = destroyed_ns;
    changed = true;
  }
  rebuild_device_live_index_(session_it->second.device_instance_id);
  return changed;
}

bool CoreAcquisitionSessionRegistry::on_capture_started(uint64_t device_instance_id,
                                                        uint64_t capture_id,
                                                        uint64_t started_ns,
                                                        uint32_t capture_width,
                                                        uint32_t capture_height,
                                                        uint32_t capture_format,
                                                        uint64_t capture_profile_version) {
  if (device_instance_id == 0 || capture_id == 0) {
    return false;
  }
  const uint64_t session_id = resolve_live_session_id_for_device_(device_instance_id);
  if (session_id == 0) {
    return false;
  }
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return false;
  }
  AcquisitionSessionEntry& entry = it->second;

  bool changed = false;
  ++entry.captures_triggered;
  changed = true;
  if (entry.last_capture_id != capture_id) {
    entry.last_capture_id = capture_id;
    changed = true;
  }
  if (entry.error_code != 0) {
    entry.error_code = 0;
    changed = true;
  }
  if (entry.capture_width != capture_width) {
    entry.capture_width = capture_width;
    changed = true;
  }
  if (entry.capture_height != capture_height) {
    entry.capture_height = capture_height;
    changed = true;
  }
  if (entry.capture_format != capture_format) {
    entry.capture_format = capture_format;
    changed = true;
  }
  if (entry.capture_profile_version != capture_profile_version) {
    entry.capture_profile_version = capture_profile_version;
    changed = true;
  }
  captures_in_flight_[capture_id] = CaptureInFlight{session_id, started_ns};
  return changed;
}

bool CoreAcquisitionSessionRegistry::on_capture_completed(uint64_t device_instance_id,
                                                          uint64_t capture_id,
                                                          uint64_t completed_ns) {
  if (device_instance_id == 0 || capture_id == 0) {
    return false;
  }

  uint64_t session_id = resolve_live_session_id_for_device_(device_instance_id);
  auto inflight_it = captures_in_flight_.find(capture_id);
  if (inflight_it != captures_in_flight_.end()) {
    session_id = inflight_it->second.acquisition_session_id;
  }
  if (session_id == 0) {
    return false;
  }
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    captures_in_flight_.erase(capture_id);
    return false;
  }
  AcquisitionSessionEntry& entry = it->second;
  ++entry.captures_completed;
  entry.last_capture_id = capture_id;
  entry.error_code = 0;
  if (inflight_it != captures_in_flight_.end()) {
    const uint64_t started_ns = inflight_it->second.started_ns;
    entry.last_capture_latency_ns = (completed_ns >= started_ns) ? (completed_ns - started_ns) : 0;
    captures_in_flight_.erase(inflight_it);
  } else {
    entry.last_capture_latency_ns = 0;
  }
  return true;
}

bool CoreAcquisitionSessionRegistry::on_capture_failed(uint64_t device_instance_id,
                                                       uint64_t capture_id,
                                                       uint32_t error_code,
                                                       uint64_t failed_ns) {
  if (device_instance_id == 0 || capture_id == 0) {
    return false;
  }

  uint64_t session_id = resolve_live_session_id_for_device_(device_instance_id);
  auto inflight_it = captures_in_flight_.find(capture_id);
  if (inflight_it != captures_in_flight_.end()) {
    session_id = inflight_it->second.acquisition_session_id;
  }
  if (session_id == 0) {
    return false;
  }
  auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    captures_in_flight_.erase(capture_id);
    return false;
  }
  AcquisitionSessionEntry& entry = it->second;
  ++entry.captures_failed;
  entry.last_capture_id = capture_id;
  entry.error_code = static_cast<int32_t>(error_code);
  if (inflight_it != captures_in_flight_.end()) {
    const uint64_t started_ns = inflight_it->second.started_ns;
    entry.last_capture_latency_ns = (failed_ns >= started_ns) ? (failed_ns - started_ns) : 0;
    captures_in_flight_.erase(inflight_it);
  } else {
    entry.last_capture_latency_ns = 0;
  }
  return true;
}

uint64_t CoreAcquisitionSessionRegistry::resolve_live_session_id_for_device_(uint64_t device_instance_id) const {
  auto count_it = device_live_session_count_.find(device_instance_id);
  if (count_it == device_live_session_count_.end() || count_it->second != 1) {
    return 0;
  }
  auto live_it = device_live_session_id_.find(device_instance_id);
  if (live_it == device_live_session_id_.end()) {
    return 0;
  }
  return live_it->second;
}

void CoreAcquisitionSessionRegistry::rebuild_device_live_index_(uint64_t device_instance_id) {
  if (device_instance_id == 0) {
    return;
  }
  uint32_t live_count = 0;
  uint64_t sole_live_id = 0;
  for (const auto& kv : sessions_) {
    if (kv.second.device_instance_id != device_instance_id || kv.second.phase != CBLifecyclePhase::LIVE) {
      continue;
    }
    ++live_count;
    sole_live_id = kv.first;
  }
  if (live_count == 0) {
    device_live_session_count_.erase(device_instance_id);
    device_live_session_id_.erase(device_instance_id);
    return;
  }
  device_live_session_count_[device_instance_id] = live_count;
  if (live_count == 1) {
    device_live_session_id_[device_instance_id] = sole_live_id;
  } else {
    device_live_session_id_.erase(device_instance_id);
  }
}

void CoreAcquisitionSessionRegistry::clear() {
  sessions_.clear();
  device_live_session_id_.clear();
  device_live_session_count_.clear();
  captures_in_flight_.clear();
}

} // namespace cambang
