#include <charconv>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

#include "dev/cli_log.h"

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "scenario_runner: build with -DCAMBANG_INTERNAL_SMOKE=1 (via SCons: smoke=1)."
#endif

#include "smoke/scenario/scenario_catalog.h"

#if defined(_WIN32)
  #include <io.h>
  #define CAMBANG_CLOSE _close
  #define CAMBANG_DUP _dup
  #define CAMBANG_DUP2 _dup2
#else
  #include <unistd.h>
  #define CAMBANG_CLOSE close
  #define CAMBANG_DUP dup
  #define CAMBANG_DUP2 dup2
#endif

namespace {

struct BufferedRunResult {
  int rc = 0;
  std::string output;
};

class ScopedStdCapture final {
public:
  bool begin() {
    capture_ = std::tmpfile();
    if (!capture_) {
      return false;
    }

    std::fflush(stdout);
    std::fflush(stderr);

    saved_stdout_fd_ = CAMBANG_DUP(fileno(stdout));
    saved_stderr_fd_ = CAMBANG_DUP(fileno(stderr));
    if (saved_stdout_fd_ < 0 || saved_stderr_fd_ < 0) {
      cleanup_capture_();
      return false;
    }

    const int capture_fd = fileno(capture_);
    if (CAMBANG_DUP2(capture_fd, fileno(stdout)) < 0 ||
        CAMBANG_DUP2(capture_fd, fileno(stderr)) < 0) {
      restore();
      cleanup_capture_();
      return false;
    }

    active_ = true;
    return true;
  }

  std::string end() {
    if (!capture_) {
      return {};
    }

    std::fflush(stdout);
    std::fflush(stderr);
    restore();

    std::rewind(capture_);
    std::string data;
    char buffer[4096];
    while (const size_t n = std::fread(buffer, 1, sizeof(buffer), capture_)) {
      data.append(buffer, n);
    }

    cleanup_capture_();
    return data;
  }

  ~ScopedStdCapture() {
    restore();
    cleanup_capture_();
  }

private:
  void restore_saved_fds_() {
    if (saved_stdout_fd_ >= 0) {
      CAMBANG_CLOSE(saved_stdout_fd_);
      saved_stdout_fd_ = -1;
    }
    if (saved_stderr_fd_ >= 0) {
      CAMBANG_CLOSE(saved_stderr_fd_);
      saved_stderr_fd_ = -1;
    }
  }

  void cleanup_capture_() {
    if (capture_) {
      std::fclose(capture_);
      capture_ = nullptr;
    }
  }

  void restore() {
    if (!active_) {
      restore_saved_fds_();
      return;
    }

    std::fflush(stdout);
    std::fflush(stderr);
    if (saved_stdout_fd_ >= 0) {
      (void)CAMBANG_DUP2(saved_stdout_fd_, fileno(stdout));
    }
    if (saved_stderr_fd_ >= 0) {
      (void)CAMBANG_DUP2(saved_stderr_fd_, fileno(stderr));
    }
    restore_saved_fds_();
    active_ = false;
  }

