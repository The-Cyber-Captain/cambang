#include "core/core_acquisition_session_registry.h"

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

bool CoreAcquisitionSessionRegistry::on_native_object_created(uint64_t native_id,
                                                              uint32_t type,
                                                              uint64_t owner_device_instance_id,
                                                              uint64_t created_ns) {
  if (native_id == 0 || type != static_cast<uint32_t>(NativeObjectType::AcquisitionSession)) {
    return false;
  }

  AcquisitionSessionEntry& entry = sessions_[native_id];
  const bool was_known = (entry.acquisition_session_id != 0);

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
  return changed;
}

void CoreAcquisitionSessionRegistry::clear() {
  sessions_.clear();
}

} // namespace cambang
