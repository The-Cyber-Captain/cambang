// src/provider/provider_error_string.h
#pragma once

#include "provider_contract_datatypes.h"

namespace cambang {

    // Returns stable enum name strings (for logs/diagnostics).
    // Never allocates. Always returns a non-null c-string.
    const char* to_string(ProviderError error);

} // namespace cambang