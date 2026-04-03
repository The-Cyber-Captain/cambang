#pragma once

#include <string>

#include "imaging/synthetic/scenario_loader_parse.h"
#include "imaging/synthetic/scenario_model.h"

namespace cambang {

bool convert_parsed_synthetic_scenario_loader_document_to_canonical(
    const SyntheticScenarioLoaderParsedDocument& parsed,
    SyntheticCanonicalScenario& out,
    std::string* error = nullptr);

} // namespace cambang
