#pragma once
#include <cstdint>

namespace cambang {

// Locked runtime modes.
enum class RuntimeMode : std::uint8_t {
  platform_backed = 0,
  synthetic = 1,
};

// Resolve runtime mode from environment/config.
// Deterministic rule for this phase:
// - If CAMBANG_PROVIDER_MODE == "synthetic" -> synthetic
// - Else -> platform_backed (default)
RuntimeMode resolve_runtime_mode();
} // namespace cambang
