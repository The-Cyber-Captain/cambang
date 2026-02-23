// src/core/core_runtime_state.h
#pragma once

#include <cstdint>

namespace cambang {

// CoreRuntimeState is the lifecycle gating state machine for CoreRuntime.
//
// Rules (v1):
// - Only LIVE accepts new work.
// - STARTING / TEARING_DOWN / STOPPED reject try_post with Closed.
// - stop() is idempotent.
// - start() is idempotent (returns true if already STARTING/LIVE).
enum class CoreRuntimeState : std::uint8_t {
  CREATED = 0,
  STARTING,
  LIVE,
  TEARING_DOWN,
  STOPPED,
};

} // namespace cambang
