#include "core/core_native_object_registry.h"

namespace cambang {

void CoreNativeObjectRegistry::on_native_object_created(uint64_t native_id,
                                                       uint32_t type,
                                                       uint64_t root_id,
                                                       uint64_t creation_gen,
                                                       uint64_t created_ns) {
  Record& r = records_[native_id];
  r.native_id = native_id;
  r.type = type;
  r.root_id = root_id;
  r.creation_gen = creation_gen;
  r.created = true;
  r.created_ns = created_ns;
  // If a provider reports "created" again for the same native_id, keep first created_ns.
  // (Scaffolding slice: no strict error accounting.)
}

void CoreNativeObjectRegistry::on_native_object_destroyed(uint64_t native_id, uint64_t destroyed_ns) {
  auto it = records_.find(native_id);
  if (it == records_.end()) {
    // Truth surface should reflect reality, including orphan destroys.
    // Create an entry so the snapshot can represent the orphan.
    Record r;
    r.native_id = native_id;
    r.destroyed = true;
    r.destroyed_ns = destroyed_ns;
    records_.emplace(native_id, r);
    return;
  }
  it->second.destroyed = true;
  it->second.destroyed_ns = destroyed_ns;
}

} // namespace cambang
