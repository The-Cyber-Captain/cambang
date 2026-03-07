#pragma once

#include <string>

#include "smoke/scenario/scenario_model.h"

namespace cambang {

bool load_scenario(const std::string& name, Scenario& out);

} // namespace cambang
