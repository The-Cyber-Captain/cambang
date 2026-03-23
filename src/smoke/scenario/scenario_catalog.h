#pragma once

#include <functional>
#include <string>
#include <vector>

#include "smoke/scenario/scenario_harness.h"

namespace cambang {

struct ScenarioDefinition {
  std::string name;
  std::function<int()> run;
};

std::vector<ScenarioDefinition> scenario_catalog(ScenarioProviderKind provider_kind,
                                               const RealizationProfilerOptions& profiler_options = {});

} // namespace cambang
