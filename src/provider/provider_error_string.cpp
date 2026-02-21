// src/provider/provider_error_string.cpp
#include "provider_error_string.h"

namespace cambang {

    const char* to_string(ProviderError error) {
        switch (error) {
        case ProviderError::OK: return "OK";
        case ProviderError::ERR_NOT_SUPPORTED: return "ERR_NOT_SUPPORTED";
        case ProviderError::ERR_INVALID_ARGUMENT: return "ERR_INVALID_ARGUMENT";
        case ProviderError::ERR_BUSY: return "ERR_BUSY";
        case ProviderError::ERR_BAD_STATE: return "ERR_BAD_STATE";
        case ProviderError::ERR_PLATFORM_CONSTRAINT: return "ERR_PLATFORM_CONSTRAINT";
        case ProviderError::ERR_TRANSIENT_FAILURE: return "ERR_TRANSIENT_FAILURE";
        case ProviderError::ERR_PROVIDER_FAILED: return "ERR_PROVIDER_FAILED";
        case ProviderError::ERR_SHUTTING_DOWN: return "ERR_SHUTTING_DOWN";
        default: return "UNKNOWN_PROVIDER_ERROR";
        }
    }

} // namespace cambang