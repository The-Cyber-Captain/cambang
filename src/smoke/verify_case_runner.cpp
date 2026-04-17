#include <charconv>
#include <iostream>
#include <string>
#include <string_view>

#include "dev/cli_log.h"

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "verify_case_runner: build with -DCAMBANG_INTERNAL_SMOKE=1 (via SCons: smoke=1)."
#endif

#include "smoke/verify_case/verify_case_catalog.h"

namespace {

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

void usage(const char* argv0, const std::vector<cambang::VerifyCaseDefinition>& verify_cases) {
  std::cerr << "usage: " << argv0 << " <verification_case_name> [--provider=synthetic|stub] [--repeat=N] [--trace-realization[=block|csv|both]]\n";
  std::cerr << "   or: " << argv0 << " --run-all [--provider=synthetic|stub] [--repeat=N] [--trace-realization[=block|csv|both]] [--verbose]\n";
  std::cerr << "default provider: synthetic\n";
  std::cerr << "default repeat: 1\n";
  std::cerr << "default run-all: concise (pass details suppressed unless --verbose)\n";
  std::cerr << "available verification cases:\n";
  for (const auto& verify_case : verify_cases) {
    std::cerr << "  " << verify_case.name << "\n";
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



void append_line_to_buffer(FILE* stream, const std::string& text, void* user) {
  auto* buffer = static_cast<std::string*>(user);
  if (!buffer) {
    return;
  }
  (*buffer) += (stream == stderr) ? "[stderr] " : "[stdout] ";
  buffer->append(text);
  buffer->push_back('\n');
}

void print_case_log_dump(std::string_view case_name, const std::string& buffer) {
  if (buffer.empty()) {
    return;
  }
  std::cerr << "----- begin verify-case log: " << case_name << " -----\n";
  std::cerr << buffer;
  if (buffer.back() != '\n') {
    std::cerr << '\n';
  }
  std::cerr << "----- end verify-case log: " << case_name << " -----\n";
}

int run_single_verify_case(const cambang::VerifyCaseDefinition& verify_case,
                        cambang::VerifyCaseProviderKind provider_kind,
                        uint64_t repeat_count,
                        bool repeat_specified) {
  cli::line("verify_case_runner ", verify_case.name, " --provider=", cambang::verify_case_provider_name(provider_kind));
  cli::blank();
  if (!repeat_specified) {
    return verify_case.run();
  }

  for (uint64_t iteration = 1; iteration <= repeat_count; ++iteration) {
    cli::line("[repeat] iteration ", iteration, "/", repeat_count);
    const int rc = verify_case.run();
    if (rc != 0) {
      cli::error("[repeat] iteration ", iteration, "/", repeat_count, " FAILED");
      return rc;
    }
  }

  cli::line("Verification case PASSED (repeat=", repeat_count, ")");
  return 0;
}

int run_all_verify_cases(const std::vector<cambang::VerifyCaseDefinition>& verify_cases,
                         uint64_t repeat_count,
                         bool verbose) {
  size_t passed = 0;
  size_t failed = 0;

  for (const auto& verify_case : verify_cases) {
    bool verify_case_failed = false;
    uint64_t completed = 0;
    uint64_t failed_iteration = 0;
    std::string case_log;

    {
      cli::scoped_line_sink capture(&append_line_to_buffer, &case_log);
      for (uint64_t iteration = 1; iteration <= repeat_count; ++iteration) {
        const int rc = verify_case.run();
        if (rc != 0) {
          verify_case_failed = true;
          failed_iteration = iteration;
          ++failed;
          break;
        }
        completed = iteration;
      }
    }

    if (verify_case_failed) {
      print_case_log_dump(verify_case.name, case_log);
      cli::line("[FAIL] ", verify_case.name, " (iteration ", failed_iteration, "/", repeat_count, ")");
      continue;
    }

    ++passed;
    if (verbose) {
      print_case_log_dump(verify_case.name, case_log);
    }
    cli::line("[PASS] ", verify_case.name, " (", completed, "/", repeat_count, ")");
  }

  cli::line("Summary: ", passed, " passed, ", failed, " failed");
  return failed == 0 ? 0 : 1;
}


} // namespace

int main(int argc, char** argv) {
  cambang::VerifyCaseProviderKind provider_kind = cambang::VerifyCaseProviderKind::Synthetic;
  uint64_t repeat_count = 1;
  bool repeat_specified = false;
  bool run_all = false;
  bool run_all_verbose = false;
  std::string requested;
  cambang::RealizationProfilerOptions profiler_options{};
  profiler_options.target_device_id = cambang::VerifyCaseHarness::kDeviceId;
  profiler_options.target_stream_id = cambang::VerifyCaseHarness::kStreamId;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      const auto verify_cases = cambang::verify_case_catalog(provider_kind, profiler_options);
      usage(argv[0], verify_cases);
      return 0;
    }
    if (arg == "--trace-realization") {
      if (!parse_trace_realization("block", profiler_options)) {
        const auto verify_cases = cambang::verify_case_catalog(provider_kind, profiler_options);
        std::cerr << "invalid trace-realization option\n";
        usage(argv[0], verify_cases);
        return 2;
      }
      continue;
    }
    if (starts_with(arg, "--trace-realization=")) {
      const std::string value = arg.substr(std::string("--trace-realization=").size());
      if (!parse_trace_realization(value, profiler_options)) {
        const auto verify_cases = cambang::verify_case_catalog(provider_kind, profiler_options);
        std::cerr << "unknown trace-realization mode: " << value << "\n";
        usage(argv[0], verify_cases);
        return 2;
      }
      continue;
    }
    if (starts_with(arg, "--provider=")) {
      const std::string value = arg.substr(std::string("--provider=").size());
      if (value == "synthetic") {
        provider_kind = cambang::VerifyCaseProviderKind::Synthetic;
      } else if (value == "stub") {
        provider_kind = cambang::VerifyCaseProviderKind::Stub;
      } else {
        const auto verify_cases = cambang::verify_case_catalog(provider_kind, profiler_options);
        std::cerr << "unknown provider: " << value << "\n";
        usage(argv[0], verify_cases);
        return 2;
      }
      continue;
    }
    if (starts_with(arg, "--repeat=")) {
      const std::string value = arg.substr(std::string("--repeat=").size());
      if (!parse_repeat_count(value, repeat_count)) {
        const auto verify_cases = cambang::verify_case_catalog(provider_kind, profiler_options);
        std::cerr << "invalid repeat count: " << value << "\n";
        usage(argv[0], verify_cases);
        return 2;
      }
      repeat_specified = true;
      continue;
    }
    if (arg == "--run-all") {
      run_all = true;
      continue;
    }
    if (arg == "--verbose") {
      run_all_verbose = true;
      continue;
    }
    if (!requested.empty()) {
      const auto verify_cases = cambang::verify_case_catalog(provider_kind, profiler_options);
      std::cerr << "unexpected extra argument: " << arg << "\n";
      usage(argv[0], verify_cases);
      return 2;
    }
    requested = arg;
  }

  const auto verify_cases = cambang::verify_case_catalog(provider_kind, profiler_options);
  if (run_all) {
    if (!requested.empty()) {
      std::cerr << "cannot combine verification case name with --run-all\n";
      usage(argv[0], verify_cases);
      return 2;
    }
    return run_all_verify_cases(verify_cases, repeat_count, run_all_verbose);
  }

  if (requested.empty()) {
    usage(argv[0], verify_cases);
    return 2;
  }

  for (const auto& verify_case : verify_cases) {
    if (verify_case.name == requested) {
      return run_single_verify_case(verify_case, provider_kind, repeat_count, repeat_specified);
    }
  }

  std::cerr << "unknown verification case: " << requested << "\n";
  usage(argv[0], verify_cases);
  return 2;
}
