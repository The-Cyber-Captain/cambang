#include "core/core_acquisition_session_registry.h"

namespace cambang {

bool CoreAcquisitionSessionRegistry::on_device_opened(uint64_t device_instance_id, uint64_t now_ns) {
  if (device_instance_id == 0) {
    return false;
  }

  auto active_it = active_session_by_device_.find(device_instance_id);
  if (active_it != active_session_by_device_.end()) {
    auto session_it = sessions_.find(active_it->second);
    if (session_it != sessions_.end()) {
      if (session_it->second.phase != CBLifecyclePhase::LIVE) {
        session_it->second.phase = CBLifecyclePhase::LIVE;
        session_it->second.ended_ns = 0;
        return true;
      }
      return false;
    }
  }

  const uint64_t session_id = next_acquisition_session_id_++;
  active_session_by_device_[device_instance_id] = session_id;

  AcquisitionSessionEntry entry;
  entry.acquisition_session_id = session_id;
  entry.device_instance_id = device_instance_id;
  entry.phase = CBLifecyclePhase::LIVE;
  entry.started_ns = now_ns;

  sessions_[session_id] = entry;
  return true;
}

bool CoreAcquisitionSessionRegistry::on_device_closed(uint64_t device_instance_id, uint64_t now_ns) {
  if (device_instance_id == 0) {
    return false;
  }

  auto active_it = active_session_by_device_.find(device_instance_id);
  if (active_it == active_session_by_device_.end()) {
    return false;
  }

  const uint64_t session_id = active_it->second;
  active_session_by_device_.erase(active_it);

  auto session_it = sessions_.find(session_id);
  if (session_it == sessions_.end()) {
    return false;
  }

  session_it->second.phase = CBLifecyclePhase::DESTROYED;
  session_it->second.ended_ns = now_ns;
  return true;
}

void CoreAcquisitionSessionRegistry::clear() {
  next_acquisition_session_id_ = 1;
  active_session_by_device_.clear();
  sessions_.clear();
}

} // namespace cambang
