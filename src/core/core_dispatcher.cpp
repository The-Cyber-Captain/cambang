// src/core/core_dispatcher.cpp

#include "core/core_dispatcher.h"

#include <variant>

namespace cambang {

void CoreDispatcher::dispatch(CoreCommand&& cmd) {
  // NOTE: For this build slice we are proving lifecycle only.
  // No state mutation yet; non-frame commands are simply acknowledged/dropped.
  // Frame commands MUST release immediately to return ownership to the provider.

  stats_.commands_total++;

  switch (cmd.type) {
  case CoreCommandType::PROVIDER_FRAME: {
    auto& p = std::get<CmdProviderFrame>(cmd.payload);

    stats_.commands_handled++;
    stats_.frames_received++;

    // Release-on-drop at dispatch boundary.
    p.frame.release_now();

    stats_.frames_released++;

    // Defensive hygiene: prevent accidental double-release if this payload is
    // inspected/logged/re-dispatched in future scaffolding.
    p.frame.release = nullptr;
    p.frame.release_user = nullptr;
    break;
  }

  default:
    stats_.commands_dropped++;
    break;
  }
}

} // namespace cambang
