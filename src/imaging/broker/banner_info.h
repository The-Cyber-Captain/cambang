#pragma once

#include "imaging/api/icamera_provider.h"

namespace cambang {

// Small POD for diagnostic banners.
// Strings are stable C strings (either literals or provider-owned stable names).
struct ProviderBannerInfo {
  const char* provider_mode = "n/a";
  const char* provider_name = "n/a";
};

// Returns banner strings describing the *effective/latched* provider selection.
//
// If p is a ProviderBroker, this returns the broker-latched runtime mode and the
// active backend identity.
// Otherwise, mode is "n/a" and name is p->provider_name().
ProviderBannerInfo describe_provider_for_banner(const ICameraProvider* p) noexcept;

} // namespace cambang
