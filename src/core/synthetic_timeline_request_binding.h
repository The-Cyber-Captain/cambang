#pragma once

#include <functional>

#include "imaging/api/icamera_provider.h"
#include "imaging/synthetic/scenario.h"

namespace cambang {

class CoreRuntime;

using SyntheticTimelineRequestDispatchHook = std::function<void(const SyntheticScheduledEvent&)>;

// Shared single-source mapping from synthetic timeline request-like events
// to CoreRuntime command flow.
SyntheticTimelineRequestDispatchHook make_synthetic_timeline_request_dispatch_hook(CoreRuntime& runtime);

// Installs the shared dispatch hook onto direct SyntheticProvider instances.
void bind_synthetic_timeline_request_dispatch(ICameraProvider& provider, CoreRuntime& runtime);

} // namespace cambang
