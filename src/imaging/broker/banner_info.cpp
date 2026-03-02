#include "imaging/broker/banner_info.h"

#include "imaging/broker/provider_broker.h"

namespace cambang {

namespace {
constexpr const char* mode_to_cstr(RuntimeMode m) noexcept {
  switch (m) {
    case RuntimeMode::platform_backed: return "platform_backed";
    case RuntimeMode::synthetic: return "synthetic";
    default: return "n/a";
  }
}
} // namespace

ProviderBannerInfo describe_provider_for_banner(const ICameraProvider* p) noexcept {
  if (!p) {
    return ProviderBannerInfo{"n/a", "(null)"};
  }

  if (auto* broker = dynamic_cast<const ProviderBroker*>(p)) {
    return ProviderBannerInfo{mode_to_cstr(broker->runtime_mode_latched()), broker->provider_name()};
  }

  return ProviderBannerInfo{"n/a", p->provider_name()};
}

} // namespace cambang
