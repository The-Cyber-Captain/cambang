#include "imaging/synthetic/gpu_backing_runtime.h"

#if !defined(CAMBANG_GDE_BUILD) || !(CAMBANG_GDE_BUILD)
namespace cambang {

std::shared_ptr<void> synthetic_gpu_backing_retain_primary_gpu_backing_rgba8(
    const uint8_t* /*src*/,
    uint32_t /*width*/,
    uint32_t /*height*/,
    uint32_t /*stride_bytes*/) noexcept {
  return {};
}

} // namespace cambang
#endif
