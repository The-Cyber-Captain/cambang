#include "imaging/broker/banner_info.h"

namespace cambang {

namespace {
constexpr const char* kind_to_cstr(ProviderKind kind) noexcept {
  switch (kind) {
    case ProviderKind::platform_backed: return "platform_backed";
    case ProviderKind::synthetic: return "synthetic";
    default: return "n/a";
  }
}
} // namespace

ProviderBannerInfo describe_provider_for_banner(const ICameraProvider* p) noexcept {
  if (!p) {
    return ProviderBannerInfo{"n/a", "(null)"};
  }

  return ProviderBannerInfo{kind_to_cstr(p->provider_kind()), p->provider_name()};
}

} // namespace cambang
