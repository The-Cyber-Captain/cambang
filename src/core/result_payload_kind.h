#pragma once

#include <cstdint>

namespace cambang {

enum class ResultPayloadKind : uint8_t {
  CPU_PACKED = 0,
  CPU_PLANAR = 1,
  GPU_SURFACE = 2,
  ENCODED_IMAGE = 3,
  RAW_IMAGE = 4,
};

} // namespace cambang
