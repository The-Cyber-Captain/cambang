#pragma once

#include <cstdint>
#include <string>

#include "imaging/api/provider_contract_datatypes.h"
#include "imaging/synthetic/scenario_model.h"

namespace cambang {

// This is the built-in scenario library (C++-authored scenarios).
// Future external/file-backed scenarios will use external_scenario_library
// and be ingested via scenario_loader.
enum class SyntheticBuiltinScenarioLibraryId : std::uint8_t {
  StreamLifecycleVersions = 0,
  TopologyChangeVersions = 1,
  PublicationCoalescing = 2,
};

const char* synthetic_builtin_scenario_library_name(SyntheticBuiltinScenarioLibraryId id) noexcept;

bool build_synthetic_builtin_scenario_library_canonical_scenario(
    SyntheticBuiltinScenarioLibraryId id,
    const CaptureProfile& baseline_profile,
    SyntheticCanonicalScenario& out,
    std::string* error = nullptr);

} // namespace cambang
