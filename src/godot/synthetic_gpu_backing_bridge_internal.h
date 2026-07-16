#pragma once

namespace cambang {

// Internal-only: registers synthetic bridge display wrapper classes. Not public API.
void register_synthetic_gpu_backing_internal_classes();

#if defined(CAMBANG_INTERNAL_SMOKE)
bool exercise_primary_gpu_post_transfer_failure_for_smoke() noexcept;
bool exercise_gpu_wrapper_post_transfer_failure_for_smoke() noexcept;
#endif

} // namespace cambang
