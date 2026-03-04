#pragma once

#include <cstdint>
#include <map>

namespace cambang {

// CoreNativeObjectRegistry
//
// Maintains the core-truth view of provider-reported native objects.
// This is the authoritative source for CamBANGStateSnapshot.native_objects.
//
// Scaffolding slice notes:
// - owner_* fields are not tracked yet (rig/device/stream ownership mapping).
// - bytes/buffers accounting is not tracked yet.
class CoreNativeObjectRegistry final {
public:
  struct Record {
    uint64_t native_id = 0;
    uint32_t type = 0;
    uint64_t root_id = 0;

    bool created = false;
    bool destroyed = false;

    uint64_t created_ns = 0;
    uint64_t destroyed_ns = 0;
  };

  void on_native_object_created(uint64_t native_id, uint32_t type, uint64_t root_id, uint64_t created_ns);
  void on_native_object_destroyed(uint64_t native_id, uint64_t destroyed_ns);

  const std::map<uint64_t, Record>& all() const noexcept { return records_; }

private:
  std::map<uint64_t, Record> records_;
};

} // namespace cambang
