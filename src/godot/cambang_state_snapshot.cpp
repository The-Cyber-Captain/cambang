#include "godot/cambang_state_snapshot.h"

#include <godot_cpp/core/class_db.hpp>

#include "core/snapshot/state_snapshot.h"

namespace cambang {

void CamBANGStateSnapshotGD::_init_from_core(std::shared_ptr<const CamBANGStateSnapshot> snap) {
  snap_ = std::move(snap);
}

uint32_t CamBANGStateSnapshotGD::get_schema_version() const {
  return snap_ ? snap_->schema_version : 0u;
}

uint64_t CamBANGStateSnapshotGD::get_gen() const {
  return snap_ ? snap_->gen : 0ull;
}

uint64_t CamBANGStateSnapshotGD::get_version() const {
  return snap_ ? snap_->version : 0ull;
}

uint64_t CamBANGStateSnapshotGD::get_topology_version() const {
  return snap_ ? snap_->topology_version : 0ull;
}

uint64_t CamBANGStateSnapshotGD::get_timestamp_ns() const {
  return snap_ ? snap_->timestamp_ns : 0ull;
}

void CamBANGStateSnapshotGD::_bind_methods() {
  godot::ClassDB::bind_method(godot::D_METHOD("get_schema_version"), &CamBANGStateSnapshotGD::get_schema_version);
  godot::ClassDB::bind_method(godot::D_METHOD("get_gen"), &CamBANGStateSnapshotGD::get_gen);
  godot::ClassDB::bind_method(godot::D_METHOD("get_version"), &CamBANGStateSnapshotGD::get_version);
  godot::ClassDB::bind_method(godot::D_METHOD("get_topology_version"), &CamBANGStateSnapshotGD::get_topology_version);
  godot::ClassDB::bind_method(godot::D_METHOD("get_timestamp_ns"), &CamBANGStateSnapshotGD::get_timestamp_ns);
}

} // namespace cambang
