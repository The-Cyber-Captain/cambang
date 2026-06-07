#include "imaging/broker/provider_broker.h"

#include <iostream>

using cambang::ProviderBroker;
using cambang::ProviderError;
using cambang::RuntimeMode;

int main() {
  const auto platform_backed = ProviderBroker::check_mode_supported_in_build(RuntimeMode::platform_backed);
  if (platform_backed.ok() || platform_backed.code != ProviderError::ERR_NOT_SUPPORTED) {
    std::cerr << "expected platform_backed to be unsupported without a compiled platform provider\n";
    return 1;
  }

  const auto synthetic = ProviderBroker::check_mode_supported_in_build(RuntimeMode::synthetic);
  if (!synthetic.ok()) {
    std::cerr << "expected synthetic to be supported when CAMBANG_ENABLE_SYNTHETIC=1\n";
    return 1;
  }

  return 0;
}
