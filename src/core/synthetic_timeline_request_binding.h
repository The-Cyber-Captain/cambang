#pragma once

#include "imaging/api/icamera_provider.h"

namespace cambang {

class CoreRuntime;

// Installs shared runtime-owned dispatch wiring for synthetic timeline
// request-like events so host layers do not own request semantics.
void bind_synthetic_timeline_request_dispatch(ICameraProvider& provider, CoreRuntime& runtime);

} // namespace cambang
