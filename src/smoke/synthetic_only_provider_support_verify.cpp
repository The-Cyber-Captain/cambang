#include "dev/cli_log.h"
#include "imaging/broker/provider_broker.h"

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "synthetic_only_provider_support_verify: build through the repo SCons maintainer_tools alias so CAMBANG_INTERNAL_SMOKE=1 is defined."
#endif

using cambang::ProviderAccessCode;
using cambang::ProviderBroker;
using cambang::ProviderError;
using cambang::RuntimeMode;

namespace {

const char* provider_error_label(ProviderError e) {
  switch (e) {
    case ProviderError::OK: return "OK";
    case ProviderError::ERR_NOT_SUPPORTED: return "ERR_NOT_SUPPORTED";
    case ProviderError::ERR_INVALID_ARGUMENT: return "ERR_INVALID_ARGUMENT";
    case ProviderError::ERR_BUSY: return "ERR_BUSY";
    case ProviderError::ERR_BAD_STATE: return "ERR_BAD_STATE";
    case ProviderError::ERR_PLATFORM_CONSTRAINT: return "ERR_PLATFORM_CONSTRAINT";
    case ProviderError::ERR_TRANSIENT_FAILURE: return "ERR_TRANSIENT_FAILURE";
    case ProviderError::ERR_PROVIDER_FAILED: return "ERR_PROVIDER_FAILED";
    case ProviderError::ERR_SHUTTING_DOWN: return "ERR_SHUTTING_DOWN";
    default: return "ERR_UNKNOWN";
  }
}

const char* ok_label(bool ok) {
  return ok ? "true" : "false";
}

bool fail_provider_result(
    const char* check_name,
    const char* expectation,
    bool actual_ok,
    ProviderError actual_code) {
  cli::error(
      "FAIL synthetic_only_provider_support_verify check=", check_name,
      " expected=", expectation,
      " actual_ok=", ok_label(actual_ok),
      " actual_code=", provider_error_label(actual_code));
  return false;
}

bool fail_access_result(
    const char* check_name,
    const char* expectation,
    bool actual_ok,
    ProviderAccessCode actual_code,
    const char* actual_reason,
    bool actual_retryable) {
  cli::error(
      "FAIL synthetic_only_provider_support_verify check=", check_name,
      " expected=", expectation,
      " actual_ok=", ok_label(actual_ok),
      " actual_code=", to_string(actual_code),
      " actual_reason=", (actual_reason ? actual_reason : "<null>"),
      " actual_retryable=", ok_label(actual_retryable));
  return false;
}

bool verify_platform_backed_unsupported() {
  const auto result = ProviderBroker::check_mode_supported_in_build(RuntimeMode::platform_backed);
  if (result.ok() || result.code != ProviderError::ERR_NOT_SUPPORTED) {
    return fail_provider_result(
        "platform_backed_build_support",
        "ok=false code=ERR_NOT_SUPPORTED",
        result.ok(),
        result.code);
  }
  cli::line("OK platform_backed_build_support");
  return true;
}

bool verify_synthetic_supported() {
  const auto result = ProviderBroker::check_mode_supported_in_build(RuntimeMode::synthetic);
  if (!result.ok()) {
    return fail_provider_result(
        "synthetic_build_support",
        "ok=true code=OK",
        result.ok(),
        result.code);
  }
  cli::line("OK synthetic_build_support");
  return true;
}

bool verify_synthetic_access_ready() {
  const auto result = ProviderBroker::check_mode_access_readiness(RuntimeMode::synthetic);
  if (!result.ok() || result.code != ProviderAccessCode::Ready) {
    return fail_access_result(
        "synthetic_access_readiness",
        "ok=true code=Ready",
        result.ok(),
        result.code,
        result.stable_reason,
        result.retryable);
  }
  cli::line("OK synthetic_access_readiness");
  return true;
}

} // namespace

int main() {
  if (!verify_platform_backed_unsupported()) return 1;
  if (!verify_synthetic_supported()) return 1;
  if (!verify_synthetic_access_ready()) return 1;

  cli::line("PASS synthetic_only_provider_support_verify");
  return 0;
}
