#include <charconv>
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
  std::cerr << "usage: " << argv0 << " <scenario_name> [--provider=synthetic|stub] [--repeat=N]\n";
  std::cerr << "default provider: synthetic\n";
  std::cerr << "default repeat: 1\n";
  std::cerr << "available scenarios:\n";
  for (const auto& scenario : scenarios) {
    std::cerr << "  " << scenario.name << "\n";
  }
}

bool parse_repeat_count(const std::string& value, uint64_t& repeat_count) {
  if (value.empty()) {
    return false;
  }
  const char* begin = value.data();
  const char* end = value.data() + value.size();
  const auto result = std::from_chars(begin, end, repeat_count);
  return result.ec == std::errc{} && result.ptr == end && repeat_count > 0;
}

} // namespace

int main(int argc, char** argv) {
  cambang::ScenarioProviderKind provider_kind = cambang::ScenarioProviderKind::Synthetic;
  uint64_t repeat_count = 1;
  bool repeat_specified = false;
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
    if (starts_with(arg, "--repeat=")) {
      const std::string value = arg.substr(std::string("--repeat=").size());
      if (!parse_repeat_count(value, repeat_count)) {
        const auto scenarios = cambang::scenario_catalog(provider_kind);
        std::cerr << "invalid repeat count: " << value << "\n";
        usage(argv[0], scenarios);
        return 2;
      }
      repeat_specified = true;
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
      if (!repeat_specified) {
        return scenario.run();
      }

      for (uint64_t iteration = 1; iteration <= repeat_count; ++iteration) {
        cli::line("[repeat] iteration ", iteration, "/", repeat_count);
        const int rc = scenario.run();
        if (rc != 0) {
          cli::error("[repeat] iteration ", iteration, "/", repeat_count, " FAILED");
          return rc;
        }
      }

      cli::line("Scenario PASSED (repeat=", repeat_count, ")");
      return 0;
    }
  }

  std::cerr << "unknown scenario: " << requested << "\n";
  usage(argv[0], scenarios);
  return 2;
}
