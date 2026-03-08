#include <iostream>
#include <string>

#include "dev/cli_log.h"

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "scenario_runner: build with -DCAMBANG_INTERNAL_SMOKE=1 (via SCons: smoke=1)."
#endif

#include "smoke/scenario/scenario_catalog.h"

namespace {

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

void usage(const char* argv0, const std::vector<cambang::ScenarioDefinition>& scenarios) {
  std::cerr << "usage: " << argv0 << " <scenario_name> [--provider=synthetic|stub]\n";
  std::cerr << "default provider: synthetic\n";
  std::cerr << "available scenarios:\n";
  for (const auto& scenario : scenarios) {
    std::cerr << "  " << scenario.name << "\n";
  }
}

} // namespace

int main(int argc, char** argv) {
  cambang::ScenarioProviderKind provider_kind = cambang::ScenarioProviderKind::Synthetic;
  std::string requested;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      const auto scenarios = cambang::scenario_catalog(provider_kind);
      usage(argv[0], scenarios);
      return 0;
    }
    if (starts_with(arg, "--provider=")) {
      const std::string value = arg.substr(std::string("--provider=").size());
      if (value == "synthetic") {
        provider_kind = cambang::ScenarioProviderKind::Synthetic;
      } else if (value == "stub") {
        provider_kind = cambang::ScenarioProviderKind::Stub;
      } else {
        const auto scenarios = cambang::scenario_catalog(provider_kind);
        std::cerr << "unknown provider: " << value << "\n";
        usage(argv[0], scenarios);
        return 2;
      }
      continue;
    }
    if (!requested.empty()) {
      const auto scenarios = cambang::scenario_catalog(provider_kind);
      std::cerr << "unexpected extra argument: " << arg << "\n";
      usage(argv[0], scenarios);
      return 2;
    }
    requested = arg;
  }

  const auto scenarios = cambang::scenario_catalog(provider_kind);
  if (requested.empty()) {
    usage(argv[0], scenarios);
    return 2;
  }

  for (const auto& scenario : scenarios) {
    if (scenario.name == requested) {
      cli::line("scenario_runner ", scenario.name, " --provider=", cambang::scenario_provider_name(provider_kind));
      cli::blank();
      return scenario.run();
    }
  }

  std::cerr << "unknown scenario: " << requested << "\n";
  usage(argv[0], scenarios);
  return 2;
}
