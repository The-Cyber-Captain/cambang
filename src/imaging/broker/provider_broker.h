#pragma once
// ProviderBroker is the Core-bound facade type for camera provisioning.
// In v1 it is a naming surface only: core binds to exactly one provider instance.
// Future work may implement a real broker that resolves runtime modes.
#include "imaging/api/icamera_provider.h"

namespace cambang {

// Naming-only alias: the broker surface is the provider interface for now.
using ProviderBroker = ICameraProvider;

} // namespace cambang
