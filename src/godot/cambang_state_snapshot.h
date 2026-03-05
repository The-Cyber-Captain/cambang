#pragma once

#include <memory>

#include <godot_cpp/classes/ref_counted.hpp>

struct CamBANGStateSnapshot;

namespace cambang {

// Godot wrapper for an immutable core snapshot.
class CamBANGStateSnapshotGD final : public godot::RefCounted {
  GDCLASS(CamBANGStateSnapshotGD, godot::RefCounted)

public:
  CamBANGStateSnapshotGD() = default;
  ~CamBANGStateSnapshotGD() override = default;

  void _init_from_core(std::shared_ptr<const CamBANGStateSnapshot> snap);

  uint32_t get_schema_version() const;
  uint64_t get_gen() const;
  uint64_t get_version() const;
  uint64_t get_topology_version() const;
  uint64_t get_timestamp_ns() const;

protected:
  static void _bind_methods();

private:
  std::shared_ptr<const CamBANGStateSnapshot> snap_;
};

} // namespace cambang
