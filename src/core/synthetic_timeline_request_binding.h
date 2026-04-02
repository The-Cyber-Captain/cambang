#pragma once

#include <functional>

#include "imaging/synthetic/scenario.h"

namespace cambang {

class CoreRuntime;

using SyntheticTimelineRequestDispatchHook = std::function<void(const SyntheticScheduledEvent&)>;

// Shared single-source mapping from synthetic timeline request-like events
// to CoreRuntime command flow.
SyntheticTimelineRequestDispatchHook make_synthetic_timeline_request_dispatch_hook(CoreRuntime& runtime);

} // namespace cambang
