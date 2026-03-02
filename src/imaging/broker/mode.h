#pragma once
#include <cstdint>

namespace cambang {

// Locked runtime modes (naming only in this session).
enum class RuntimeMode : std::uint8_t {
  platform_backed = 0,
  synthetic = 1,
};

} // namespace cambang
