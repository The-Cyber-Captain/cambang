#pragma once

#include <functional>
#include <string>
#include <vector>

#include "smoke/verify_case/verify_case_harness.h"

namespace cambang {

struct VerifyCaseDefinition {
  std::string name;
  std::function<int()> run;
};

std::vector<VerifyCaseDefinition> verify_case_catalog(VerifyCaseProviderKind provider_kind,
                                               const RealizationProfilerOptions& profiler_options = {});

} // namespace cambang
