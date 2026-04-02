#pragma once

#include <cstdint>
#include <string>

#include "imaging/api/provider_contract_datatypes.h"
#include "imaging/synthetic/scenario_model.h"

namespace cambang {

enum class SyntheticScenarioCatalogId : std::uint8_t {
  StreamLifecycleVersions = 0,
  TopologyChangeVersions = 1,
  PublicationCoalescing = 2,
};

const char* synthetic_scenario_catalog_name(SyntheticScenarioCatalogId id) noexcept;

bool build_synthetic_catalog_canonical_scenario(
    SyntheticScenarioCatalogId id,
    const CaptureProfile& baseline_profile,
    const PictureConfig& baseline_picture,
    SyntheticCanonicalScenario& out,
    std::string* error = nullptr);

} // namespace cambang
