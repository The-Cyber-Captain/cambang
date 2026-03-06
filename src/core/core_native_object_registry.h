#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace cambang {

// CoreNativeObjectRegistry
//
// Maintains the core-truth view of provider-reported native objects.
// This is the authoritative source for CamBANGStateSnapshot.native_objects.
//
class CoreNativeObjectRegistry final {
public:
  struct Record {
    uint64_t native_id = 0;
    uint32_t type = 0;
    uint64_t root_id = 0;
    uint64_t owner_device_instance_id = 0;
    uint64_t owner_stream_id = 0;
    uint64_t creation_gen = 0;

    bool created = false;
    bool destroyed = false;

    uint64_t created_ns = 0;
    uint64_t destroyed_ns = 0;
    uint64_t destroyed_integration_ns = 0;

    uint64_t bytes_allocated = 0;
    uint32_t buffers_in_use = 0;
  };

  void on_native_object_created(uint64_t native_id, uint32_t type, uint64_t root_id,
                              uint64_t owner_device_instance_id,
                              uint64_t owner_stream_id,
                              uint64_t bytes_allocated,
                              uint32_t buffers_in_use,
                              uint64_t creation_gen,
                              uint64_t created_ns);
  void on_native_object_destroyed(uint64_t native_id,
                                  uint64_t destroyed_ns,
                                  uint64_t destroyed_integration_ns);

  size_t retire_destroyed_older_than(uint64_t now_ns, uint64_t retention_window_ns);
  std::optional<uint64_t> next_retirement_delay_ns(uint64_t now_ns, uint64_t retention_window_ns) const;

  const std::map<uint64_t, Record>& all() const noexcept { return records_; }

private:
  std::map<uint64_t, Record> records_;
};

} // namespace cambang
