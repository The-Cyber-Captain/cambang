#pragma once

#include <cstdint>

namespace cambang {

enum class ResultCapability : uint8_t {
  READY = 0,
  CHEAP = 1,
  EXPENSIVE = 2,
  UNSUPPORTED = 3,
};

} // namespace cambang
