#pragma once

#include <string>

#include "imaging/synthetic/scenario_model.h"

namespace cambang {

bool load_synthetic_canonical_scenario_from_json_file(
    const std::string& path,
    SyntheticCanonicalScenario& out,
    std::string* error = nullptr);

bool load_synthetic_canonical_scenario_from_json_text(
    const std::string& text,
    SyntheticCanonicalScenario& out,
    std::string* error = nullptr);

} // namespace cambang
