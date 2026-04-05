#pragma once

#include "imaging/api/icamera_provider.h"

namespace cambang {

// Small POD for diagnostic banners.
// Strings are stable C strings (either literals or provider-owned stable names).
struct ProviderBannerInfo {
  const char* provider_mode = "n/a";
  const char* provider_name = "n/a";
};

// Returns banner strings describing the active provider selection via ICameraProvider
// only (no broker or concrete-provider dependency).
ProviderBannerInfo describe_provider_for_banner(const ICameraProvider* p) noexcept;

} // namespace cambang