  FILE* capture_ = nullptr;
  int saved_stdout_fd_ = -1;
  int saved_stderr_fd_ = -1;
  bool active_ = false;
};

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

void usage(const char* argv0, const std::vector<cambang::ScenarioDefinition>& scenarios) {
  std::cerr << "usage: " << argv0 << " <scenario_name> [--provider=synthetic|stub] [--repeat=N] [--trace-realization[=block|csv|both]]\n";
  std::cerr << "   or: " << argv0 << " --run-all [--provider=synthetic|stub] [--repeat=N] [--trace-realization[=block|csv|both]]\n";
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


bool parse_trace_realization(const std::string& value, cambang::RealizationProfilerOptions& options) {
  options.enabled = true;
  if (value.empty() || value == "block") {
    options.format = cambang::RealizationTraceFormat::Block;
    return true;
  }
  if (value == "csv") {
    options.format = cambang::RealizationTraceFormat::Csv;
    return true;
  }
  if (value == "both") {
    options.format = cambang::RealizationTraceFormat::Both;
    return true;
  }
  return false;
}

BufferedRunResult run_buffered(const cambang::ScenarioDefinition& scenario) {
  ScopedStdCapture capture;
  if (!capture.begin()) {
    return BufferedRunResult{scenario.run(), {}};
  }

  const int rc = scenario.run();
  return BufferedRunResult{rc, capture.end()};
}

void emit_buffered_failure_detail(std::string_view scenario_name,
                                  uint64_t iteration,
                                  uint64_t repeat_count,
                                  const std::string& detail) {
  cli::line("[failure-detail] ", scenario_name, " iteration ", iteration, "/", repeat_count);
  if (!detail.empty()) {
    std::fwrite(detail.data(), 1, detail.size(), stdout);
    if (detail.back() != '\n') {
      std::fputc('\n', stdout);
    }
    std::fflush(stdout);
  }
}

int run_single_scenario(const cambang::ScenarioDefinition& scenario,
                        cambang::ScenarioProviderKind provider_kind,
                        uint64_t repeat_count,
                        bool repeat_specified) {
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

int run_all_scenarios(const std::vector<cambang::ScenarioDefinition>& scenarios, uint64_t repeat_count) {
  size_t passed = 0;
  size_t failed = 0;

  for (const auto& scenario : scenarios) {
    bool scenario_failed = false;
    uint64_t completed = 0;

    for (uint64_t iteration = 1; iteration <= repeat_count; ++iteration) {
      const BufferedRunResult result = run_buffered(scenario);
      if (result.rc != 0) {
        cli::line("[FAIL] ", scenario.name, " (iteration ", iteration, "/", repeat_count, ")");
        emit_buffered_failure_detail(scenario.name, iteration, repeat_count, result.output);
        scenario_failed = true;
        ++failed;
        break;
      }
      completed = iteration;
    }

    if (!scenario_failed) {
      ++passed;
      cli::line("[PASS] ", scenario.name, " (", completed, "/", repeat_count, ")");
    }
  }

  cli::line("Summary: ", passed, " passed, ", failed, " failed");
  return failed == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
  cambang::ScenarioProviderKind provider_kind = cambang::ScenarioProviderKind::Synthetic;
  uint64_t repeat_count = 1;
  bool repeat_specified = false;
  bool run_all = false;
  std::string requested;
  cambang::RealizationProfilerOptions profiler_options{};
  profiler_options.target_device_id = cambang::ScenarioHarness::kDeviceId;
  profiler_options.target_stream_id = cambang::ScenarioHarness::kStreamId;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      const auto scenarios = cambang::scenario_catalog(provider_kind, profiler_options);
      usage(argv[0], scenarios);
      return 0;
    }
    if (arg == "--trace-realization") {
      if (!parse_trace_realization("block", profiler_options)) {
        const auto scenarios = cambang::scenario_catalog(provider_kind, profiler_options);
        std::cerr << "invalid trace-realization option\n";
        usage(argv[0], scenarios);
        return 2;
      }
      continue;
    }
    if (starts_with(arg, "--trace-realization=")) {
      const std::string value = arg.substr(std::string("--trace-realization=").size());
      if (!parse_trace_realization(value, profiler_options)) {
        const auto scenarios = cambang::scenario_catalog(provider_kind, profiler_options);
        std::cerr << "unknown trace-realization mode: " << value << "\n";
        usage(argv[0], scenarios);
        return 2;
      }
      continue;
    }
    if (starts_with(arg, "--provider=")) {
      const std::string value = arg.substr(std::string("--provider=").size());
      if (value == "synthetic") {
        provider_kind = cambang::ScenarioProviderKind::Synthetic;
      } else if (value == "stub") {
        provider_kind = cambang::ScenarioProviderKind::Stub;
      } else {
        const auto scenarios = cambang::scenario_catalog(provider_kind, profiler_options);
        std::cerr << "unknown provider: " << value << "\n";
        usage(argv[0], scenarios);
        return 2;
      }
      continue;
    }
    if (starts_with(arg, "--repeat=")) {
      const std::string value = arg.substr(std::string("--repeat=").size());
      if (!parse_repeat_count(value, repeat_count)) {
        const auto scenarios = cambang::scenario_catalog(provider_kind, profiler_options);
        std::cerr << "invalid repeat count: " << value << "\n";
        usage(argv[0], scenarios);
        return 2;
      }
      repeat_specified = true;
      continue;
    }
    if (arg == "--run-all") {
      run_all = true;
      continue;
    }
    if (!requested.empty()) {
      const auto scenarios = cambang::scenario_catalog(provider_kind, profiler_options);
      std::cerr << "unexpected extra argument: " << arg << "\n";
      usage(argv[0], scenarios);
      return 2;
    }
    requested = arg;
  }

  const auto scenarios = cambang::scenario_catalog(provider_kind, profiler_options);
  if (run_all) {
    if (!requested.empty()) {
      std::cerr << "cannot combine scenario name with --run-all\n";
      usage(argv[0], scenarios);
      return 2;
    }
    return run_all_scenarios(scenarios, repeat_count);
  }

  if (requested.empty()) {
    usage(argv[0], scenarios);
    return 2;
  }

  for (const auto& scenario : scenarios) {
    if (scenario.name == requested) {
      return run_single_scenario(scenario, provider_kind, repeat_count, repeat_specified);
    }
  }

  std::cerr << "unknown scenario: " << requested << "\n";
  usage(argv[0], scenarios);
  return 2;
}
