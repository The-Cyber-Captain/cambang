#pragma once

#include <cstdint>
#include <cstddef>

namespace cambang {

// Internal algorithm id used by renderers.
// Keep this decoupled from PatternPreset: multiple presets may map to one algo, and future backends
// may support only a subset of algos.
enum class PatternAlgoId : uint8_t {
#define X(preset_id, token_name, display_name, caps_bitmask, algo_id) algo_id,
#include "pixels/pattern/pattern_defs.inc"
#undef X
};

inline constexpr size_t pattern_algo_count() noexcept {
  size_t n = 0;
#define X(preset_id, token_name, display_name, caps_bitmask, algo_id) ++n;
#include "pixels/pattern/pattern_defs.inc"
#undef X
  return n;
}

} // namespace cambang
