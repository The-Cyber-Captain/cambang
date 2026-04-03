#pragma once

#include <string>

#include "imaging/synthetic/scenario_loader_parse.h"

namespace cambang {

bool validate_parsed_synthetic_scenario_loader_document(
    const SyntheticScenarioLoaderParsedDocument& parsed,
    std::string* error = nullptr);

} // namespace cambang
