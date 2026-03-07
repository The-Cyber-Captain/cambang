#pragma once

#include <cstdint>

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include "core/snapshot/state_snapshot.h"

namespace cambang {

// Export a core snapshot into a Godot-facing, struct-like Variant graph.
//
// The returned Dictionary matches docs/state_snapshot.md (schema v1):
//   - top-level scalar header fields
//   - Array<Dictionary> for record vectors
//
// The supplied (gen, version, topology_version) are the **Godot-facing**
// tick-bounded counters and MUST be used in the exported header.
godot::Dictionary export_snapshot_to_godot(const CamBANGStateSnapshot& snap,
                                          uint64_t gen,
                                          uint64_t version,
                                          uint64_t topology_version);

} // namespace cambang
