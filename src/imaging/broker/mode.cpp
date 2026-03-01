#include "imaging/broker/mode.h"

#include <cstdlib>
#include <cstring>

namespace cambang {

RuntimeMode resolve_runtime_mode() {
  const char* v = std::getenv("CAMBANG_PROVIDER_MODE");
  if (!v || !*v) {
    return RuntimeMode::platform_backed;
  }
  if (std::strcmp(v, "synthetic") == 0) {
    return RuntimeMode::synthetic;
  }
  // Default/fallback is platform-backed.
  return RuntimeMode::platform_backed;
}

} // namespace cambang
