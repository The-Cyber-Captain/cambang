#pragma once

#include <cstdint>

namespace cambang {

// Internal provider startup access/readiness vocabulary.
//
// This is intentionally separate from ProviderError: build support answers
// whether a provider mode is compiled/selectable, while access/readiness answers
// whether the selected provider may become operational now.  The status is not
// Godot-facing and is not part of the public snapshot schema.
enum class ProviderAccessCode : uint32_t {
  Ready = 0,
  PermissionRequired,
  PermissionDenied,
  AccessUnavailable,
  CheckFailed,
};

struct ProviderAccessStatus {
  ProviderAccessCode code = ProviderAccessCode::Ready;
  const char* stable_reason = "ready";
  bool retryable = false;

  constexpr bool ok() const noexcept { return code == ProviderAccessCode::Ready; }

  static constexpr ProviderAccessStatus ready(const char* reason = "ready") noexcept {
    return ProviderAccessStatus{ProviderAccessCode::Ready, reason, false};
  }

  static constexpr ProviderAccessStatus failure(
      ProviderAccessCode c,
      const char* reason,
      bool may_retry = false) noexcept {
    return ProviderAccessStatus{c, reason, may_retry};
  }
};

inline const char* to_string(ProviderAccessCode code) noexcept {
  switch (code) {
    case ProviderAccessCode::Ready: return "Ready";
    case ProviderAccessCode::PermissionRequired: return "PermissionRequired";
    case ProviderAccessCode::PermissionDenied: return "PermissionDenied";
    case ProviderAccessCode::AccessUnavailable: return "AccessUnavailable";
    case ProviderAccessCode::CheckFailed: return "CheckFailed";
    default: return "UnknownProviderAccessCode";
  }
}

} // namespace cambang
