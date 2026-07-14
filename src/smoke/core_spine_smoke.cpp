/*
CamBANG Maintainer Utility

Tool: core_spine_smoke

Purpose
-------
Validates the minimal Core runtime invariants using the stub provider.

This executable is a fast CI gate: it proves the core thread can start,
publish snapshots, and attach a provider without involving platform backends
or SyntheticProvider.

Category
--------
Smoke test (maintainer/CI).

Non-Goals
---------
- Not a performance benchmark
- Not a user-facing test harness (Godot)
- SyntheticProvider must not be introduced here
*/

#include <chrono>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#endif

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "core_spine_smoke: build through the repo SCons maintainer_tools alias so CAMBANG_INTERNAL_SMOKE=1 is defined."
#endif
#include "core/camera_concurrency_adc.h"
#include "core/adc_camera_description.h"
#include "core/core_runtime.h"
#include "core/provider_callback_ingress.h"
#include "core/resource_aggregate_telemetry.h"
#include "core/state_snapshot_buffer.h"

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
#include "imaging/stub/provider.h"
#endif

using namespace cambang;

namespace {

constexpr uint64_t kDeviceInstanceId = 1;
constexpr uint64_t kRootId = 1;
constexpr uint64_t kStreamId = 1;

static std::string json_quote(const std::string& value) {
  std::ostringstream oss;
  oss << '"';
  for (const char c : value) {
    switch (c) {
      case '\\': oss << "\\\\"; break;
      case '"': oss << "\\\""; break;
      case '\n': oss << "\\n"; break;
      case '\r': oss << "\\r"; break;
      case '\t': oss << "\\t"; break;
      default: oss << c; break;
    }
  }
  oss << '"';
  return oss.str();
}

static std::string make_adc_camera_concurrency_json(
    const std::vector<std::string>& camera_ids,
    bool supported,
    const std::vector<std::vector<std::string>>& combinations = {},
    std::optional<uint32_t> max_concurrent_cameras = std::nullopt,
    bool include_generator = true,
    bool include_query_error = false) {
  std::ostringstream oss;
  oss << "{\"schema_version\":"
      << camera_concurrency::ADC::kMinSupportedSchemaVersion;
  if (include_generator) {
    oss << ",\"generator\":\"core_spine_smoke\"";
  }
  oss << ",\"cameras\":[";
  for (size_t i = 0; i < camera_ids.size(); ++i) {
    if (i != 0) {
      oss << ',';
    }
    oss << "{\"camera_id\":" << json_quote(camera_ids[i]) << '}';
  }
  oss << "],\"concurrent_camera_support\":{\"supported\":"
      << (supported ? "true" : "false");
  if (max_concurrent_cameras.has_value()) {
    oss << ",\"max_concurrent_cameras\":"
        << *max_concurrent_cameras;
  }
  if (!combinations.empty()) {
    oss << ",\"camera_id_combinations\":[";
    for (size_t i = 0; i < combinations.size(); ++i) {
      if (i != 0) {
        oss << ',';
      }
      oss << '[';
      for (size_t j = 0; j < combinations[i].size(); ++j) {
        if (j != 0) {
          oss << ',';
        }
        oss << json_quote(combinations[i][j]);
      }
      oss << ']';
    }
    oss << ']';
  }
  if (include_query_error) {
    oss << ",\"error\":\"query failed\"";
  }
  oss << "}}";
  return oss.str();
}

static std::string make_realistic_full_adc_camera_concurrency_json() {
  return
      "{"
      "\"schema_version\":1,"
      "\"generator\":\"Aide-De-Cam \\u2014 \\u2713\","
      "\"cameras\":["
      "{\"camera_id\":\"camA\",\"sensor\":{\"max_aperture\":1.8,\"focal_length_mm\":4.25,\"note\":\"caf\\u00e9\"}},"
      "{\"camera_id\":\"camB\",\"lens\":{\"stabilization_gain\":5.76e-1,\"localized_name\":\"ni\\u00f1o\"}}"
      "],"
      "\"concurrent_camera_support\":{\"supported\":true,\"camera_id_combinations\":[[\"camA\",\"camB\"]]},"
      "\"ignored_numeric\":6.022e23,"
      "\"ignored_object\":{\"gamma\":2.2,\"caption\":\"ol\\u00e9\"}"
      "}";
}

static std::string make_repeated_json_nesting(std::size_t depth) {
  std::string out;
  out.reserve(depth * 2 + 4);
  for (std::size_t i = 0; i < depth; ++i) {
    out += '[';
  }
  out += '0';
  for (std::size_t i = 0; i < depth; ++i) {
    out += ']';
  }
  return out;
}

static std::vector<uint8_t> as_bytes(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

static CoreRuntime::RigPreflightResult make_manual_rig_preflight(
    uint64_t rig_id,
    const std::vector<std::string>& hardware_ids) {
  CoreRuntime::RigPreflightResult preflight{};
  preflight.ok = true;
  preflight.failure = CoreRuntime::RigPreflightFailure::None;
  preflight.rig_id = rig_id;
  for (size_t i = 0; i < hardware_ids.size(); ++i) {
    CaptureRequest request{};
    request.device_instance_id = static_cast<uint64_t>(i + 1);
    request.still_image_bundle = make_default_metered_still_image_bundle();
    preflight.participants.push_back(
        {hardware_ids[i], static_cast<uint64_t>(i + 1), request});
  }
  return preflight;
}

struct Options {
  bool stress = false;
  bool verbose = false;
  int loops = 1;           // default non-stress
  int jitter_ms = 0;       // 0 = deterministic/no sleep jitter
  uint32_t seed = 1;       // only used if jitter_ms > 0
};

enum class ParseOptsResult {
  Ok,
  Help,
  Error,
};

class QuietOutputCapture final {
public:
  QuietOutputCapture() = default;
  QuietOutputCapture(const QuietOutputCapture&) = delete;
  QuietOutputCapture& operator=(const QuietOutputCapture&) = delete;

  bool begin() {
#if !defined(_WIN32)
    return false;
#else
    std::fflush(stdout);
    std::fflush(stderr);
    saved_stdout_fd_ = _dup(_fileno(stdout));
    saved_stderr_fd_ = _dup(_fileno(stderr));
    if (saved_stdout_fd_ < 0 || saved_stderr_fd_ < 0) {
      restore_saved_fds_();
      return false;
    }
    stdout_file_.reset(std::tmpfile());
    stderr_file_.reset(std::tmpfile());
    if (!stdout_file_ || !stderr_file_) {
      end();
      return false;
    }
    if (_dup2(_fileno(stdout_file_.get()), _fileno(stdout)) != 0 ||
        _dup2(_fileno(stderr_file_.get()), _fileno(stderr)) != 0) {
      end();
      return false;
    }
    active_ = true;
    return true;
#endif
  }

  void end() {
    if (!active_) {
      restore_saved_fds_();
      stdout_file_.reset();
      stderr_file_.reset();
      return;
    }
    std::fflush(stdout);
    std::fflush(stderr);
#if defined(_WIN32)
    if (saved_stdout_fd_ >= 0) {
      (void)_dup2(saved_stdout_fd_, _fileno(stdout));
    }
    if (saved_stderr_fd_ >= 0) {
      (void)_dup2(saved_stderr_fd_, _fileno(stderr));
    }
#endif
    restore_saved_fds_();
    active_ = false;
  }

  std::string captured_stdout() const { return read_file_(stdout_file_.get()); }
  std::string captured_stderr() const { return read_file_(stderr_file_.get()); }

  ~QuietOutputCapture() {
    end();
  }

private:
  struct FileCloser {
    void operator()(FILE* f) const noexcept {
      if (f) {
        std::fclose(f);
      }
    }
  };

  static std::string read_file_(FILE* f) {
    if (!f) {
      return {};
    }
    std::fflush(f);
    if (std::fseek(f, 0, SEEK_END) != 0) {
      return {};
    }
    const long size = std::ftell(f);
    if (size <= 0) {
      std::rewind(f);
      return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    std::rewind(f);
    const size_t read = std::fread(out.data(), 1, out.size(), f);
    out.resize(read);
    return out;
  }

  void restore_saved_fds_() noexcept {
#if defined(_WIN32)
    if (saved_stdout_fd_ >= 0) {
      _close(saved_stdout_fd_);
      saved_stdout_fd_ = -1;
    }
    if (saved_stderr_fd_ >= 0) {
      _close(saved_stderr_fd_);
      saved_stderr_fd_ = -1;
    }
#endif
  }

  bool active_ = false;
  int saved_stdout_fd_ = -1;
  int saved_stderr_fd_ = -1;
  std::unique_ptr<FILE, FileCloser> stdout_file_{};
  std::unique_ptr<FILE, FileCloser> stderr_file_{};
};

class CheckReporter final {
public:
  explicit CheckReporter(bool verbose) : verbose_(verbose) {}

  template <typename Fn>
  int run(const char* name, Fn&& fn) {
    if (verbose_) {
      std::cout << "[ RUN      ] " << name << "\n";
    }
    ++run_count_;
    QuietOutputCapture quiet_capture;
    const bool capturing = !verbose_ && quiet_capture.begin();
    const int rc = fn();
    if (capturing) {
      quiet_capture.end();
    }
    if (rc == 0) {
      ++ok_count_;
      if (verbose_) {
        std::cout << "[       OK ] " << name << "\n";
      }
    } else {
      ++failed_count_;
      if (capturing) {
        const std::string captured_stdout = quiet_capture.captured_stdout();
        const std::string captured_stderr = quiet_capture.captured_stderr();
        if (!captured_stdout.empty()) {
          std::cout << captured_stdout;
        }
        if (!captured_stderr.empty()) {
          std::cerr << captured_stderr;
        }
      }
      std::cout << "[  FAILED  ] " << name << " rc=" << rc << "\n";
    }
    return rc;
  }

  void print_summary() const {
    std::fprintf(stdout,
                 "[ SUMMARY  ] run=%d ok=%d failed=%d\n",
                 run_count_,
                 ok_count_,
                 failed_count_);
    std::fflush(stdout);
  }

  void print_pass_line(const char* tool_name) const {
    std::fprintf(stdout,
                 "PASS %s run=%d ok=%d failed=%d\n",
                 tool_name,
                 run_count_,
                 ok_count_,
                 failed_count_);
    std::fflush(stdout);
  }

  void print_fail_line(const char* tool_name, const char* failed_check, int rc) const {
    std::fprintf(stdout,
                 "FAIL %s failed_check=%s rc=%d run=%d ok=%d failed=%d\n",
                 tool_name,
                 failed_check,
                 rc,
                 run_count_,
                 ok_count_,
                 failed_count_);
    std::fflush(stdout);
  }

  bool verbose() const noexcept { return verbose_; }
  int run_count() const noexcept { return run_count_; }
  int ok_count() const noexcept { return ok_count_; }
  int failed_count() const noexcept { return failed_count_; }

private:
  bool verbose_ = false;
  int run_count_ = 0;
  int ok_count_ = 0;
  int failed_count_ = 0;
};

static void usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [--stress] [--verbose] [--loops=N] [--jitter_ms=K] [--seed=S]\n"
      << "Default: run once (smoke).\n"
      << "  --stress        Enable stress loop (default loops=50).\n"
      << "  --verbose       Print per-check [RUN]/[OK] reporting.\n"
      << "  --loops=N       Number of stress iterations.\n"
      << "  --jitter_ms=K   Max jitter sleep per step (0 disables).\n"
      << "  --seed=S        RNG seed for jitter (default 1).\n";
}

static bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

static bool parse_int(const std::string& s, int& out) {
  try {
    size_t idx = 0;
    int v = std::stoi(s, &idx, 10);
    if (idx != s.size()) return false;
    out = v;
    return true;
  } catch (...) {
    return false;
  }
}

static bool parse_u32(const std::string& s, uint32_t& out) {
  try {
    size_t idx = 0;
    unsigned long v = std::stoul(s, &idx, 10);
    if (idx != s.size()) return false;
    out = static_cast<uint32_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

static ParseOptsResult parse_opts(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return ParseOptsResult::Help;
    }
    if (a == "--stress") {
      opt.stress = true;
      continue;
    }
    if (a == "--verbose") {
      opt.verbose = true;
      continue;
    }
    if (starts_with(a, "--loops=")) {
      int v = 0;
      if (!parse_int(a.substr(8), v) || v <= 0) {
        std::cerr << "Invalid --loops\n";
        return ParseOptsResult::Error;
      }
      opt.loops = v;
      continue;
    }
    if (starts_with(a, "--jitter_ms=")) {
      int v = 0;
      if (!parse_int(a.substr(12), v) || v < 0) {
        std::cerr << "Invalid --jitter_ms\n";
        return ParseOptsResult::Error;
      }
      opt.jitter_ms = v;
      continue;
    }
    if (starts_with(a, "--seed=")) {
      uint32_t v = 0;
      if (!parse_u32(a.substr(7), v)) {
        std::cerr << "Invalid --seed\n";
        return ParseOptsResult::Error;
      }
      opt.seed = v;
      continue;
    }
    std::cerr << "Unknown arg: " << a << "\n";
    usage(argv[0]);
    return ParseOptsResult::Error;
  }

  if (opt.stress && opt.loops == 1) {
    opt.loops = 50; // default stress loops
  }
  return ParseOptsResult::Ok;
}

static void maybe_jitter(const Options& opt, std::mt19937& rng) {
  if (opt.jitter_ms <= 0) return;
  std::uniform_int_distribution<int> d(0, opt.jitter_ms);
  const int ms = d(rng);
  if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static CoreDispatchStats get_dispatch_stats(CoreRuntime& rt) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    std::promise<CoreDispatchStats> p;
    auto fut = p.get_future();
    const auto r = rt.try_post([&rt, &p]() mutable { p.set_value(rt.dispatcher_stats()); });
    if (r == CoreThread::PostResult::Enqueued) {
      if (fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        return fut.get();
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::cerr << "Failed to retrieve dispatcher stats (core queue saturated or stopped).\n";
  std::exit(1);
}

static bool get_stream_record(CoreRuntime& rt, uint64_t stream_id, CoreStreamRegistry::StreamRecord& out) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    std::promise<bool> p;
    auto fut = p.get_future();
    const auto r = rt.try_post([&rt, stream_id, &out, &p]() mutable {
      const auto* rec = rt.stream_record(stream_id);
      if (!rec) {
        p.set_value(false);
        return;
      }
      out = *rec;
      p.set_value(true);
    });
    if (r == CoreThread::PostResult::Enqueued) {
      if (fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        return fut.get();
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  std::cerr << "Failed to retrieve stream record (core queue saturated or stopped).\n";
  std::exit(1);
}

static bool wait_until(std::function<bool()> pred, int max_iters = 200, int sleep_ms = 5) {
  for (int i = 0; i < max_iters; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return false;
}

static std::shared_ptr<const CamBANGStateSnapshot> get_last_snapshot(StateSnapshotBuffer& buf) {
  return buf.snapshot_copy();
}

static bool wait_for_snapshot_gen(StateSnapshotBuffer& buf, uint64_t min_gen) {
  return wait_until([&]() {
    auto s = buf.snapshot_copy();
    return s && s->gen >= min_gen;
  });
}


static bool wait_for_snapshot_version(StateSnapshotBuffer& buf, uint64_t gen, uint64_t min_version) {
  return wait_until([&]() {
    auto s = buf.snapshot_copy();
    return s && s->gen == gen && s->version >= min_version;
  });
}



static bool wait_for_snapshot_pred(
    StateSnapshotBuffer& buf,
    const std::function<bool(const CamBANGStateSnapshot&)>& pred) {
  return wait_until([&]() {
    auto s = buf.snapshot_copy();
    return s && pred(*s);
  });
}

static int test_core_spec_state_imaging_spec_retention_smoke() {
  CoreSpecState spec_state;
  spec_state.reset_for_generation(0);
  if (spec_state.imaging_spec_version() != 0 ||
      spec_state.has_imaging_spec_payload() ||
      spec_state.imaging_spec_retention_kind() != CoreSpecState::ImagingSpecRetentionKind::None) {
    std::cerr << "FAIL: initial imaging spec retained state unexpected\n";
    return 1;
  }

  const std::string unsupported_json = make_adc_camera_concurrency_json(
      {"camA", "camB"},
      false);
  const std::vector<uint8_t> replace_bytes = as_bytes(unsupported_json);
  if (!spec_state.retain_imaging_spec_replace(
          41,
          SpecPatchView{replace_bytes.data(), replace_bytes.size()})) {
    std::cerr << "FAIL: imaging spec replace retention rejected valid payload\n";
    return 1;
  }
  const std::vector<uint8_t> replace_view = spec_state.imaging_spec_payload_copy();
  if (spec_state.imaging_spec_version() != 41 ||
      !spec_state.has_imaging_spec_payload() ||
      spec_state.imaging_spec_retention_kind() != CoreSpecState::ImagingSpecRetentionKind::Replace ||
      replace_view != replace_bytes ||
      spec_state.interpret_imaging_spec().camera_concurrency.kind !=
          camera_concurrency::TruthKind::Unsupported) {
    std::cerr << "FAIL: imaging spec replace retention produced unexpected retained truth\n";
    return 1;
  }

  const std::string patch_json = make_adc_camera_concurrency_json(
      {"camA", "camB", "camC"},
      true,
      {{"camA", "camB", "camC"}, {"camB", "camC"}},
      3);
  const std::vector<uint8_t> patch_bytes = as_bytes(patch_json);
  if (!spec_state.retain_imaging_spec_patch(
          42,
          SpecPatchView{patch_bytes.data(), patch_bytes.size()})) {
    std::cerr << "FAIL: imaging spec patch retention rejected valid payload\n";
    return 1;
  }
  const std::vector<uint8_t> patch_view = spec_state.imaging_spec_payload_copy();
  if (spec_state.imaging_spec_version() != 42 ||
      !spec_state.has_imaging_spec_payload() ||
      spec_state.imaging_spec_retention_kind() != CoreSpecState::ImagingSpecRetentionKind::Patch ||
      patch_view != patch_bytes ||
      spec_state.interpret_imaging_spec().camera_concurrency.kind !=
          camera_concurrency::TruthKind::Supported ||
      spec_state.interpret_imaging_spec()
              .camera_concurrency
              .allowed_camera_id_combinations.size() != 2) {
    std::cerr << "FAIL: imaging spec patch retention produced unexpected retained truth\n";
    return 1;
  }

  if (spec_state.retain_imaging_spec_patch(43, SpecPatchView{nullptr, 2})) {
    std::cerr << "FAIL: imaging spec retention accepted invalid payload view\n";
    return 1;
  }
  if (spec_state.imaging_spec_version() != 42 ||
      spec_state.imaging_spec_retention_kind() != CoreSpecState::ImagingSpecRetentionKind::Patch) {
    std::cerr << "FAIL: invalid imaging spec retention mutated prior retained truth\n";
    return 1;
  }
  const std::string invalid_json = "{\"schema_version\":1,\"cameras\":[],";
  if (spec_state.retain_imaging_spec_patch(
          43,
          SpecPatchView{invalid_json.data(), invalid_json.size()})) {
    std::cerr << "FAIL: imaging spec retention accepted malformed ADC json payload\n";
    return 1;
  }
  if (spec_state.imaging_spec_version() != 42 ||
      spec_state.interpret_imaging_spec().camera_concurrency.kind !=
          camera_concurrency::TruthKind::Supported) {
    std::cerr << "FAIL: malformed imaging spec retention mutated prior retained truth\n";
    return 1;
  }

  spec_state.set_imaging_spec_version(44);
  if (spec_state.imaging_spec_version() != 44 ||
      spec_state.has_imaging_spec_payload() ||
      spec_state.imaging_spec_retention_kind() != CoreSpecState::ImagingSpecRetentionKind::None ||
      spec_state.interpret_imaging_spec().camera_concurrency.kind !=
          camera_concurrency::TruthKind::Unavailable) {
    std::cerr << "FAIL: imaging spec version-only retention did not clear opaque retained payload\n";
    return 1;
  }

  return 0;
}

static int test_runtime_imaging_spec_retention_publish_smoke() {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);

  if (!rt.start() ||
      !wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "FAIL: runtime start failed for imaging spec retention smoke\n";
    rt.stop();
    return 1;
  }
  if (!wait_for_snapshot_gen(buf, 0)) {
    std::cerr << "FAIL: baseline snapshot not published for imaging spec retention smoke\n";
    rt.stop();
    return 1;
  }

  const std::string replace_json = make_adc_camera_concurrency_json(
      {"camA", "camB"},
      false);
  std::vector<uint8_t> replace_bytes = as_bytes(replace_json);
  const std::vector<uint8_t> expected_replace_payload = replace_bytes;
  const auto replace_result =
      rt.retain_imaging_spec_replace(
          91,
          SpecPatchView{replace_bytes.data(), replace_bytes.size()});
  if (replace_result != CoreThread::PostResult::Enqueued) {
    std::cerr << "FAIL: imaging spec replace retention admission failed result="
              << static_cast<int>(replace_result) << "\n";
    rt.stop();
    return 1;
  }
  std::fill(std::begin(replace_bytes), std::end(replace_bytes), 0u);
  if (!wait_for_snapshot_pred(buf, [](const CamBANGStateSnapshot& s) {
        return s.gen == 0 && s.imaging_spec_version == 91 && s.version >= 1;
      })) {
    std::cerr << "FAIL: snapshot did not reflect retained imaging spec replace version\n";
    rt.stop();
    return 1;
  }
  const auto retained_replace = rt.imaging_spec_retained_state_for_smoke();
  if (!retained_replace.has_value() ||
      retained_replace->imaging_spec_version != 91 ||
      retained_replace->retention_kind != CoreSpecState::ImagingSpecRetentionKind::Replace ||
      retained_replace->payload != expected_replace_payload) {
    std::cerr << "FAIL: runtime imaging spec replace retention did not preserve async payload bytes\n";
    rt.stop();
    return 1;
  }

  const auto invalid_patch_result =
      rt.retain_imaging_spec_patch(92, SpecPatchView{nullptr, 1});
  if (invalid_patch_result != CoreThread::PostResult::Closed) {
    std::cerr << "FAIL: invalid imaging spec patch admission result="
              << static_cast<int>(invalid_patch_result) << " expected Closed\n";
    rt.stop();
    return 1;
  }

  const std::string patch_json = make_adc_camera_concurrency_json(
      {"camA", "camB", "camC"},
      true,
      {{"camA", "camB", "camC"}},
      3);
  std::vector<uint8_t> patch_bytes = as_bytes(patch_json);
  const std::vector<uint8_t> expected_patch_payload = patch_bytes;
  const auto patch_result =
      rt.retain_imaging_spec_patch(
          92,
          SpecPatchView{patch_bytes.data(), patch_bytes.size()});
  if (patch_result != CoreThread::PostResult::Enqueued) {
    std::cerr << "FAIL: imaging spec patch retention admission failed result="
              << static_cast<int>(patch_result) << "\n";
    rt.stop();
    return 1;
  }
  std::fill(std::begin(patch_bytes), std::end(patch_bytes), 0u);
  if (!wait_for_snapshot_pred(buf, [](const CamBANGStateSnapshot& s) {
        return s.gen == 0 && s.imaging_spec_version == 92 && s.version >= 2;
      })) {
    std::cerr << "FAIL: snapshot did not reflect retained imaging spec patch version\n";
    rt.stop();
    return 1;
  }
  const auto retained_patch = rt.imaging_spec_retained_state_for_smoke();
  if (!retained_patch.has_value() ||
      retained_patch->imaging_spec_version != 92 ||
      retained_patch->retention_kind != CoreSpecState::ImagingSpecRetentionKind::Patch ||
      retained_patch->payload != expected_patch_payload) {
    std::cerr << "FAIL: runtime imaging spec patch retention did not preserve async payload bytes\n";
    rt.stop();
    return 1;
  }

  const auto version_only_result = rt.retain_imaging_spec_version(93);
  if (version_only_result != CoreThread::PostResult::Enqueued) {
    std::cerr << "FAIL: imaging spec version-only retention admission failed result="
              << static_cast<int>(version_only_result) << "\n";
    rt.stop();
    return 1;
  }
  if (!wait_for_snapshot_pred(buf, [](const CamBANGStateSnapshot& s) {
        return s.gen == 0 && s.imaging_spec_version == 93 && s.version >= 3;
      })) {
    std::cerr << "FAIL: snapshot did not reflect version-only imaging spec retention\n";
    rt.stop();
    return 1;
  }
  const auto retained_version_only = rt.imaging_spec_retained_state_for_smoke();
  if (!retained_version_only.has_value() ||
      retained_version_only->imaging_spec_version != 93 ||
      retained_version_only->retention_kind != CoreSpecState::ImagingSpecRetentionKind::None ||
      !retained_version_only->payload.empty()) {
    std::cerr << "FAIL: version-only imaging spec retention left stale retained payload state\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_adc_camera_description_parser_and_retention_smoke() {
  const std::string populated = R"JSON({
    "schema_version":2,
    "generator":"Aide-De-Cam",
    "generator_version":"2.0",
    "timestamp_ms":1780000000000,
    "device_model":"ignored",
    "ignored_root":{"additive":true},
    "cameras":[
      {"camera_id":"Cam A ","ignored_camera":5,
       "facing":{"source":"native_reported","value":"back"},
       "camera_nature":{"source":"user_supplied","value":"physical"},
       "sensor_orientation":{"source":"native_reported","value_degrees":0},
       "intrinsics":{"source":"native_reported","focal_length_x_px":3120.4,"focal_length_y_px":3118.9,"principal_point_x_px":2014.3,"principal_point_y_px":1508.7,"skew_px":0.0,"reference_width_px":4032,"reference_height_px":3024,"coordinate_domain":"delivered_image"},
       "distortion":{"source":"derived","model":"brown_conrady_5","radial_k1":0.0,"radial_k2":0.0,"radial_k3":0.0,"tangential_p1":0.0,"tangential_p2":0.0,"reference_width_px":4032,"reference_height_px":3024,"coordinate_domain":"delivered_image","image_state":"distorted"},
       "pose":{"source":"user_supplied","reference_kind":"custom_reference","reference_id":"rig frame","coordinate_convention":"camera_optical_frame","translation_m":[0,0,0],"rotation_xyzw":[0,0,0,2]}},
      {"camera_id":"cam-b",
       "distortion":{"source":"virtual_camera_authored","model":"none","image_state":"rectified"},
       "pose":{"source":"virtual_camera_authored","reference_kind":"platform_defined","platform_defined_reference":"native rig","coordinate_convention":"platform_defined","platform_defined_convention":"native pose","translation_m":[1,2,3],"rotation_xyzw":[0,0,0,1]}}
    ],
    "concurrent_camera_support":{"supported":true,"camera_id_combinations":[["Cam A ","cam-b"]]}
  })JSON";

  const auto loaded = adc_camera_description::load_replacement_from_json_text(populated);
  if (!loaded.ok || loaded.state.entries().size() != 2 ||
      !loaded.state.concurrency().has_value() ||
      loaded.state.concurrency()->kind != camera_concurrency::TruthKind::Supported) {
    std::cerr << "FAIL: populated ADC camera-description replacement did not parse\n";
    return 1;
  }
  const auto* camera_a = loaded.state.find_exact("Cam A ");
  if (!camera_a || loaded.state.find_exact("cam a ") || !camera_a->facts.facing ||
      camera_a->facts.facing->value != CameraFacing::BACK ||
      !camera_a->facts.sensor_orientation ||
      camera_a->facts.sensor_orientation->value != SensorOrientationDegrees::DEGREES_0 ||
      !camera_a->facts.intrinsics || !camera_a->facts.intrinsics->value.skew_px() ||
      *camera_a->facts.intrinsics->value.skew_px() != 0.0 ||
      !camera_a->facts.distortion || !std::holds_alternative<BrownConrady5Distortion>(camera_a->facts.distortion->value) ||
      !camera_a->facts.pose) {
    std::cerr << "FAIL: ADC camera-description static fact mapping was incomplete\n";
    return 1;
  }
  const auto* camera_b = loaded.state.find_exact("cam-b");
  if (!camera_b || !camera_b->facts.distortion ||
      !std::holds_alternative<NoDistortion>(camera_b->facts.distortion->value) ||
      !camera_b->facts.pose) {
    std::cerr << "FAIL: ADC camera-description no-distortion or platform pose mapping failed\n";
    return 1;
  }

  const auto absent_concurrency = adc_camera_description::load_replacement_from_json_text(
      R"JSON({"schema_version":2,"cameras":[{"camera_id":"unmatched"}]})JSON");
  const auto unsupported_concurrency = adc_camera_description::load_replacement_from_json_text(
      R"JSON({"schema_version":2,"cameras":[],"concurrent_camera_support":{"supported":false}})JSON");
  if (!absent_concurrency.ok || absent_concurrency.state.concurrency().has_value() ||
      !unsupported_concurrency.ok || !unsupported_concurrency.state.entries().empty() ||
      !unsupported_concurrency.state.concurrency().has_value() ||
      unsupported_concurrency.state.concurrency()->kind != camera_concurrency::TruthKind::Unsupported) {
    std::cerr << "FAIL: ADC camera-description absent/unsupported concurrency semantics failed\n";
    return 1;
  }

  const auto make_numeric_intrinsics_document = [](const std::string& skew) {
    return "{\"schema_version\":2,\"cameras\":[{\"camera_id\":\"numeric\",\"intrinsics\":{"
           "\"source\":\"user_supplied\",\"focal_length_x_px\":1,\"focal_length_y_px\":1,"
           "\"principal_point_x_px\":0,\"principal_point_y_px\":0,\"skew_px\":" + skew +
           ",\"reference_width_px\":1,\"reference_height_px\":1,"
           "\"coordinate_domain\":\"delivered_image\"}}]}";
  };
  const std::vector<std::string> valid_numeric_lexemes = {
      "1", "1.25", "-1.25", "1e2", "1E2", "-0", "0", "1e308"};
  for (const std::string& lexeme : valid_numeric_lexemes) {
    const auto numeric = adc_camera_description::load_replacement_from_json_text(
        make_numeric_intrinsics_document(lexeme));
    const auto* entry = numeric.ok ? numeric.state.find_exact("numeric") : nullptr;
    if (!entry || !entry->facts.intrinsics || !entry->facts.intrinsics->value.skew_px()) {
      std::cerr << "FAIL: ADC camera-description floating numeric lexeme was rejected: "
                << lexeme << '\n';
      return 1;
    }
    if (lexeme == "-0" && !std::signbit(*entry->facts.intrinsics->value.skew_px())) {
      std::cerr << "FAIL: ADC camera-description floating negative zero was not preserved\n";
      return 1;
    }
  }
  for (const char* lexeme : {"1e309", "1e-9999"}) {
    if (adc_camera_description::load_replacement_from_json_text(
            make_numeric_intrinsics_document(lexeme)).ok) {
      std::cerr << "FAIL: ADC camera-description accepted out-of-range floating numeric lexeme: "
                << lexeme << '\n';
      return 1;
    }
  }
  for (const char* malformed_number : {"1.25x", "1e+", "1,25"}) {
    if (adc_camera_description::load_replacement_from_json_text(
            make_numeric_intrinsics_document(malformed_number)).ok) {
      std::cerr << "FAIL: ADC camera-description accepted malformed JSON numeric lexeme: "
                << malformed_number << '\n';
      return 1;
    }
  }

  const std::vector<std::string> invalid_documents = {
      "{\"schema_version\":2",
      R"JSON({"schema_version":3,"cameras":[]})JSON",
      R"JSON({"schema_version":2})JSON",
      R"JSON({"schema_version":2,"cameras":[{"camera_id":"a"},{"camera_id":"a"}]})JSON",
      R"JSON({"schema_version":2,"cameras":[{"camera_id":"a","facing":{"source":"native_reported","value":"side"}}]})JSON",
      R"JSON({"schema_version":2,"cameras":[{"camera_id":"a","intrinsics":{"source":"native_reported","focal_length_x_px":1,"focal_length_y_px":1,"principal_point_x_px":1,"principal_point_y_px":1,"reference_width_px":0,"reference_height_px":1,"coordinate_domain":"delivered_image"}}]})JSON",
      R"JSON({"schema_version":2,"cameras":[{"camera_id":"a","distortion":{"source":"native_reported","model":"none","image_state":"rectified","radial_k1":0}}]})JSON",
      R"JSON({"schema_version":2,"cameras":[{"camera_id":"a","pose":{"source":"native_reported","reference_kind":"camera","reference_camera_id":"a","coordinate_convention":"camera_optical_frame","translation_m":[0,0,0],"rotation_xyzw":[0,0,0,1]}}]})JSON",
      R"JSON({"schema_version":2,"cameras":[{"camera_id":"a","pose":{"source":"native_reported","reference_kind":"custom_reference","reference_id":"","coordinate_convention":"camera_optical_frame","translation_m":[0,0,0],"rotation_xyzw":[0,0,0,1]}}]})JSON",
      R"JSON({"schema_version":2,"cameras":[{"camera_id":"a"},{"camera_id":"b"}],"concurrent_camera_support":{"supported":false,"camera_id_combinations":[["a","b"]]}})JSON",
      R"JSON({"schema_version":2,"cameras":[{"camera_id":"a"},{"camera_id":"b"}],"concurrent_camera_support":{"supported":true,"camera_id_combinations":[["a","a"]]}})JSON"};
  for (const std::string& json : invalid_documents) {
    if (adc_camera_description::load_replacement_from_json_text(json).ok) {
      std::cerr << "FAIL: ADC camera-description parser accepted invalid semantic document\n";
      return 1;
    }
  }
  if (adc_camera_description::load_replacement_from_json_text(
          std::string(adc_camera_description::max_supported_input_bytes() + 1, ' ')).ok ||
      adc_camera_description::load_replacement_from_json_text(
          make_repeated_json_nesting(adc_camera_description::max_supported_nesting_depth() + 1)).ok ||
      adc_camera_description::load_replacement_from_json_text(
          "{\"schema_version\":2,\"generator\":\"" +
          std::string(adc_camera_description::max_supported_string_bytes() + 1, 'x') +
          "\",\"cameras\":[]}").ok) {
    std::cerr << "FAIL: ADC camera-description parser resource limits were not enforced\n";
    return 1;
  }
  const auto make_camera_array = [](size_t count) {
    std::ostringstream json;
    json << "{\"schema_version\":2,\"cameras\":[";
    for (size_t i = 0; i < count; ++i) {
      if (i != 0) json << ',';
      json << "{\"camera_id\":\"cam" << i << "\"}";
    }
    return json.str() + "]}";
  };
  if (adc_camera_description::load_replacement_from_json_text(
          make_camera_array(adc_camera_description::max_supported_camera_records() + 1)).ok) {
    std::cerr << "FAIL: ADC camera-description camera-count limit was not enforced\n";
    return 1;
  }
  std::ostringstream too_many_combinations;
  too_many_combinations << "{\"schema_version\":2,\"cameras\":[{\"camera_id\":\"a\"},{\"camera_id\":\"b\"}],\"concurrent_camera_support\":{\"supported\":true,\"camera_id_combinations\":[";
  for (size_t i = 0; i <= adc_camera_description::max_supported_combination_count(); ++i) {
    if (i != 0) too_many_combinations << ',';
    too_many_combinations << "[\"a\",\"b\"]";
  }
  too_many_combinations << "]}}";
  if (adc_camera_description::load_replacement_from_json_text(too_many_combinations.str()).ok) {
    std::cerr << "FAIL: ADC camera-description combination-count limit was not enforced\n";
    return 1;
  }
  std::ostringstream too_many_members;
  too_many_members << "{\"schema_version\":2,\"cameras\":[";
  for (size_t i = 0; i <= adc_camera_description::max_supported_combination_members(); ++i) {
    if (i != 0) too_many_members << ',';
    too_many_members << "{\"camera_id\":\"member" << i << "\"}";
  }
  too_many_members << "],\"concurrent_camera_support\":{\"supported\":true,\"camera_id_combinations\":[[";
  for (size_t i = 0; i <= adc_camera_description::max_supported_combination_members(); ++i) {
    if (i != 0) too_many_members << ',';
    too_many_members << "\"member" << i << "\"";
  }
  too_many_members << "]]}}";
  if (adc_camera_description::load_replacement_from_json_text(too_many_members.str()).ok) {
    std::cerr << "FAIL: ADC camera-description combination-member limit was not enforced\n";
    return 1;
  }

  CoreRuntime rt;
  const auto configured = rt.replace_external_camera_description_json_for_internal(populated);
  if (configured.status != CoreRuntime::ReplaceExternalCameraDescriptionStatus::Ok ||
      configured.camera_description_version == 0 || configured.imaging_spec_version == 0 || !rt.start() ||
      !wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1) ||
      rt.active_external_camera_description_count_for_smoke() != 2 ||
      rt.active_external_camera_description_version_for_smoke() != configured.camera_description_version ||
      !rt.active_external_camera_description_for_smoke("Cam A ")) {
    std::cerr << "FAIL: ADC camera-description configured state did not persist into active generation\n";
    rt.stop();
    return 1;
  }
  const auto allowed = rt.smoke_admit_rig_cohort_from_preflight(
      9100, 9101, make_manual_rig_preflight(9100, {"Cam A ", "cam-b"}));
  if (!allowed.ok || rt.replace_external_camera_description_json_for_internal(populated).status !=
                         CoreRuntime::ReplaceExternalCameraDescriptionStatus::Busy) {
    std::cerr << "FAIL: ADC camera-description concurrency projection or running rejection failed\n";
    rt.stop();
    return 1;
  }
  rt.stop();
  if (rt.replace_external_camera_description_json_for_internal("{\"schema_version\":2").status !=
      CoreRuntime::ReplaceExternalCameraDescriptionStatus::ParseError) {
    std::cerr << "FAIL: ADC camera-description malformed replacement did not reject\n";
    return 1;
  }
  if (!rt.start() || !wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1) ||
      rt.active_external_camera_description_count_for_smoke() != 2 ||
      rt.active_external_camera_description_version_for_smoke() != configured.camera_description_version) {
    std::cerr << "FAIL: rejected ADC camera-description replacement mutated retained state\n";
    rt.stop();
    return 1;
  }
  rt.stop();
  const auto replacement = rt.replace_external_camera_description_json_for_internal(
      R"JSON({"schema_version":2,"cameras":[{"camera_id":"Cam A ","facing":{"source":"user_supplied","value":"front"}}]})JSON");
  if (replacement.status != CoreRuntime::ReplaceExternalCameraDescriptionStatus::Ok ||
      replacement.camera_description_version != configured.camera_description_version + 1 ||
      replacement.imaging_spec_version != 0 || !rt.start() ||
      !wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1) ||
      rt.active_external_camera_description_count_for_smoke() != 1 ||
      rt.active_external_camera_description_version_for_smoke() != replacement.camera_description_version) {
    std::cerr << "FAIL: ADC camera-description full replacement did not remove omitted entries\n";
    rt.stop();
    return 1;
  }
  const auto reduced = rt.active_external_camera_description_for_smoke("Cam A ");
  const auto unavailable = rt.smoke_admit_rig_cohort_from_preflight(
      9100, 9102, make_manual_rig_preflight(9100, {"Cam A ", "cam-b"}));
  const auto no_effective_spec = rt.imaging_spec_retained_state_for_smoke();
  if (!reduced || !reduced->facts.facing || reduced->facts.intrinsics ||
      !no_effective_spec || no_effective_spec->imaging_spec_version != 0 ||
      unavailable.failure != CoreRuntime::RigCohortAdmissionFailure::ImagingSpecUnavailable) {
    std::cerr << "FAIL: ADC facts-only replacement did not clear effective ImagingSpec truth\n";
    rt.stop();
    return 1;
  }
  rt.stop();

  const auto rejected_legacy = rt.ingest_camera_concurrency_json_for_server("{\"schema_version\":1");
  if (rejected_legacy.status != CoreRuntime::IngestCameraConcurrencyStatus::ParseError || !rt.start() ||
      !wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1) ||
      rt.active_external_camera_description_count_for_smoke() != 1 ||
      rt.active_external_camera_description_version_for_smoke() != replacement.camera_description_version ||
      !rt.imaging_spec_retained_state_for_smoke() ||
      rt.imaging_spec_retained_state_for_smoke()->imaging_spec_version != 0) {
    std::cerr << "FAIL: rejected legacy replacement mutated facts-only camera-description state\n";
    rt.stop();
    return 1;
  }
  rt.stop();

  const auto legacy = rt.ingest_camera_concurrency_json_for_server(
      make_adc_camera_concurrency_json({"Cam A ", "cam-b"}, true, {{"Cam A ", "cam-b"}}, 2));
  if (legacy.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok || legacy.imaging_spec_version == 0 ||
      !rt.start() || !wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1) ||
      rt.active_external_camera_description_count_for_smoke() != 0 ||
      rt.active_external_camera_description_version_for_smoke() != 0) {
    std::cerr << "FAIL: accepted legacy replacement did not clear typed external camera-description state\n";
    rt.stop();
    return 1;
  }
  rt.stop();
  const auto rejected_v2 = rt.replace_external_camera_description_json_for_internal("{\"schema_version\":2");
  if (rejected_v2.status != CoreRuntime::ReplaceExternalCameraDescriptionStatus::ParseError || !rt.start() ||
      !wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1) ||
      rt.active_external_camera_description_count_for_smoke() != 0 ||
      rt.active_external_camera_description_version_for_smoke() != 0 ||
      !rt.imaging_spec_retained_state_for_smoke() ||
      rt.imaging_spec_retained_state_for_smoke()->imaging_spec_version != legacy.imaging_spec_version) {
    std::cerr << "FAIL: rejected camera-description replacement mutated legacy configured truth\n";
    rt.stop();
    return 1;
  }
  rt.stop();

  const auto restored = rt.replace_external_camera_description_json_for_internal(populated);
  if (restored.status != CoreRuntime::ReplaceExternalCameraDescriptionStatus::Ok ||
      restored.camera_description_version != replacement.camera_description_version + 1 ||
      restored.imaging_spec_version == 0 || !rt.start() ||
      !wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1) ||
      rt.active_external_camera_description_count_for_smoke() != 2 ||
      rt.active_external_camera_description_version_for_smoke() != restored.camera_description_version) {
    std::cerr << "FAIL: accepted camera-description replacement did not replace legacy raw state\n";
    rt.stop();
    return 1;
  }
  const auto restored_spec = rt.imaging_spec_retained_state_for_smoke();
  const auto restored_admission = rt.smoke_admit_rig_cohort_from_preflight(
      9100, 9103, make_manual_rig_preflight(9100, {"Cam A ", "cam-b"}));
  if (!restored_spec || restored_spec->imaging_spec_version != restored.imaging_spec_version ||
      restored_spec->retention_kind != CoreSpecState::ImagingSpecRetentionKind::None ||
      !restored_spec->payload.empty() || !restored_admission.ok) {
    std::cerr << "FAIL: camera-description replacement did not restore projected grouped-rig admission\n";
    rt.stop();
    return 1;
  }
  rt.stop();

  const auto cleared = rt.replace_external_camera_description_json_for_internal(
      R"JSON({"schema_version":2,"cameras":[]})JSON");
  if (cleared.status != CoreRuntime::ReplaceExternalCameraDescriptionStatus::Ok ||
      cleared.camera_description_version != restored.camera_description_version + 1 ||
      cleared.imaging_spec_version != 0 || !rt.start() ||
      !wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1) ||
      rt.active_external_camera_description_count_for_smoke() != 0 ||
      rt.active_external_camera_description_version_for_smoke() != cleared.camera_description_version ||
      !rt.imaging_spec_retained_state_for_smoke() ||
      rt.imaging_spec_retained_state_for_smoke()->imaging_spec_version != 0) {
    std::cerr << "FAIL: ADC camera-description cameras[] clear failed\n";
    rt.stop();
    return 1;
  }
  rt.stop();
  return 0;
}

static int test_runtime_camera_concurrency_ingest_lifecycle_smoke() {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);

  const std::string supported_json = make_adc_camera_concurrency_json(
      {"camA", "camB", "camC", "camD"},
      true,
      {{"camA", "camB", "camC"}},
      3);
  const std::string unsupported_json = make_adc_camera_concurrency_json(
      {"camA", "camB", "camC"},
      false,
      {},
      3);

  const auto malformed =
      rt.ingest_camera_concurrency_json_for_server("{\"schema_version\":1");
  if (malformed.status != CoreRuntime::IngestCameraConcurrencyStatus::ParseError) {
    std::cerr << "FAIL: malformed camera concurrency ingestion did not return ParseError\n";
    return 1;
  }

  const auto configured_supported =
      rt.ingest_camera_concurrency_json_for_server(supported_json);
  if (configured_supported.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok ||
      configured_supported.imaging_spec_version == 0) {
    std::cerr << "FAIL: stopped-time supported camera concurrency ingestion failed\n";
    return 1;
  }

  if (!rt.start()) {
    std::cerr << "FAIL: runtime start failed for camera concurrency lifecycle smoke\n";
    rt.stop();
    return 1;
  }
  if (!wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return s.gen == 0 && s.version == 0 &&
               s.imaging_spec_version == configured_supported.imaging_spec_version;
      })) {
    std::cerr << "FAIL: first baseline did not reuse stopped-time configured camera concurrency truth\n";
    rt.stop();
    return 1;
  }

  const auto busy_running =
      rt.ingest_camera_concurrency_json_for_server(unsupported_json);
  if (busy_running.status != CoreRuntime::IngestCameraConcurrencyStatus::Busy) {
    std::cerr << "FAIL: running-time camera concurrency ingestion did not preserve active-generation immutability\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  if (!rt.start()) {
    std::cerr << "FAIL: second runtime start failed for camera concurrency lifecycle smoke\n";
    rt.stop();
    return 1;
  }
  if (!wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return s.gen == 1 && s.version == 0 &&
               s.imaging_spec_version == configured_supported.imaging_spec_version;
      })) {
    std::cerr << "FAIL: stopped-time configured camera concurrency truth was not reused on restart\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  const auto invalid_semantic = rt.ingest_camera_concurrency_json_for_server(
      make_adc_camera_concurrency_json({"camA", "camB"}, true, {{"camA"}}, 3));
  if (invalid_semantic.status != CoreRuntime::IngestCameraConcurrencyStatus::Invalid) {
    std::cerr << "FAIL: invalid semantic camera concurrency ingestion did not return Invalid\n";
    return 1;
  }

  if (!rt.start()) {
    std::cerr << "FAIL: third runtime start failed for camera concurrency lifecycle smoke\n";
    rt.stop();
    return 1;
  }
  if (!wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return s.gen == 2 && s.version == 0 &&
               s.imaging_spec_version == configured_supported.imaging_spec_version;
      })) {
    std::cerr << "FAIL: failed stopped-time ingestion mutated prior configured camera concurrency truth\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  const auto configured_unsupported =
      rt.ingest_camera_concurrency_json_for_server(unsupported_json);
  if (configured_unsupported.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok ||
      configured_unsupported.imaging_spec_version <=
          configured_supported.imaging_spec_version) {
    std::cerr << "FAIL: stopped-time unsupported camera concurrency ingestion failed to replace configured truth\n";
    return 1;
  }
  if (!rt.start()) {
    std::cerr << "FAIL: fourth runtime start failed for camera concurrency lifecycle smoke\n";
    rt.stop();
    return 1;
  }
  if (!wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return s.gen == 3 && s.version == 0 &&
               s.imaging_spec_version == configured_unsupported.imaging_spec_version;
      })) {
    std::cerr << "FAIL: replaced stopped-time camera concurrency truth was not applied on later start\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_runtime_camera_concurrency_invalid_input_smoke() {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);

  const auto configured = rt.ingest_camera_concurrency_json_for_server(
      make_adc_camera_concurrency_json(
          {"camA", "camB", "camC"},
          true,
          {{"camA", "camB", "camC"}},
          3));
  if (configured.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok ||
      configured.imaging_spec_version == 0) {
    std::cerr << "FAIL: baseline camera concurrency ingestion failed before invalid-input checks\n";
    return 1;
  }

  const auto no_generator = rt.ingest_camera_concurrency_json_for_server(
      make_adc_camera_concurrency_json(
          {"camA", "camB"},
          true,
          {{"camA", "camB"}},
          2,
          false));
  if (no_generator.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok ||
      no_generator.imaging_spec_version <= configured.imaging_spec_version) {
    std::cerr << "FAIL: generator-absent camera concurrency ingestion should be accepted transactionally\n";
    return 1;
  }

  const auto non_adc_generator = rt.ingest_camera_concurrency_json_for_server(
      "{\"schema_version\":1,\"generator\":\"third_party_tool\",\"cameras\":[{\"camera_id\":\"camA\"},{\"camera_id\":\"camB\"}],\"concurrent_camera_support\":{\"supported\":true,\"camera_id_combinations\":[[\"camA\",\"camB\"]]}}");
  if (non_adc_generator.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok ||
      non_adc_generator.imaging_spec_version <= no_generator.imaging_spec_version) {
    std::cerr << "FAIL: non-ADC generator identity must not affect accepted camera concurrency truth\n";
    return 1;
  }

  const auto realistic_full_adc = rt.ingest_camera_concurrency_json_for_server(
      make_realistic_full_adc_camera_concurrency_json());
  if (realistic_full_adc.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok ||
      realistic_full_adc.imaging_spec_version <=
          non_adc_generator.imaging_spec_version) {
    std::cerr << "FAIL: realistic full ADC document with floats/unicode should be accepted when concurrency projection is valid\n";
    return 1;
  }

  const auto unsupported_absent = rt.ingest_camera_concurrency_json_for_server(
      make_adc_camera_concurrency_json({"camA", "camB"}, false));
  if (unsupported_absent.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok ||
      unsupported_absent.imaging_spec_version <= realistic_full_adc.imaging_spec_version) {
    std::cerr << "FAIL: supported=false without camera_id_combinations should remain valid authoritative unsupported truth\n";
    return 1;
  }

  const auto unsupported_empty = rt.ingest_camera_concurrency_json_for_server(
      "{\"schema_version\":1,\"cameras\":[{\"camera_id\":\"camA\"},{\"camera_id\":\"camB\"}],\"concurrent_camera_support\":{\"supported\":false,\"camera_id_combinations\":[]}}");
  if (unsupported_empty.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok ||
      unsupported_empty.imaging_spec_version <= unsupported_absent.imaging_spec_version) {
    std::cerr << "FAIL: supported=false with empty camera_id_combinations should be accepted as authoritative unsupported truth\n";
    return 1;
  }

  struct InvalidCase {
    const char* label;
    std::string json;
    CoreRuntime::IngestCameraConcurrencyStatus expected_status;
  };
  const std::vector<InvalidCase> cases = {
      {"missing_schema_version",
       "{\"cameras\":[{\"camera_id\":\"camA\"}],\"concurrent_camera_support\":{\"supported\":false}}",
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"non_integral_schema_version",
       "{\"schema_version\":1.5,\"cameras\":[{\"camera_id\":\"camA\"}],\"concurrent_camera_support\":{\"supported\":false}}",
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"unsupported_version",
       "{\"schema_version\":2,\"cameras\":[{\"camera_id\":\"camA\"}],\"concurrent_camera_support\":{\"supported\":false}}",
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"bad_generator_type",
       "{\"schema_version\":1,\"generator\":17,\"cameras\":[{\"camera_id\":\"camA\"}],\"concurrent_camera_support\":{\"supported\":false}}",
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"missing_concurrency_truth",
       "{\"schema_version\":1,\"cameras\":[{\"camera_id\":\"camA\"}]}",
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"query_error",
       make_adc_camera_concurrency_json({"camA", "camB"}, true, {{"camA", "camB"}}, 2, true, true),
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"supported_without_combinations",
       "{\"schema_version\":1,\"cameras\":[{\"camera_id\":\"camA\"},{\"camera_id\":\"camB\"}],\"concurrent_camera_support\":{\"supported\":true}}",
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"contradictory_support_and_combinations",
       make_adc_camera_concurrency_json({"camA", "camB"}, false, {{"camA", "camB"}}, 2),
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"unsupported_non_empty_with_unicode",
       "{\"schema_version\":1,\"cameras\":[{\"camera_id\":\"camA\",\"label\":\"caf\\u00e9\"},{\"camera_id\":\"camB\"}],\"concurrent_camera_support\":{\"supported\":false,\"camera_id_combinations\":[[\"camA\",\"camB\"]]}}",
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"singleton_combination",
       make_adc_camera_concurrency_json({"camA", "camB"}, true, {{"camA"}}, 2),
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"duplicate_normalized_combinations",
       make_adc_camera_concurrency_json(
           {"camA", "camB", "camC"},
           true,
           {{"camA", "camB"}, {"camB", "camA"}},
           2),
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"duplicate_members",
       make_adc_camera_concurrency_json(
           {"camA", "camB", "camC"},
           true,
           {{"camA", "camA"}},
           2),
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"bad_camera_id",
       "{\"schema_version\":1,\"cameras\":[{\"camera_id\":\"\"}],\"concurrent_camera_support\":{\"supported\":false}}",
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"unknown_combo_id",
       make_adc_camera_concurrency_json(
           {"camA", "camB"},
           true,
           {{"camA", "camC"}},
           2),
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"invalid_maximum",
       "{\"schema_version\":1,\"cameras\":[{\"camera_id\":\"camA\"},{\"camera_id\":\"camB\"}],\"concurrent_camera_support\":{\"supported\":true,\"max_concurrent_cameras\":1,\"camera_id_combinations\":[[\"camA\",\"camB\"]]}}",
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"oversized_input",
       std::string(camera_concurrency::max_supported_input_bytes() + 1, ' '),
       CoreRuntime::IngestCameraConcurrencyStatus::ParseError},
      {"excessive_nesting",
       make_repeated_json_nesting(camera_concurrency::max_supported_nesting_depth() + 1),
       CoreRuntime::IngestCameraConcurrencyStatus::ParseError},
      {"oversized_string",
       "{\"schema_version\":1,\"generator\":\"" +
           std::string(camera_concurrency::max_supported_string_bytes() + 1, 'x') +
           "\",\"cameras\":[{\"camera_id\":\"camA\"}],\"concurrent_camera_support\":{\"supported\":false}}",
       CoreRuntime::IngestCameraConcurrencyStatus::ParseError},
      {"too_many_cameras",
       []() {
         std::vector<std::string> camera_ids;
         camera_ids.reserve(
             camera_concurrency::max_supported_camera_records() + 1);
         for (std::size_t i = 0;
              i < camera_concurrency::max_supported_camera_records() + 1;
              ++i) {
           camera_ids.push_back("cam" + std::to_string(i));
         }
         return make_adc_camera_concurrency_json(camera_ids, false);
       }(),
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"too_many_combinations",
       []() {
         std::vector<std::string> camera_ids;
         std::vector<std::vector<std::string>> combinations;
         camera_ids.reserve(
             camera_concurrency::max_supported_combination_count() + 2);
         combinations.reserve(
             camera_concurrency::max_supported_combination_count() + 1);
         for (std::size_t i = 0;
              i < camera_concurrency::max_supported_combination_count() + 2;
              ++i) {
           camera_ids.push_back("cam" + std::to_string(i));
         }
         for (std::size_t i = 0;
              i < camera_concurrency::max_supported_combination_count() + 1;
              ++i) {
           combinations.push_back({camera_ids[i], camera_ids[i + 1]});
         }
         return make_adc_camera_concurrency_json(
             camera_ids,
             true,
             combinations,
             2);
       }(),
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
      {"too_many_members_in_combination",
       []() {
         std::vector<std::string> camera_ids;
         std::vector<std::string> combination;
         camera_ids.reserve(
             camera_concurrency::max_supported_combination_members() + 1);
         combination.reserve(
             camera_concurrency::max_supported_combination_members() + 1);
         for (std::size_t i = 0;
              i < camera_concurrency::max_supported_combination_members() + 1;
              ++i) {
           const std::string id = "cam" + std::to_string(i);
           camera_ids.push_back(id);
           combination.push_back(id);
         }
         return make_adc_camera_concurrency_json(
             camera_ids,
             true,
             {combination},
             static_cast<uint32_t>(combination.size()));
       }(),
       CoreRuntime::IngestCameraConcurrencyStatus::Invalid},
  };

  for (const auto& c : cases) {
    const auto result = rt.ingest_camera_concurrency_json_for_server(c.json);
    if (result.status != c.expected_status) {
      std::cerr << "FAIL: invalid camera concurrency case '" << c.label
                << "' returned status=" << static_cast<int>(result.status)
                << " expected=" << static_cast<int>(c.expected_status) << "\n";
      return 1;
    }
    if (result.imaging_spec_version != 0) {
      std::cerr << "FAIL: invalid camera concurrency case '" << c.label
                << "' advanced configured imaging_spec_version\n";
      return 1;
    }
  }

  if (!rt.start()) {
    std::cerr << "FAIL: runtime start failed after invalid camera concurrency checks\n";
    rt.stop();
    return 1;
  }
  if (!wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return s.gen == 0 && s.version == 0 &&
               s.imaging_spec_version == unsupported_empty.imaging_spec_version;
      })) {
    std::cerr << "FAIL: invalid camera concurrency ingests did not preserve prior configured truth transactionally\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}


class RefusingDeviceProvider final : public ICameraProvider {
public:
  ProviderResult initialize(IProviderCallbacks* callbacks) override {
    callbacks_ = callbacks;
    return ProviderResult::success();
  }

  const char* provider_name() const override { return "refusing_device_provider"; }
  ProviderKind provider_kind() const noexcept override { return ProviderKind::platform_backed; }

  StreamTemplate stream_template() const override {
    StreamTemplate t{};
    t.profile.width = 640;
    t.profile.height = 480;
    t.profile.format_fourcc = FOURCC_RGBA;
    t.profile.target_fps_min = 30;
    t.profile.target_fps_max = 30;
    return t;
  }

  CaptureTemplate capture_template() const override {
    CaptureTemplate t{};
    t.profile.width = 640;
    t.profile.height = 480;
    t.profile.format_fourcc = FOURCC_RGBA;
    t.profile.target_fps_min = 30;
    t.profile.target_fps_max = 30;
    return t;
  }

  bool supports_stream_picture_updates() const noexcept override { return false; }
  bool supports_capture_picture_updates() const noexcept override { return false; }
  bool supports_multi_image_still_sequence() const noexcept override { return false; }

  ProviderResult enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) override {
    CameraEndpoint ep{};
    ep.hardware_id = "refusing:0";
    ep.name = "Refusing Device";
    out_endpoints.push_back(std::move(ep));
    return ProviderResult::success();
  }

  ProviderResult open_device(const std::string&, uint64_t, uint64_t) override {
    open_called_.fetch_add(1, std::memory_order_relaxed);
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  ProviderResult close_device(uint64_t) override {
    close_called_.fetch_add(1, std::memory_order_relaxed);
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  ProviderResult create_stream(const StreamRequest&) override { return ProviderResult::failure(ProviderError::ERR_BAD_STATE); }
  ProviderResult destroy_stream(uint64_t) override { return ProviderResult::failure(ProviderError::ERR_BAD_STATE); }
  ProviderResult start_stream(uint64_t, const CaptureProfile&, const PictureConfig&) override {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  ProviderResult stop_stream(uint64_t) override { return ProviderResult::failure(ProviderError::ERR_BAD_STATE); }
  ProviderResult set_stream_picture_config(uint64_t, const PictureConfig&) override {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  ProviderResult set_capture_picture_config(uint64_t, const PictureConfig&) override {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  ProviderResult trigger_capture(const CaptureRequest&) override { return ProviderResult::failure(ProviderError::ERR_BAD_STATE); }
  ProviderResult abort_capture(uint64_t) override { return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED); }
  ProviderResult apply_camera_spec_patch(const std::string&, uint64_t, SpecPatchView) override {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  ProviderResult apply_imaging_spec_patch(uint64_t, SpecPatchView) override {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  ProviderResult shutdown() override {
    callbacks_ = nullptr;
    return ProviderResult::success();
  }

  uint64_t open_called() const noexcept { return open_called_.load(std::memory_order_relaxed); }
  uint64_t close_called() const noexcept { return close_called_.load(std::memory_order_relaxed); }

private:
  IProviderCallbacks* callbacks_ = nullptr;
  std::atomic<uint64_t> open_called_{0};
  std::atomic<uint64_t> close_called_{0};
};

static const char* compiled_provider_name() {
#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
  return "stub";
#else
  return "unset";
#endif
}

static StreamRequest make_req() {
  StreamRequest req{};
  req.stream_id = kStreamId;
  req.device_instance_id = kDeviceInstanceId;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 2;
  req.profile.height = 2;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 0;
  req.profile.target_fps_max = 0;

  req.picture.preset = PatternPreset::XyXor;
  req.picture.seed = 0;
  req.picture.overlay_frame_index_offsets = false;
  req.picture.overlay_moving_bar = false;
  req.profile_version = 1;
  return req;
}


static int test_provider_callback_ingress_null_core_thread_drop_accounting() {
  ProviderCallbackIngress ingress(
      nullptr,
      [](ProviderToCoreCommand&&) {
        std::cerr << "Null-core ProviderCallbackIngress unexpectedly invoked sink\n";
      },
      []() -> uint64_t { return 0; },
      [](uint64_t) { return false; });

  ingress.on_device_error(kDeviceInstanceId, ProviderError::ERR_PROVIDER_FAILED);
  auto stats_after_non_frame = ingress.stats_copy();
  if (stats_after_non_frame.commands_dropped_closed != 1 ||
      stats_after_non_frame.non_frame_rejected_closed != 1) {
    std::cerr << "Expected null-core non-frame command to be accounted as Closed. closed="
              << stats_after_non_frame.commands_dropped_closed
              << " non_frame_closed=" << stats_after_non_frame.non_frame_rejected_closed << "\n";
    return 1;
  }

  std::atomic<uint64_t> release_calls{0};
  uint8_t pixel[4] = {0, 0, 0, 0};
  FrameView frame{};
  frame.device_instance_id = kDeviceInstanceId;
  frame.stream_id = kStreamId;
  frame.acquisition_session_id = 77;
  frame.width = 1;
  frame.height = 1;
  frame.format_fourcc = FOURCC_RGBA;
  frame.data = pixel;
  frame.size_bytes = sizeof(pixel);
  frame.stride_bytes = 4;
  frame.release = [](void* user, const FrameView*) {
    auto* count = static_cast<std::atomic<uint64_t>*>(user);
    count->fetch_add(1, std::memory_order_relaxed);
  };
  frame.release_user = &release_calls;

  ingress.on_frame(frame);
  const auto stats_after_frame = ingress.stats_copy();
  if (stats_after_frame.commands_dropped_closed != 2 ||
      stats_after_frame.frames_dropped_closed != 1 ||
      stats_after_frame.frames_released_on_drop_closed != 1 ||
      stats_after_frame.non_frame_rejected_closed != 1 ||
      release_calls.load(std::memory_order_relaxed) != 1 ||
      ingress.ingress_depth_for_stream(kStreamId) != 0) {
    std::cerr << "Expected null-core frame command to drop as Closed and release exactly once. closed="
              << stats_after_frame.commands_dropped_closed
              << " frame_closed=" << stats_after_frame.frames_dropped_closed
              << " frame_release_closed=" << stats_after_frame.frames_released_on_drop_closed
              << " non_frame_closed=" << stats_after_frame.non_frame_rejected_closed
              << " release_calls=" << release_calls.load(std::memory_order_relaxed)
              << " ingress_depth=" << ingress.ingress_depth_for_stream(kStreamId) << "\n";
    return 1;
  }

  return 0;
}

static int test_provider_callback_ingress_null_sink_frame_release_balances_telemetry() {
  struct TelemetryClearGuard {
    TelemetryClearGuard() { global_resource_aggregate_telemetry().clear(); }
    ~TelemetryClearGuard() { global_resource_aggregate_telemetry().clear(); }
  } telemetry_clear_guard;

  struct NoopHooks final : CoreThread::IHooks {} hooks;
  CoreThread core;
  if (!core.start(&hooks)) {
    std::cerr << "Failed to start CoreThread for null-sink ProviderCallbackIngress check\n";
    return 1;
  }

  ProviderCallbackIngress ingress(
      &core,
      std::function<void(ProviderToCoreCommand&&)>(),
      []() -> uint64_t { return 0; },
      [](uint64_t) { return false; });

  std::atomic<uint64_t> release_calls{0};
  uint8_t pixel[4] = {0, 0, 0, 0};
  constexpr uint64_t kNullSinkStreamId = 424242;
  FrameView frame{};
  frame.device_instance_id = kDeviceInstanceId;
  frame.stream_id = kNullSinkStreamId;
  frame.acquisition_session_id = 78;
  frame.width = 1;
  frame.height = 1;
  frame.format_fourcc = FOURCC_RGBA;
  frame.data = pixel;
  frame.size_bytes = sizeof(pixel);
  frame.stride_bytes = 4;
  frame.release = [](void* user, const FrameView*) {
    auto* count = static_cast<std::atomic<uint64_t>*>(user);
    count->fetch_add(1, std::memory_order_relaxed);
  };
  frame.release_user = &release_calls;

  ingress.on_frame(frame);

  auto barrier = std::make_shared<std::promise<void>>();
  auto barrier_done = barrier->get_future();
  const auto barrier_post = core.try_post([barrier]() mutable { barrier->set_value(); });
  if (barrier_post != CoreThread::PostResult::Enqueued ||
      barrier_done.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
    core.stop();
    std::cerr << "Failed to drain null-sink ProviderCallbackIngress frame task\n";
    return 1;
  }

  core.stop();

  const auto telemetry = global_resource_aggregate_telemetry().snapshot();
  const auto telemetry_it = std::find_if(telemetry.begin(), telemetry.end(), [](const ScopedResourceTelemetryKey& key) {
    return key.telemetry_scope == TelemetryScope::STREAM && key.stream_id == kNullSinkStreamId;
  });
  if (release_calls.load(std::memory_order_relaxed) != 1 ||
      ingress.ingress_depth_for_stream(kNullSinkStreamId) != 0 ||
      telemetry_it == telemetry.end() ||
      telemetry_it->framebuffer_lease_current != 0 ||
      telemetry_it->framebuffer_lease_total_created != 1 ||
      telemetry_it->framebuffer_lease_total_released != 1) {
    std::cerr << "Expected null-sink frame fallback to release once, clear ingress depth, and balance telemetry."
              << " release_calls=" << release_calls.load(std::memory_order_relaxed)
              << " ingress_depth=" << ingress.ingress_depth_for_stream(kNullSinkStreamId);
    if (telemetry_it == telemetry.end()) {
      std::cerr << " telemetry=missing\n";
    } else {
      std::cerr << " telemetry_current=" << telemetry_it->framebuffer_lease_current
                << " telemetry_created=" << telemetry_it->framebuffer_lease_total_created
                << " telemetry_released=" << telemetry_it->framebuffer_lease_total_released << "\n";
    }
    return 1;
  }

  return 0;
}


static int test_provider_open_close_refusal_visibility() {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  RefusingDeviceProvider provider;
  rt.set_snapshot_publisher(&buf);

  if (!rt.start()) {
    std::cerr << "FAIL: failed to start runtime for provider refusal smoke\n";
    return 1;
  }
  if (!wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "FAIL: runtime did not become LIVE for provider refusal smoke\n";
    rt.stop();
    return 1;
  }
  if (!provider.initialize(rt.provider_callbacks()).ok()) {
    std::cerr << "FAIL: refusing provider initialize failed\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&provider);

  if (!wait_for_snapshot_gen(buf, 0)) {
    std::cerr << "FAIL: baseline snapshot not published before provider refusal smoke\n";
    rt.stop();
    return 1;
  }

  const TryOpenDeviceStatus open_status = rt.try_open_device("refusing:0", kDeviceInstanceId, kRootId);
  if (open_status != TryOpenDeviceStatus::ProviderRejected) {
    std::cerr << "FAIL: provider-refused open returned status=" << static_cast<int>(open_status)
              << " expected ProviderRejected\n";
    rt.stop();
    return 1;
  }
  if (provider.open_called() != 1) {
    std::cerr << "FAIL: provider open call count=" << provider.open_called() << " expected 1\n";
    rt.stop();
    return 1;
  }

  const auto retain_spec = rt.retain_imaging_spec_version(7001);
  if (retain_spec != CoreThread::PostResult::Enqueued) {
    std::cerr << "FAIL: retain_imaging_spec_version admission failed after refused open; result="
              << static_cast<int>(retain_spec) << "\n";
    rt.stop();
    return 1;
  }
  if (!wait_for_snapshot_pred(buf, [](const CamBANGStateSnapshot& s) {
        return s.version >= 1 && s.devices.empty();
      })) {
    auto snap = buf.snapshot_copy();
    std::cerr << "FAIL: provider-refused open left stale device truth; device_count="
              << (snap ? snap->devices.size() : 0) << "\n";
    rt.stop();
    return 1;
  }

  const TryCloseDeviceStatus close_status = rt.try_close_device(kDeviceInstanceId);
  if (close_status != TryCloseDeviceStatus::ProviderRejected) {
    std::cerr << "FAIL: provider-refused close returned status=" << static_cast<int>(close_status)
              << " expected ProviderRejected\n";
    rt.stop();
    return 1;
  }
  if (provider.close_called() != 1) {
    std::cerr << "FAIL: provider close call count=" << provider.close_called() << " expected 1\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  (void)provider.shutdown();
  return 0;
}


static int test_stream_registry_non_ok_stop_ack_does_not_clobber_restart() {
  CoreStreamRegistry streams;
  StreamRequest req = make_req();
  if (!streams.declare_stream_effective(req) || !streams.on_stream_created(req.stream_id)) {
    std::cerr << "Stream registry non-OK stop ack setup failed\n";
    return 1;
  }

  if (!streams.on_core_stream_started(req.stream_id)) {
    std::cerr << "Core start setup failed for non-OK stop ack smoke\n";
    return 1;
  }
  if (!streams.on_core_stream_stopped(req.stream_id, 0)) {
    std::cerr << "Core stop setup failed for non-OK stop ack smoke\n";
    return 1;
  }
  if (!streams.on_core_stream_started(req.stream_id)) {
    std::cerr << "Core restart setup failed for non-OK stop ack smoke\n";
    return 1;
  }

  const uint32_t delayed_error = static_cast<uint32_t>(ProviderError::ERR_BAD_STATE);
  if (!streams.on_provider_stream_stopped(req.stream_id, delayed_error)) {
    std::cerr << "Provider non-OK delayed stop ack was not accepted\n";
    return 1;
  }

  const CoreStreamRegistry::StreamRecord* rec = streams.find(req.stream_id);
  if (!rec || !rec->started) {
    std::cerr << "Delayed non-OK stop ack must not clobber newer core restart\n";
    return 1;
  }
  if (rec->pending_core_stop_facts != 0) {
    std::cerr << "Delayed non-OK stop ack must consume pending core stop fact\n";
    return 1;
  }
  if (rec->last_error_code != delayed_error) {
    std::cerr << "Delayed non-OK stop ack must preserve provider error visibility\n";
    return 1;
  }

  if (!streams.on_core_stream_stopped(req.stream_id, 0)) {
    std::cerr << "Subsequent core stop failed after delayed non-OK ack\n";
    return 1;
  }
  rec = streams.find(req.stream_id);
  if (!rec || rec->started || rec->pending_core_stop_facts != 1) {
    std::cerr << "Subsequent core stop did not produce expected stopped pending state\n";
    return 1;
  }
  if (!streams.on_provider_stream_stopped(req.stream_id, 0)) {
    std::cerr << "Subsequent OK provider stop ack failed\n";
    return 1;
  }
  rec = streams.find(req.stream_id);
  if (!rec || rec->started || rec->pending_core_stop_facts != 0 || rec->last_error_code != 0) {
    std::cerr << "Subsequent OK provider stop ack did not clear pending stopped state\n";
    return 1;
  }

  return 0;
}

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
static bool wait_for_core_barrier(CoreRuntime& rt, std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
  auto barrier = std::make_shared<std::promise<void>>();
  auto done = barrier->get_future();
  const auto post_result = rt.try_post([barrier]() mutable { barrier->set_value(); });
  if (post_result != CoreThread::PostResult::Enqueued) {
    return false;
  }
  return done.wait_for(timeout) == std::future_status::ready;
}

static bool converge_stub_provider_core(CoreRuntime& rt, StubProvider& prov) {
  prov.flush_callbacks_for_smoke();
  return wait_for_core_barrier(rt);
}

static bool wait_for_created_stream_with_effective_profile(
    CoreRuntime& rt,
    StubProvider& prov,
    uint64_t stream_id,
    CoreStreamRegistry::StreamRecord& out) {
  return wait_until([&]() {
    prov.flush_callbacks_for_smoke();
    if (!wait_for_core_barrier(rt, std::chrono::milliseconds(50))) {
      return false;
    }

    CoreStreamRegistry::StreamRecord rec{};
    if (!get_stream_record(rt, stream_id, rec)) {
      return false;
    }

    out = rec;
    return rec.created &&
           rec.profile.width != 0 &&
           rec.profile.height != 0 &&
           rec.profile.format_fourcc != 0;
  }, 200, 1);
}

static const char* try_start_stream_status_name(TryStartStreamStatus status) {
  switch (status) {
    case TryStartStreamStatus::OK: return "OK";
    case TryStartStreamStatus::Busy: return "Busy";
    case TryStartStreamStatus::InvalidArgument: return "InvalidArgument";
    case TryStartStreamStatus::ProviderRejected: return "ProviderRejected";
  }
  return "Unknown";
}

static bool setup_one_runtime_created_stream(CoreRuntime& rt, StubProvider& prov);

static bool setup_one_stream(CoreRuntime& rt, StubProvider& prov) {
  if (!setup_one_runtime_created_stream(rt, prov)) {
    return false;
  }

  CaptureRequest capture_req{};
  if (!wait_until([&]() { return rt.materialize_capture_request(kDeviceInstanceId, capture_req); }, 200, 1)) {
    std::cerr << "Stub provider setup did not converge to a materialized capture request\n";
    return false;
  }

  return true;
}

static bool setup_one_runtime_created_stream(CoreRuntime& rt, StubProvider& prov) {
  if (!wait_until([&]() {
        return rt.state_copy() == CoreRuntimeState::LIVE;
      }, 200, 1)) {
    std::cerr << "CoreRuntime did not become LIVE before runtime lifecycle setup\n";
    return false;
  }

  if (!prov.initialize(rt.provider_callbacks()).ok()) {
    std::cerr << "Stub provider initialize failed for runtime lifecycle setup\n";
    return false;
  }
  rt.attach_provider(&prov);

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    std::cerr << "Stub provider endpoint enumeration failed for runtime lifecycle setup\n";
    return false;
  }

  const auto retain_identity = rt.retain_device_identity(kDeviceInstanceId, eps[0].hardware_id);
  if (retain_identity != CoreThread::PostResult::Enqueued) {
    std::cerr << "FAIL: retain_device_identity admission failed in setup_one_runtime_created_stream; result="
              << static_cast<int>(retain_identity) << "\n";
    return false;
  }

  const TryOpenDeviceStatus open_status = rt.try_open_device(eps[0].hardware_id, kDeviceInstanceId, kRootId);
  if (open_status != TryOpenDeviceStatus::OK) {
    std::cerr << "try_open_device failed for runtime lifecycle setup; status="
              << static_cast<int>(open_status) << "\n";
    return false;
  }
  if (!converge_stub_provider_core(rt, prov)) {
    std::cerr << "Timed out converging runtime lifecycle open-device facts\n";
    return false;
  }

  const StreamRequest req = make_req();
  const TryCreateStreamStatus create_status = rt.try_create_stream(
      kStreamId,
      kDeviceInstanceId,
      req.intent,
      &req.profile,
      &req.picture,
      req.profile_version);
  if (create_status != TryCreateStreamStatus::OK) {
    std::cerr << "try_create_stream failed for runtime lifecycle setup; status="
              << static_cast<int>(create_status) << "\n";
    return false;
  }
  CoreStreamRegistry::StreamRecord rec{};
  if (!wait_for_created_stream_with_effective_profile(rt, prov, kStreamId, rec)) {
    std::cerr << "Runtime lifecycle setup did not converge to a created stream with effective profile. created="
              << rec.created << " width=" << rec.profile.width
              << " height=" << rec.profile.height
              << " format=" << rec.profile.format_fourcc << "\n";
    return false;
  }

  return true;
}

static int test_strict_destroy_rejects_started_stream_smoke() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start for strict destroy smoke\n";
    return 1;
  }

  StubProvider prov;
  if (!setup_one_runtime_created_stream(rt, prov)) {
    rt.stop();
    return 1;
  }

  const TryStartStreamStatus start_status = rt.try_start_stream(kStreamId);
  if (start_status != TryStartStreamStatus::OK) {
    std::cerr << "Strict destroy smoke failed to start stream; status="
              << try_start_stream_status_name(start_status)
              << "(" << static_cast<int>(start_status) << ")\n";
    rt.stop();
    return 1;
  }

  const auto destroy_started = rt.try_destroy_stream(kStreamId);
  if (destroy_started != TryDestroyStreamStatus::Started) {
    std::cerr << "Expected destroy of started stream to return Started, got "
              << static_cast<int>(destroy_started) << "\n";
    rt.stop();
    return 1;
  }

  CoreStreamRegistry::StreamRecord rec{};
  if (!get_stream_record(rt, kStreamId, rec) || !rec.created || !rec.started) {
    std::cerr << "Destroy-while-started should leave stream created and started\n";
    rt.stop();
    return 1;
  }

  if (rt.try_stop_stream(kStreamId) != TryStopStreamStatus::OK) {
    std::cerr << "Strict destroy smoke failed to stop stream after rejected destroy\n";
    rt.stop();
    return 1;
  }

  if (rt.try_destroy_stream(kStreamId) != TryDestroyStreamStatus::OK) {
    std::cerr << "Destroy after explicit stop should succeed\n";
    rt.stop();
    return 1;
  }

  if (!converge_stub_provider_core(rt, prov)) {
    std::cerr << "Timed out converging destroy-after-stop fact\n";
    rt.stop();
    return 1;
  }
  if (get_stream_record(rt, kStreamId, rec)) {
    std::cerr << "Stream should be absent after successful destroy\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}


static int test_stream_start_stop_idempotency_survives_delayed_provider_facts_smoke() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start for stream idempotency smoke\n";
    return 1;
  }

  StubProvider prov;
  if (!setup_one_runtime_created_stream(rt, prov)) {
    rt.stop();
    return 1;
  }

  if (rt.try_start_stream(kStreamId) != TryStartStreamStatus::OK) {
    std::cerr << "First stream start should return OK\n";
    rt.stop();
    return 1;
  }
  if (rt.try_start_stream(kStreamId) != TryStartStreamStatus::OK) {
    std::cerr << "Second immediate stream start should be idempotent OK\n";
    rt.stop();
    return 1;
  }
  if (rt.try_stop_stream(kStreamId) != TryStopStreamStatus::OK) {
    std::cerr << "First stream stop should return OK\n";
    rt.stop();
    return 1;
  }

  if (!converge_stub_provider_core(rt, prov)) {
    std::cerr << "Timed out converging delayed provider stream lifecycle facts\n";
    rt.stop();
    return 1;
  }

  CoreStreamRegistry::StreamRecord rec{};
  if (!get_stream_record(rt, kStreamId, rec) || !rec.created || rec.started) {
    std::cerr << "Delayed provider lifecycle facts must not resurrect a core-stopped stream\n";
    rt.stop();
    return 1;
  }

  if (rt.try_stop_stream(kStreamId) != TryStopStreamStatus::OK) {
    std::cerr << "Second stream stop after delayed provider facts should remain idempotent OK\n";
    rt.stop();
    return 1;
  }
  if (rt.try_destroy_stream(kStreamId) != TryDestroyStreamStatus::OK) {
    std::cerr << "Destroy after idempotent stops should return OK\n";
    rt.stop();
    return 1;
  }
  if (!converge_stub_provider_core(rt, prov)) {
    std::cerr << "Timed out converging idempotency smoke destroy fact\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_destroy_never_started_stream_smoke() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start for never-started destroy smoke\n";
    return 1;
  }

  StubProvider prov;
  if (!setup_one_runtime_created_stream(rt, prov)) {
    rt.stop();
    return 1;
  }

  if (rt.try_destroy_stream(kStreamId) != TryDestroyStreamStatus::OK) {
    std::cerr << "Destroy of created but never-started stream should succeed\n";
    rt.stop();
    return 1;
  }

  if (!converge_stub_provider_core(rt, prov)) {
    std::cerr << "Timed out converging never-started destroy fact\n";
    rt.stop();
    return 1;
  }

  CoreStreamRegistry::StreamRecord rec{};
  if (get_stream_record(rt, kStreamId, rec)) {
    std::cerr << "Never-started stream should be absent after successful destroy\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static const char* rig_preflight_failure_name(CoreRuntime::RigPreflightFailure failure) {
  switch (failure) {
    case CoreRuntime::RigPreflightFailure::None:
      return "None";
    case CoreRuntime::RigPreflightFailure::RigNotFound:
      return "RigNotFound";
    case CoreRuntime::RigPreflightFailure::EmptyMembership:
      return "EmptyMembership";
    case CoreRuntime::RigPreflightFailure::HardwareIdUnresolved:
      return "HardwareIdUnresolved";
    case CoreRuntime::RigPreflightFailure::HardwareIdAmbiguous:
      return "HardwareIdAmbiguous";
    case CoreRuntime::RigPreflightFailure::DuplicateResolvedDevice:
      return "DuplicateResolvedDevice";
    case CoreRuntime::RigPreflightFailure::MaterializeFailed:
      return "MaterializeFailed";
  }
  return "Unknown";
}

static void print_rig_preflight_result(
    const char* prefix,
    const CoreRuntime::RigPreflightResult& res) {
  std::cerr << prefix
            << " ok=" << (res.ok ? "true" : "false")
            << " failure=" << rig_preflight_failure_name(res.failure)
            << "(" << static_cast<int>(res.failure) << ")"
            << " rig_id=" << res.rig_id
            << " failure_member_index=" << res.failure_member_index
            << " failure_hardware_id=" << res.failure_hardware_id
            << " failure_device_instance_id=" << res.failure_device_instance_id
            << " participants=" << res.participants.size() << "\n";
}

static CoreRuntime::RigPreflightResult wait_for_rig_preflight_ok(
    CoreRuntime& rt,
    uint64_t rig_id,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
  CoreRuntime::RigPreflightResult last{};
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    last = rt.preflight_rig_participants_materialize(rig_id);
    if (last.ok) {
      return last;
    }

    auto barrier = std::make_shared<std::promise<void>>();
    auto barrier_done = barrier->get_future();
    const auto post_result = rt.try_post([barrier]() mutable { barrier->set_value(); });
    if (post_result != CoreThread::PostResult::Enqueued) {
      return last;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }
    const auto remaining = deadline - now;
    (void)barrier_done.wait_for(remaining);
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);

  last = rt.preflight_rig_participants_materialize(rig_id);
  return last;
}
#endif

// ---- Tests (existing behaviour preserved) ----

static int test_publish_gating_before_start() {
  CoreRuntime pre;
  pre.request_publish();
  const auto st = pre.stats_copy();
  if (st.publish_requests_dropped_closed != 1) {
    std::cerr << "Expected publish drop before start (closed). got="
              << st.publish_requests_dropped_closed << "\n";
    return 1;
  }
  return 0;
}

static int test_display_demand_async_release_closed_accounting_before_start() {
  CoreRuntime pre;

  // stream_id==0 remains a no-op and must not be accounted as a failed release.
  pre.release_stream_display_demand_async(0);
  auto st = pre.stats_copy();
  if (st.display_demand_release_async_dropped_closed != 0 ||
      st.display_demand_release_async_dropped_allocfail != 0 ||
      st.display_demand_release_async_dropped_full != 0) {
    std::cerr << "Unexpected display-demand release accounting for stream_id=0. closed="
              << st.display_demand_release_async_dropped_closed
              << " alloc=" << st.display_demand_release_async_dropped_allocfail
              << " full=" << st.display_demand_release_async_dropped_full << "\n";
    return 1;
  }

  pre.release_stream_display_demand_async(20);
  st = pre.stats_copy();
  if (st.display_demand_release_async_dropped_closed != 1 ||
      st.display_demand_release_async_dropped_allocfail != 0 ||
      st.display_demand_release_async_dropped_full != 0) {
    std::cerr << "Expected display-demand async release Closed accounting before start. closed="
              << st.display_demand_release_async_dropped_closed
              << " alloc=" << st.display_demand_release_async_dropped_allocfail
              << " full=" << st.display_demand_release_async_dropped_full << "\n";
    return 1;
  }
  return 0;
}

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
static int test_baseline_live_one_frame_and_snapshot(CoreRuntime& rt,
                                                     StateSnapshotBuffer& buf,
                                                     StubProvider& prov) {
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start\n";
    return 1;
  }

  // CoreRuntime now starts "dirty" and publishes an initial snapshot automatically.
  // The first snapshot is zero-indexed (gen=0).
  if (!wait_for_snapshot_gen(buf, 0)) {
    std::cerr << "Timeout waiting for initial snapshot publication\n";
    rt.stop();
    return 1;
  }

  if (!rt.start() || rt.state_copy() == CoreRuntimeState::STOPPED) {
    std::cerr << "Idempotent start failed\n";
    rt.stop();
    return 1;
  }

  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed\n";
    rt.stop();
    return 1;
  }

  rt.attach_provider(&prov);

  const StreamRequest req = make_req();
  if (!prov.start_stream(kStreamId, req.profile, req.picture).ok()) {
    std::cerr << "start_stream failed\n";
    rt.stop();
    return 1;
  }

  if (!wait_until([&]() { return prov.frames_released() >= 1; })) {
    std::cerr << "Timeout waiting for frame release\n";
    rt.stop();
    return 1;
  }

  const CoreDispatchStats ds0 = get_dispatch_stats(rt);
  if (prov.frames_emitted() != 1 || prov.frames_released() != 1) {
    std::cerr << "Provider counters mismatch. emitted=" << prov.frames_emitted()
              << " released=" << prov.frames_released() << "\n";
    rt.stop();
    return 1;
  }
  if (ds0.frames_received != 1 || ds0.frames_released != 1) {
    std::cerr << "Dispatcher counters mismatch. received=" << ds0.frames_received
              << " released=" << ds0.frames_released << "\n";
    rt.stop();
    return 1;
  }

  CoreStreamRegistry::StreamRecord rec{};
  if (!get_stream_record(rt, kStreamId, rec)) {
    std::cerr << "Stream registry missing stream_id=" << kStreamId << "\n";
    rt.stop();
    return 1;
  }
  if (!rec.created || !rec.started || rec.frames_received != 1) {
    std::cerr << "Stream registry mismatch. created=" << rec.created
              << " started=" << rec.started
              << " frames_received=" << rec.frames_received
              << " frames_released=" << rec.frames_released
              << " frames_dropped=" << rec.frames_dropped << "\n";
    rt.stop();
    return 1;
  }

  // Frame accounting boundary:
  // - delivered requires sink handoff (frames_released)
  // - no-sink release-on-drop increments frames_dropped
  if ((rec.frames_released + rec.frames_dropped) != 1) {
    std::cerr << "Stream registry mismatch. created=" << rec.created
              << " started=" << rec.started
              << " frames_received=" << rec.frames_received
              << " frames_released=" << rec.frames_released
              << " frames_dropped=" << rec.frames_dropped << "\n";
    rt.stop();
    return 1;
  }

  // We need a snapshot that reflects the flowing stream and frame counters.
  // This may be published naturally (dirty-driven) when the frame fact is integrated,
  // or it may require an explicit smoke-only request_publish() to force a publish.
  rt.request_publish();
  if (!wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
        for (const auto& st : s.streams) {
          if (st.stream_id == kStreamId) {
            return (st.mode == CBStreamMode::FLOWING && st.frames_received >= 1 &&
                    (st.frames_delivered + st.frames_dropped) >= 1);
          }
        }
        return false;
      })) {
    std::cerr << "Timeout waiting for snapshot containing flowing stream counters\n";
    rt.stop();
    return 1;
  }

  auto snap0 = get_last_snapshot(buf);
  if (!snap0 || snap0->schema_version != CamBANGStateSnapshot::kSchemaVersion) {
    std::cerr << "Snapshot missing or wrong schema_version\n";
    rt.stop();
    return 1;
  }

  bool found = false;
  for (const auto& s : snap0->streams) {
    if (s.stream_id == kStreamId) {
      found = true;
      if (s.mode != CBStreamMode::FLOWING || s.frames_received != 1 ||
          (s.frames_delivered + s.frames_dropped) != 1) {
        std::cerr << "Snapshot stream mismatch. mode=" << static_cast<int>(s.mode)
                  << " frames_received=" << s.frames_received
                  << " frames_delivered=" << s.frames_delivered
                  << " frames_dropped=" << s.frames_dropped << "\n";
        rt.stop();
        return 1;
      }
      break;
    }
  }
  if (!found) {
    std::cerr << "Snapshot missing stream_id=" << kStreamId << "\n";
    rt.stop();
    return 1;
  }

  return 0;
}

#endif

static int test_baseline_publish_without_provider(CoreRuntime& rt, StateSnapshotBuffer& buf) {
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start\n";
    return 1;
  }

  // CoreRuntime starts "dirty" and publishes an initial snapshot automatically.
  // The first snapshot is zero-indexed (gen=0).
  if (!wait_for_snapshot_gen(buf, 0)) {
    std::cerr << "Timeout waiting for initial snapshot publication\n";
    rt.stop();
    return 1;
  }
  auto snap = get_last_snapshot(buf);
  if (!snap || snap->schema_version != CamBANGStateSnapshot::kSchemaVersion) {
    std::cerr << "Snapshot missing or wrong schema_version\n";
    rt.stop();
    return 1;
  }

  // Still validate request_publish() works once the core is live.
  // (Smoke-only; production should rely on dirty-driven publication.)
  rt.request_publish();
  if (!wait_for_snapshot_version(buf, /*gen=*/0, /*min_version=*/1)) {
    std::cerr << "Timeout waiting for request_publish() snapshot\n";
    rt.stop();
    return 1;
  }
  rt.stop();
  return 0;
}

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
static int test_overload_queuefull_release_accounting(CoreRuntime& rt, StubProvider& prov) {
  (void)prov;
  if (!wait_for_core_barrier(rt)) {
    std::cerr << "Failed to establish pre-overload core barrier\n";
    rt.stop();
    return 1;
  }

  auto release_gate = std::make_shared<std::promise<void>>();
  std::shared_future<void> release_gate_done(release_gate->get_future());
  std::atomic<bool> gate_started{false};
  const auto gate_post = rt.try_post_core_thread_unchecked([release_gate_done, &gate_started]() mutable {
    gate_started.store(true, std::memory_order_release);
    release_gate_done.wait();
  });
  if (gate_post != CoreThread::PostResult::Enqueued) {
    std::cerr << "Failed to post deterministic overload gate\n";
    rt.stop();
    return 1;
  }
  while (!gate_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  size_t fillers_posted = 0;
  for (; fillers_posted < CoreThread::kMaxPendingTasks; ++fillers_posted) {
    const auto filler_post = rt.try_post_core_thread_unchecked([]() {});
    if (filler_post != CoreThread::PostResult::Enqueued) {
      release_gate->set_value();
      std::cerr << "Failed to fill deterministic overload queue. fillers_posted=" << fillers_posted
                << " post_result=" << static_cast<int>(filler_post) << "\n";
      rt.stop();
      return 1;
    }
  }

  const auto ingress_before = rt.ingress_stats_copy();
  std::atomic<uint64_t> released_on_drop{0};
  uint8_t pixel[4] = {0, 0, 0, 0};
  FrameView frame{};
  frame.device_instance_id = kDeviceInstanceId;
  frame.stream_id = kStreamId;
  frame.width = 1;
  frame.height = 1;
  frame.format_fourcc = FOURCC_RGBA;
  frame.data = pixel;
  frame.size_bytes = sizeof(pixel);
  frame.stride_bytes = 4;
  frame.release = [](void* user, const FrameView*) {
    auto* count = static_cast<std::atomic<uint64_t>*>(user);
    count->fetch_add(1, std::memory_order_relaxed);
  };
  frame.release_user = &released_on_drop;

  rt.provider_callbacks()->on_frame(frame);
  const auto ingress_after_drop = rt.ingress_stats_copy();
  const uint64_t full_drops = ingress_after_drop.frames_dropped_full - ingress_before.frames_dropped_full;
  const uint64_t full_releases = ingress_after_drop.frames_released_on_drop_full -
                                 ingress_before.frames_released_on_drop_full;
  if (full_drops != 1 || full_releases != 1 || released_on_drop.load(std::memory_order_relaxed) != 1) {
    release_gate->set_value();
    std::cerr << "Expected deterministic ProviderCallbackIngress QueueFull frame drop/release. drops="
              << full_drops << " releases=" << full_releases
              << " release_hook_calls=" << released_on_drop.load(std::memory_order_relaxed) << "\n";
    rt.stop();
    return 1;
  }

  release_gate->set_value();
  if (!wait_until([&]() { return wait_for_core_barrier(rt); }, 200, 1)) {
    std::cerr << "Failed to drain deterministic overload queue after releasing gate\n";
    rt.stop();
    return 1;
  }

  const auto ingress_after_drain = rt.ingress_stats_copy();
  if (ingress_after_drain.frames_dropped_full - ingress_before.frames_dropped_full != 1 ||
      ingress_after_drain.frames_released_on_drop_full - ingress_before.frames_released_on_drop_full != 1) {
    std::cerr << "Deterministic QueueFull accounting changed after drain\n";
    rt.stop();
    return 1;
  }

  return 0;
}

static int test_non_frame_provider_fact_survives_ordinary_queue_full(CoreRuntime& rt, StubProvider& prov) {
  (void)prov;
  if (!wait_for_core_barrier(rt)) {
    std::cerr << "Failed to establish pre-essential-overload core barrier\n";
    rt.stop();
    return 1;
  }

  const CoreDispatchStats before = get_dispatch_stats(rt);

  auto release_gate = std::make_shared<std::promise<void>>();
  std::shared_future<void> release_gate_done(release_gate->get_future());
  std::atomic<bool> gate_started{false};
  const auto gate_post = rt.try_post_core_thread_unchecked([release_gate_done, &gate_started]() mutable {
    gate_started.store(true, std::memory_order_release);
    release_gate_done.wait();
  });
  if (gate_post != CoreThread::PostResult::Enqueued) {
    std::cerr << "Failed to post essential-overload gate\n";
    rt.stop();
    return 1;
  }
  while (!gate_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  size_t fillers_posted = 0;
  for (; fillers_posted < CoreThread::kMaxPendingTasks; ++fillers_posted) {
    const auto filler_post = rt.try_post_core_thread_unchecked([]() {});
    if (filler_post != CoreThread::PostResult::Enqueued) {
      release_gate->set_value();
      std::cerr << "Failed to fill ordinary queue for essential-overload check. fillers_posted="
                << fillers_posted << " post_result=" << static_cast<int>(filler_post) << "\n";
      rt.stop();
      return 1;
    }
  }

  const auto ingress_before = rt.ingress_stats_copy();
  rt.provider_callbacks()->on_device_error(kDeviceInstanceId, ProviderError::ERR_PROVIDER_FAILED);
  const auto ingress_after_post = rt.ingress_stats_copy();

  if (ingress_after_post.commands_dropped_full != ingress_before.commands_dropped_full ||
      ingress_after_post.commands_dropped_closed != ingress_before.commands_dropped_closed ||
      ingress_after_post.commands_dropped_allocfail != ingress_before.commands_dropped_allocfail ||
      ingress_after_post.non_frame_rejected_closed != ingress_before.non_frame_rejected_closed ||
      ingress_after_post.non_frame_rejected_allocfail != ingress_before.non_frame_rejected_allocfail) {
    release_gate->set_value();
    std::cerr << "Non-frame provider fact was rejected while ordinary queue was full. full_delta="
              << (ingress_after_post.commands_dropped_full - ingress_before.commands_dropped_full)
              << " closed_delta="
              << (ingress_after_post.commands_dropped_closed - ingress_before.commands_dropped_closed)
              << " alloc_delta="
              << (ingress_after_post.commands_dropped_allocfail - ingress_before.commands_dropped_allocfail)
              << " non_frame_closed_delta="
              << (ingress_after_post.non_frame_rejected_closed - ingress_before.non_frame_rejected_closed)
              << " non_frame_alloc_delta="
              << (ingress_after_post.non_frame_rejected_allocfail - ingress_before.non_frame_rejected_allocfail)
              << "\n";
    rt.stop();
    return 1;
  }

  release_gate->set_value();

  const bool observed = wait_until([&]() {
    const CoreDispatchStats after = get_dispatch_stats(rt);
    return after.commands_handled >= before.commands_handled + 1;
  }, 1000, 2);
  if (!observed) {
    std::cerr << "Timed out waiting for non-frame provider fact dispatch after ordinary queue saturation\n";
    rt.stop();
    return 1;
  }

  return 0;
}

static int test_shutdown_choreography(CoreRuntime& rt, StubProvider& prov) {
  rt.stop();

  if (!prov.shutting_down()) {
    std::cerr << "Expected provider to be in shutting_down state after rt.stop()\n";
    return 1;
  }

  const auto diag = rt.shutdown_diag_copy();
  const uint8_t exit_code = CoreRuntime::shutdown_phase_exit_code();
  if (diag.phase_changes == 0 || diag.phase_code != exit_code) {
    std::cerr << "Shutdown phase diagnostics unexpected. phase_code="
              << static_cast<int>(diag.phase_code)
              << " changes=" << diag.phase_changes << "\n";
    return 1;
  }

  rt.stop(); // idempotent
  return 0;
}

static int test_rig_preflight_materialization_smoke() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (rig preflight smoke)\n";
    return 1;
  }

  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed (rig preflight smoke)\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);

  // Missing rig.
  {
    const auto res = rt.preflight_rig_participants_materialize(9001);
    if (res.ok || res.failure != CoreRuntime::RigPreflightFailure::RigNotFound) {
      std::cerr << "Expected RigNotFound for missing rig\n";
      rt.stop();
      return 1;
    }
  }

  // Empty membership.
  if (!rt.smoke_set_rig_member_hardware_ids(7001, {})) {
    std::cerr << "Failed to set rig members for empty-membership case\n";
    rt.stop();
    return 1;
  }
  {
    const auto res = rt.preflight_rig_participants_materialize(7001);
    if (res.ok || res.failure != CoreRuntime::RigPreflightFailure::EmptyMembership) {
      std::cerr << "Expected EmptyMembership for rig with no members\n";
      rt.stop();
      return 1;
    }
  }

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    std::cerr << "Failed to enumerate endpoints (rig preflight smoke)\n";
    rt.stop();
    return 1;
  }
  const std::string live_hw = eps[0].hardware_id;

  // Unresolved hardware id.
  if (!rt.smoke_set_rig_member_hardware_ids(7002, {"missing:hw"})) {
    std::cerr << "Failed to set unresolved hardware id case\n";
    rt.stop();
    return 1;
  }
  {
    const auto res = rt.preflight_rig_participants_materialize(7002);
    if (res.ok || res.failure != CoreRuntime::RigPreflightFailure::HardwareIdUnresolved) {
      std::cerr << "Expected HardwareIdUnresolved\n";
      rt.stop();
      return 1;
    }
  }

  // Success.
  if (!rt.smoke_set_rig_member_hardware_ids(7003, {live_hw})) {
    std::cerr << "Failed to set success case membership\n";
    rt.stop();
    return 1;
  }
  {
    const auto res = wait_for_rig_preflight_ok(rt, 7003);
    if (!res.ok || res.failure != CoreRuntime::RigPreflightFailure::None || res.participants.size() != 1) {
      print_rig_preflight_result("Expected successful single participant preflight", res);
      rt.stop();
      return 1;
    }
    if (res.participants[0].hardware_id != live_hw ||
        res.participants[0].device_instance_id != kDeviceInstanceId ||
        res.participants[0].request.device_instance_id != kDeviceInstanceId ||
        res.participants[0].request.rig_id != 0) {
      std::cerr << "Unexpected participant resolution/materialization output\n";
      rt.stop();
      return 1;
    }
    const auto& members = res.participants[0].request.still_image_bundle.members;
    if (members.size() != 1 ||
        members[0].image_member_index != 0 ||
        members[0].role != CaptureStillImageMemberRole::DEFAULT_METERED) {
        std::cerr << "Expected rig preflight materialized request with one default-metered image member\n";
        rt.stop();
        return 1;
    }
  }

  // Duplicate resolved participant (same hardware id listed twice).
  if (!rt.smoke_set_rig_member_hardware_ids(7004, {live_hw, live_hw})) {
    std::cerr << "Failed to set duplicate-resolution case\n";
    rt.stop();
    return 1;
  }
  {
    const auto res = rt.preflight_rig_participants_materialize(7004);
    if (res.ok || res.failure != CoreRuntime::RigPreflightFailure::DuplicateResolvedDevice) {
      std::cerr << "Expected DuplicateResolvedDevice\n";
      rt.stop();
      return 1;
    }
  }

  rt.stop();
  return 0;
}

static int test_device_capture_request_materialization_smoke() {
  CoreRuntime rt;
  const auto ingest_result = rt.ingest_camera_concurrency_json_for_server(
      make_adc_camera_concurrency_json({"synthetic:0", "synthetic:1"}, false));
  if (ingest_result.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok) {
    std::cerr << "Expected stopped-time camera concurrency ingestion for device materialization smoke\n";
    return 1;
  }
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (device materialization smoke)\n";
    return 1;
  }
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed (device materialization smoke)\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);

  CaptureRequest req{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req)) {
    std::cerr << "Expected materialized capture request for live device\n";
    rt.stop();
    return 1;
  }
  if (req.still_image_bundle.members.size() != 1 ||
      req.still_image_bundle.members[0].image_member_index != 0 ||
      req.still_image_bundle.members[0].role != CaptureStillImageMemberRole::DEFAULT_METERED) {
    std::cerr << "Expected device materialized request with one default-metered image member\n";
    rt.stop();
    return 1;
  }

  CaptureRequest retained_req{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, retained_req)) {
    std::cerr << "Camera concurrency truth should not reject single-device capture request materialization\n";
    rt.stop();
    return 1;
  }
  if (retained_req.still_image_bundle.members.size() != 1 ||
      retained_req.still_image_bundle.members[0].image_member_index != 0 ||
      retained_req.still_image_bundle.members[0].role != CaptureStillImageMemberRole::DEFAULT_METERED) {
    std::cerr << "Camera concurrency truth changed default single-device still_image_bundle materialization\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_capture_admission_context_smoke() {
  const auto same_context = [](const CaptureAdmissionContext& left,
                               const CaptureAdmissionContext& right) {
    if (left.capture_date_time.unix_epoch_nanoseconds() !=
        right.capture_date_time.unix_epoch_nanoseconds()) {
      return false;
    }
    if (left.geolocation.has_value() != right.geolocation.has_value()) {
      return false;
    }
    if (!left.geolocation) {
      return true;
    }
    return left.geolocation->latitude_degrees() == right.geolocation->latitude_degrees() &&
           left.geolocation->longitude_degrees() == right.geolocation->longitude_degrees() &&
           left.geolocation->altitude_meters() == right.geolocation->altitude_meters();
  };

  CoreRuntime rt;
  if (rt.smoke_replace_capture_geolocation(51.5, -0.12, 35.0) !=
          CoreRuntime::ReplaceCaptureGeolocationStatus::Ok ||
      rt.ingest_camera_concurrency_json_for_server(
          make_adc_camera_concurrency_json({"a", "b"}, true, {{"a", "b"}}, 2)).status !=
          CoreRuntime::IngestCameraConcurrencyStatus::Ok ||
      !rt.smoke_set_capture_datetime_utc_nanoseconds(100) || !rt.start()) {
    std::cerr << "FAIL: capture-admission context stopped-time setup failed\n";
    return 1;
  }
  StubProvider prov;
  rt.smoke_reset_capture_admission_clock_sample_count();
  if (!setup_one_stream(rt, prov) ||
      rt.try_trigger_device_capture_with_capture_id_for_server(kDeviceInstanceId, 99001) !=
          TryTriggerDeviceCaptureStatus::OK) {
    std::cerr << "FAIL: standalone capture admission failed\n";
    rt.stop();
    return 1;
  }
  const auto first = rt.smoke_capture_admission_context(99001, kDeviceInstanceId);
  if (!first || first->capture_date_time.unix_epoch_nanoseconds() != 100 ||
      !first->geolocation || first->geolocation->latitude_degrees() != 51.5 ||
      first->geolocation->longitude_degrees() != -0.12 ||
      !first->geolocation->altitude_meters() || *first->geolocation->altitude_meters() != 35.0 ||
      rt.smoke_capture_admission_clock_sample_count() != 1 ||
      rt.smoke_replace_capture_geolocation(1.0, 2.0, std::nullopt) !=
          CoreRuntime::ReplaceCaptureGeolocationStatus::Busy) {
    std::cerr << "FAIL: standalone admission context was not immutable or stopped-only\n";
    rt.stop();
    return 1;
  }
  rt.smoke_set_capture_datetime_utc_nanoseconds(200);
  if (rt.try_trigger_device_capture_with_capture_id_for_server(kDeviceInstanceId, 99002) !=
          TryTriggerDeviceCaptureStatus::OK) {
    std::cerr << "FAIL: later standalone capture admission failed\n";
    rt.stop();
    return 1;
  }
  const auto later = rt.smoke_capture_admission_context(99002, kDeviceInstanceId);
  if (!later || later->capture_date_time.unix_epoch_nanoseconds() != 200 ||
      first->capture_date_time.unix_epoch_nanoseconds() != 100 ||
      rt.smoke_capture_admission_clock_sample_count() != 2) {
    std::cerr << "FAIL: capture date-time was not independently sampled per admission\n";
    rt.stop();
    return 1;
  }
  rt.stop();
  if (!rt.start() ||
      !wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "FAIL: capture geolocation did not persist across restart\n";
    return 1;
  }
  const auto rejected_preflight = rt.smoke_admit_rig_cohort_from_preflight(
      991, 99000, CoreRuntime::RigPreflightResult{});
  if (rejected_preflight.ok || rt.smoke_capture_admission_context(99000, 1) ||
      rt.smoke_capture_admission_clock_sample_count() != 2) {
    std::cerr << "FAIL: rejected rig preflight created capture context\n";
    rt.stop();
    return 1;
  }
  const auto admitted = rt.smoke_admit_rig_cohort_from_preflight(
      991, 99003, make_manual_rig_preflight(991, {"a", "b"}));
  const auto first_rig = rt.smoke_capture_admission_context(99003, 1);
  const auto second_rig = rt.smoke_capture_admission_context(99003, 2);
  if (!admitted.ok || !first_rig || !second_rig ||
      first_rig->capture_date_time.unix_epoch_nanoseconds() != 200 ||
      second_rig->capture_date_time.unix_epoch_nanoseconds() != 200 ||
      !first_rig->geolocation || !second_rig->geolocation ||
      first_rig->geolocation->latitude_degrees() != second_rig->geolocation->latitude_degrees() ||
      admitted.participants.size() != 2 ||
      !admitted.participants[0].request.has_admission_context ||
      !admitted.participants[1].request.has_admission_context ||
      !same_context(*first_rig, *second_rig) ||
      !same_context(admitted.participants[0].request.admission_context, *first_rig) ||
      !same_context(admitted.participants[1].request.admission_context, *first_rig) ||
      rt.smoke_capture_admission_clock_sample_count() != 3) {
    std::cerr << "FAIL: grouped admission did not share persisted geolocation context"
              << " admitted=" << admitted.ok
              << " failure=" << static_cast<int>(admitted.failure)
              << " first=" << static_cast<bool>(first_rig)
              << " second=" << static_cast<bool>(second_rig) << '\n';
    rt.stop();
    return 1;
  }
  const auto duplicate = rt.smoke_admit_rig_cohort_from_preflight(
      991, 99003, make_manual_rig_preflight(991, {"a", "b"}));
  if (duplicate.ok ||
      duplicate.failure != CoreRuntime::RigCohortAdmissionFailure::DuplicateCaptureId ||
      rt.smoke_capture_admission_clock_sample_count() != 3) {
    std::cerr << "FAIL: duplicate rig capture ID sampled a capture context\n";
    rt.stop();
    return 1;
  }
  rt.stop();
  rt.smoke_set_capture_datetime_utc_nanoseconds(300);
  if (rt.smoke_clear_capture_geolocation() != CoreRuntime::ReplaceCaptureGeolocationStatus::Ok ||
      !rt.start() ||
      !wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "FAIL: capture geolocation clear failed while stopped\n";
    return 1;
  }
  const auto cleared = rt.smoke_admit_rig_cohort_from_preflight(
      991, 99004, make_manual_rig_preflight(991, {"a", "b"}));
  const auto cleared_context = rt.smoke_capture_admission_context(99004, 1);
  if (!cleared.ok || !cleared_context || cleared_context->geolocation ||
      cleared_context->capture_date_time.unix_epoch_nanoseconds() != 300) {
    std::cerr << "FAIL: cleared capture geolocation did not remain absent at admission\n";
    rt.stop();
    return 1;
  }
  rt.stop();

  CoreRuntime rejection_rt;
  if (!rejection_rt.smoke_set_capture_datetime_utc_nanoseconds(400) ||
      !rejection_rt.start() ||
      !wait_until([&]() { return rejection_rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "FAIL: capture-admission rejection runtime setup failed\n";
    return 1;
  }
  rejection_rt.smoke_reset_capture_admission_clock_sample_count();
  const auto unavailable = rejection_rt.smoke_admit_rig_cohort_from_preflight(
      992, 99005, make_manual_rig_preflight(992, {"a", "b"}));
  if (unavailable.ok ||
      unavailable.failure != CoreRuntime::RigCohortAdmissionFailure::ImagingSpecUnavailable ||
      rejection_rt.smoke_capture_admission_clock_sample_count() != 0) {
    std::cerr << "FAIL: unavailable ImagingSpec sampled a capture context\n";
    rejection_rt.stop();
    return 1;
  }
  rejection_rt.stop();
  if (rejection_rt.ingest_camera_concurrency_json_for_server(
          make_adc_camera_concurrency_json({"a", "b"}, false, {}, 2)).status !=
          CoreRuntime::IngestCameraConcurrencyStatus::Ok ||
      !rejection_rt.start() ||
      !wait_until([&]() { return rejection_rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "FAIL: rejected ImagingSpec setup failed\n";
    return 1;
  }
  rejection_rt.smoke_reset_capture_admission_clock_sample_count();
  const auto rejected = rejection_rt.smoke_admit_rig_cohort_from_preflight(
      992, 99006, make_manual_rig_preflight(992, {"a", "b"}));
  if (rejected.ok ||
      rejected.failure != CoreRuntime::RigCohortAdmissionFailure::ImagingSpecRejected ||
      rejection_rt.smoke_capture_admission_clock_sample_count() != 0) {
    std::cerr << "FAIL: rejected ImagingSpec sampled a capture context\n";
    rejection_rt.stop();
    return 1;
  }
  rejection_rt.stop();
  if (rejection_rt.ingest_camera_concurrency_json_for_server(
          make_adc_camera_concurrency_json({"a", "b"}, true, {{"a", "b"}}, 2)).status !=
          CoreRuntime::IngestCameraConcurrencyStatus::Ok ||
      !rejection_rt.start() ||
      !wait_until([&]() { return rejection_rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "FAIL: rig submission failure setup failed\n";
    return 1;
  }
  rejection_rt.smoke_reset_capture_admission_clock_sample_count();
  const auto admitted_without_provider = rejection_rt.smoke_admit_rig_cohort_from_preflight(
      992, 99007, make_manual_rig_preflight(992, {"a", "b"}));
  if (!admitted_without_provider.ok ||
      rejection_rt.smoke_capture_admission_clock_sample_count() != 1) {
    std::cerr << "FAIL: successful rig admission did not sample exactly one context\n";
    rejection_rt.stop();
    return 1;
  }
  const auto submission_failure =
      rejection_rt.smoke_submit_admitted_rig_bundle(admitted_without_provider);
  if (submission_failure.ok ||
      submission_failure.failure != CoreRuntime::RigSubmissionFailure::ProviderUnavailable ||
      rejection_rt.smoke_capture_admission_clock_sample_count() != 1) {
    std::cerr << "FAIL: provider submission failure re-sampled a capture context\n";
    rejection_rt.stop();
    return 1;
  }
  rejection_rt.stop();
  return 0;
}

static int test_open_device_snapshot_retains_default_capture_profile_smoke() {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (default capture profile snapshot smoke)\n";
    return 1;
  }
  if (!wait_for_snapshot_gen(buf, 0)) {
    std::cerr << "Timeout waiting for initial snapshot (default capture profile snapshot smoke)\n";
    rt.stop();
    return 1;
  }

  StubProvider prov;
  if (!prov.initialize(rt.provider_callbacks()).ok()) {
    std::cerr << "Stub provider initialize failed (default capture profile snapshot smoke)\n";
    rt.stop();
    return 1;
  }
  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    std::cerr << "Stub provider enumerate failed (default capture profile snapshot smoke)\n";
    rt.stop();
    return 1;
  }

  rt.attach_provider(&prov);
  if (rt.try_open_device(eps[0].hardware_id, kDeviceInstanceId, kRootId) != TryOpenDeviceStatus::OK) {
    std::cerr << "try_open_device failed (default capture profile snapshot smoke)\n";
    rt.stop();
    return 1;
  }

  if (!wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
        for (const auto& d : s.devices) {
          if (d.instance_id != kDeviceInstanceId || d.phase != CBLifecyclePhase::LIVE) continue;
          const auto& still = d.capture_profile.still;
          const auto& members = still.still_image_bundle.members;
          return still.version == 0 &&
                 still.width != 0 &&
                 still.height != 0 &&
                 still.format != 0 &&
                 members.size() == 1 &&
                 members[0].image_member_index == 0 &&
                 members[0].role == CaptureStillImageMemberRole::DEFAULT_METERED &&
                 members[0].intended_exposure_compensation_milli_ev == 0;
        }
        return false;
      })) {
    std::cerr << "Timeout waiting for opened device snapshot with retained default still profile\n";
    rt.stop();
    return 1;
  }

  CaptureRequest req{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req)) {
    std::cerr << "Expected materialized request for default capture profile snapshot smoke\n";
    rt.stop();
    return 1;
  }

  auto snap = get_last_snapshot(buf);
  if (!snap) {
    std::cerr << "Snapshot missing after default capture profile wait\n";
    rt.stop();
    return 1;
  }
  const DeviceState* device = nullptr;
  for (const auto& d : snap->devices) {
    if (d.instance_id == kDeviceInstanceId) {
      device = &d;
      break;
    }
  }
  if (!device) {
    std::cerr << "Opened device missing from snapshot after default capture profile wait\n";
    rt.stop();
    return 1;
  }

  const auto& still = device->capture_profile.still;
  if (still.version != 0 ||
      still.width != req.width ||
      still.height != req.height ||
      still.format != req.format_fourcc) {
    std::cerr << "Snapshot default still profile mismatch. snapshot version=" << still.version
              << " width=" << still.width
              << " height=" << still.height
              << " format=" << still.format
              << " materialized width=" << req.width
              << " height=" << req.height
              << " format=" << req.format_fourcc << "\n";
    rt.stop();
    return 1;
  }
  const auto& members = still.still_image_bundle.members;
  if (members.size() != 1 ||
      members[0].image_member_index != 0 ||
      members[0].role != CaptureStillImageMemberRole::DEFAULT_METERED ||
      members[0].intended_exposure_compensation_milli_ev != 0) {
    std::cerr << "Snapshot default still_image_bundle was not a single DEFAULT_METERED member\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_still_capture_profile_version_idempotency_smoke(StateSnapshotBuffer& /*unused_buf*/) {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (still profile idempotency smoke)\n";
    return 1;
  }
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed (still profile idempotency smoke)\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);
  if (!wait_for_snapshot_gen(buf, 0)) {
    std::cerr << "Timeout waiting for initial snapshot publication (still profile idempotency smoke)\n";
    rt.stop();
    return 1;
  }

  CaptureRequest req{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req)) {
    std::cerr << "Expected materialized request for baseline version check\n";
    rt.stop();
    return 1;
  }
  const uint64_t v0 = req.profile_version;
  const auto default_bundle_ready = wait_until([&]() {
    CaptureRequest r{};
    return rt.materialize_capture_request(kDeviceInstanceId, r) &&
           r.still_image_bundle.members.size() == 1 &&
           r.still_image_bundle.members[0].image_member_index == 0 &&
           r.still_image_bundle.members[0].role == CaptureStillImageMemberRole::DEFAULT_METERED &&
           r.still_image_bundle.members[0].intended_exposure_compensation_milli_ev == 0;
  }, 400, 1);
  if (!default_bundle_ready) {
    std::cerr << "Expected default snapshot still_image_bundle with one DEFAULT_METERED member\n";
    rt.stop();
    return 1;
  }
  // setup_one_stream() proves core/provider convergence, but not that the
  // snapshot buffer has already observed the fully realized stream/capture
  // topology. Force and wait for a publish that includes the existing
  // device/stream/acquisition-session shape before taking the topology baseline
  // used for still-profile idempotency checks.
  rt.request_publish();
  const auto topology_baseline_ready = wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& snap) {
    const bool has_device = std::any_of(snap.devices.begin(), snap.devices.end(), [](const DeviceState& d) {
      return d.instance_id == kDeviceInstanceId;
    });
    const bool has_stream = std::any_of(snap.streams.begin(), snap.streams.end(), [](const StreamState& s) {
      return s.stream_id == kStreamId && s.device_instance_id == kDeviceInstanceId;
    });
    const bool has_acquisition_session = std::any_of(
        snap.acquisition_sessions.begin(),
        snap.acquisition_sessions.end(),
        [](const AcquisitionSessionState& a) {
          return a.device_instance_id == kDeviceInstanceId;
        });
    return has_device && has_stream && has_acquisition_session;
  });
  if (!topology_baseline_ready) {
    std::cerr << "Expected settled snapshot topology before still profile/bundle topology_version check\n";
    rt.stop();
    return 1;
  }
  auto snap_before_profile = get_last_snapshot(buf);
  const uint64_t topo_before_profile = snap_before_profile ? snap_before_profile->topology_version : 0;

  req.capture_id = 9901;
  if (!prov.trigger_capture(req).ok()) {
    std::cerr << "Expected trigger_capture success for baseline version check\n";
    rt.stop();
    return 1;
  }
  CaptureRequest req_after_trigger{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req_after_trigger) ||
      req_after_trigger.profile_version != v0) {
    std::cerr << "trigger_capture must not change capture_profile_version\n";
    rt.stop();
    return 1;
  }

  auto is_one_member_default_bundle = [](const CaptureStillImageBundleState& bundle) {
    return bundle.members.size() == 1 &&
           bundle.members[0].image_member_index == 0 &&
           bundle.members[0].role == CaptureStillImageMemberRole::DEFAULT_METERED &&
           bundle.members[0].intended_exposure_compensation_milli_ev == 0;
  };
  auto is_still_profile = [](const StillCaptureProfileState& still, const CaptureProfile& profile) {
    return still.width == profile.width &&
           still.height == profile.height &&
           still.format == profile.format_fourcc;
  };

  CaptureProfile p{};
  p.width = req.width + 16;
  p.height = req.height;
  p.format_fourcc = req.format_fourcc;
  CaptureStillImageBundle s = make_default_metered_still_image_bundle();

  if (rt.try_set_device_still_capture_profile(kDeviceInstanceId, p, s) != TrySetStillCaptureProfileStatus::OK) {
    std::cerr << "Expected simple one-member still profile set success\n";
    rt.stop();
    return 1;
  }
  if (!wait_until([&]() {
        CaptureRequest r{};
        return rt.materialize_capture_request(kDeviceInstanceId, r) && r.profile_version > v0;
      }, 400, 5)) {
    std::cerr << "Expected profile_version increment after simple one-member still profile set\n";
    rt.stop();
    return 1;
  }
  CaptureRequest req_after_set{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req_after_set)) {
    std::cerr << "Expected materialized request after simple one-member still profile set\n";
    rt.stop();
    return 1;
  }
  const uint64_t v1 = req_after_set.profile_version;
  const auto device_one_member_ready = wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& snap) {
    for (const auto& d : snap.devices) {
      if (d.instance_id != kDeviceInstanceId) continue;
      return d.capture_profile.still.version == v1 &&
             is_still_profile(d.capture_profile.still, p) &&
             is_one_member_default_bundle(d.capture_profile.still.still_image_bundle);
    }
    return false;
  });
  if (!device_one_member_ready) {
    std::cerr << "Expected one-member still_image_bundle and explicit still profile snapshot on device after simple set\n";
    rt.stop();
    return 1;
  }
  auto snap_after_profile = get_last_snapshot(buf);
  if (snap_after_profile && snap_after_profile->topology_version != topo_before_profile) {
    std::cerr << "topology_version must not change solely on still profile/bundle configuration\n";
    rt.stop();
    return 1;
  }

  if (rt.try_set_device_still_capture_profile(kDeviceInstanceId, p, s) != TrySetStillCaptureProfileStatus::OK) {
    std::cerr << "Expected identical one-member still profile set idempotency success\n";
    rt.stop();
    return 1;
  }
  CaptureRequest req_after_same{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req_after_same) ||
      req_after_same.profile_version != v1) {
    std::cerr << "Identical one-member still profile set must preserve profile_version\n";
    rt.stop();
    return 1;
  }

  CaptureProfile p2 = p;
  p2.width = p.width + 16;
  if (rt.try_set_device_still_capture_profile(kDeviceInstanceId, p2, s) != TrySetStillCaptureProfileStatus::OK) {
    std::cerr << "Expected changed one-member still profile set success\n";
    rt.stop();
    return 1;
  }
  CaptureRequest req_after_changed{};
  if (!wait_until([&]() {
        CaptureRequest r{};
        return rt.materialize_capture_request(kDeviceInstanceId, r) && r.profile_version == v1 + 1;
      }, 400, 5)) {
    std::cerr << "Changed one-member still profile set must increment profile_version exactly once\n";
    rt.stop();
    return 1;
  }
  if (!rt.materialize_capture_request(kDeviceInstanceId, req_after_changed)) {
    std::cerr << "Expected materialized request after changed one-member still profile set\n";
    rt.stop();
    return 1;
  }
  req_after_changed.capture_id = 9902;
  if (!prov.trigger_capture(req_after_changed).ok()) {
    std::cerr << "Expected trigger_capture success after one-member profile change\n";
    rt.stop();
    return 1;
  }
  CaptureRequest req_after_trigger2{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req_after_trigger2) ||
      req_after_trigger2.profile_version != v1 + 1) {
    std::cerr << "trigger_capture after profile set must not increment profile_version\n";
    rt.stop();
    return 1;
  }
  const auto acquisition_session_bundle_ready = wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& snap) {
    for (const auto& a : snap.acquisition_sessions) {
      if (a.device_instance_id != kDeviceInstanceId) continue;
      return a.capture_profile.still.version == v1 + 1 &&
             is_still_profile(a.capture_profile.still, p2) &&
             is_one_member_default_bundle(a.capture_profile.still.still_image_bundle);
    }
    return false;
  });
  if (!acquisition_session_bundle_ready) {
    std::cerr << "Expected one-member still_image_bundle snapshot on acquisition session after capture-context realization\n";
    rt.stop();
    return 1;
  }

  auto snap_before_rejected_multi = get_last_snapshot(buf);
  const uint64_t topo_before_rejected_multi = snap_before_rejected_multi ? snap_before_rejected_multi->topology_version : 0;
  const uint64_t v_before_rejected_multi = req_after_trigger2.profile_version;
  CaptureStillImageBundle multi = make_default_metered_still_image_bundle();
  multi.members.push_back(CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, -1000});
  multi.members.push_back(CaptureStillImageMember{2u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, 1000});

  if (rt.try_set_device_still_capture_profile(kDeviceInstanceId, p2, multi) != TrySetStillCaptureProfileStatus::NotSupported) {
    std::cerr << "Expected multi-member still_image_bundle NotSupported for StubProvider\n";
    rt.stop();
    return 1;
  }
  CaptureRequest req_after_rejected_multi{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req_after_rejected_multi) ||
      req_after_rejected_multi.profile_version != v_before_rejected_multi) {
    std::cerr << "Unexpected profile_version mutation after rejected multi-member still_image_bundle\n";
    rt.stop();
    return 1;
  }
  auto snap_after_rejected_multi = get_last_snapshot(buf);
  if (!snap_after_rejected_multi) {
    std::cerr << "Expected snapshot after rejected multi-member still_image_bundle check\n";
    rt.stop();
    return 1;
  }
  bool accepted_one_member_still_retained = false;
  for (const auto& d : snap_after_rejected_multi->devices) {
    if (d.instance_id != kDeviceInstanceId) continue;
    accepted_one_member_still_retained =
        d.capture_profile.still.version == v_before_rejected_multi &&
        is_still_profile(d.capture_profile.still, p2) &&
        is_one_member_default_bundle(d.capture_profile.still.still_image_bundle);
    break;
  }
  if (!accepted_one_member_still_retained ||
      snap_after_rejected_multi->topology_version != topo_before_rejected_multi) {
    std::cerr << "Unexpected mutation after rejected multi-member still_image_bundle\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_rig_cohort_admission_from_preflight_smoke() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (rig cohort admit smoke)\n";
    return 1;
  }
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed (rig cohort admit smoke)\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    std::cerr << "Failed to enumerate endpoints (rig cohort admit smoke)\n";
    rt.stop();
    return 1;
  }
  const std::string live_hw = eps[0].hardware_id;
  if (!rt.smoke_set_rig_member_hardware_ids(8001, {live_hw})) {
    std::cerr << "Failed to set rig members (rig cohort admit smoke)\n";
    rt.stop();
    return 1;
  }
  const auto preflight_ok = wait_for_rig_preflight_ok(rt, 8001);
  if (!preflight_ok.ok) {
    print_rig_preflight_result("Preflight failed unexpectedly (rig cohort admit smoke)", preflight_ok);
    rt.stop();
    return 1;
  }

  const auto admitted = rt.smoke_admit_rig_cohort_from_preflight(8001, 9001, preflight_ok);
  if (!admitted.ok || admitted.failure != CoreRuntime::RigCohortAdmissionFailure::None ||
      admitted.capture_id != 9001 || admitted.rig_id != 8001 || admitted.participants.size() != 1) {
    std::cerr << "Expected successful cohort admission from preflight\n";
    rt.stop();
    return 1;
  }
  const auto& p = admitted.participants[0];
  if (p.hardware_id != live_hw ||
      p.request.capture_id != 9001 ||
      p.request.rig_id != 8001 ||
      p.request.device_instance_id != kDeviceInstanceId) {
    std::cerr << "Admitted request bundle stamping/traceability mismatch\n";
    rt.stop();
    return 1;
  }

  const auto zero_capture = rt.smoke_admit_rig_cohort_from_preflight(8001, 0, preflight_ok);
  if (zero_capture.ok || zero_capture.failure != CoreRuntime::RigCohortAdmissionFailure::InvalidCaptureId) {
    std::cerr << "Expected InvalidCaptureId for capture_id=0\n";
    rt.stop();
    return 1;
  }

  const auto dup_capture = rt.smoke_admit_rig_cohort_from_preflight(8001, 9001, preflight_ok);
  if (dup_capture.ok || dup_capture.failure != CoreRuntime::RigCohortAdmissionFailure::DuplicateCaptureId) {
    std::cerr << "Expected DuplicateCaptureId on second insert\n";
    rt.stop();
    return 1;
  }

  const auto bad_preflight = rt.preflight_rig_participants_materialize(9999);
  if (bad_preflight.ok) {
    std::cerr << "Expected missing-rig preflight failure\n";
    rt.stop();
    return 1;
  }
  const auto admit_bad_preflight = rt.smoke_admit_rig_cohort_from_preflight(9999, 9002, bad_preflight);
  if (admit_bad_preflight.ok || admit_bad_preflight.failure != CoreRuntime::RigCohortAdmissionFailure::PreflightFailed) {
    std::cerr << "Expected PreflightFailed when admitting failed preflight\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_rig_cohort_admission_imaging_spec_constraint_smoke() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (rig cohort imaging-spec admit smoke)\n";
    return 1;
  }
  if (!wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "CoreRuntime did not reach LIVE before rig cohort imaging-spec admit smoke\n";
    rt.stop();
    return 1;
  }
  const auto unavailable = rt.smoke_admit_rig_cohort_from_preflight(
      8002, 9001, make_manual_rig_preflight(8002, {"camA", "camB"}));
  if (unavailable.ok ||
      unavailable.failure != CoreRuntime::RigCohortAdmissionFailure::ImagingSpecUnavailable) {
    std::cerr << "Expected distinct ImagingSpecUnavailable when no grouped camera concurrency truth is configured\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  const auto configured_supported = rt.ingest_camera_concurrency_json_for_server(
      make_adc_camera_concurrency_json(
          {"camA", "camB", "camC", "camD"},
          true,
          {{"camA", "camB", "camC"}},
          3));
  if (configured_supported.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok) {
    std::cerr << "Expected supported camera concurrency ingestion for rig cohort imaging-spec admit smoke\n";
    return 1;
  }
  if (!rt.start()) {
    std::cerr << "CoreRuntime restart failed (rig cohort imaging-spec admit smoke)\n";
    return 1;
  }
  if (!wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "CoreRuntime restart did not reach LIVE before supported rig cohort imaging-spec admit smoke\n";
    rt.stop();
    return 1;
  }

  const auto subset_allowed = rt.smoke_admit_rig_cohort_from_preflight(
      8002,
      9002,
      make_manual_rig_preflight(8002, {"camA", "camB"}));
  if (!subset_allowed.ok ||
      subset_allowed.failure != CoreRuntime::RigCohortAdmissionFailure::None ||
      subset_allowed.participants.size() != 2) {
    std::cerr << "Expected two-camera grouped admission to be covered by reported three-camera combination\n";
    rt.stop();
    return 1;
  }

  const auto larger_rejected = rt.smoke_admit_rig_cohort_from_preflight(
      8002,
      9003,
      make_manual_rig_preflight(8002, {"camA", "camB", "camC", "camD"}));
  if (larger_rejected.ok ||
      larger_rejected.failure != CoreRuntime::RigCohortAdmissionFailure::ImagingSpecRejected) {
    std::cerr << "Expected larger grouped request without covering combination to be rejected\n";
    rt.stop();
    return 1;
  }

  const auto unrelated_rejected = rt.smoke_admit_rig_cohort_from_preflight(
      8002,
      9004,
      make_manual_rig_preflight(8002, {"camA", "camD"}));
  if (unrelated_rejected.ok ||
      unrelated_rejected.failure != CoreRuntime::RigCohortAdmissionFailure::ImagingSpecRejected) {
    std::cerr << "Expected unrelated grouped request to be rejected by camera concurrency truth\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  const auto configured_unsupported = rt.ingest_camera_concurrency_json_for_server(
      make_adc_camera_concurrency_json({"camA", "camB", "camC"}, false, {}, 3));
  if (configured_unsupported.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok) {
    std::cerr << "Expected unsupported camera concurrency ingestion for rig cohort imaging-spec admit smoke\n";
    return 1;
  }
  if (!rt.start()) {
    std::cerr << "CoreRuntime second restart failed (rig cohort imaging-spec admit smoke)\n";
    return 1;
  }
  if (!wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "CoreRuntime second restart did not reach LIVE before unsupported rig cohort imaging-spec admit smoke\n";
    rt.stop();
    return 1;
  }
  const auto unsupported_rejected = rt.smoke_admit_rig_cohort_from_preflight(
      8002,
      9005,
      make_manual_rig_preflight(8002, {"camA", "camB"}));
  if (unsupported_rejected.ok ||
      unsupported_rejected.failure != CoreRuntime::RigCohortAdmissionFailure::ImagingSpecRejected) {
    std::cerr << "Expected explicit unsupported grouped concurrency truth to reject grouped cohort admission\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_rig_orchestration_imaging_spec_admission_propagation_smoke() {
  constexpr uint64_t kRigId = 8402;
  constexpr uint64_t kFirstCaptureId = 9401;
  constexpr uint64_t kSecondCaptureId = 9402;
  constexpr uint64_t kThirdCaptureId = 9403;

  CoreRuntime rt;
  const auto has_capture_lifecycle_report = [&](uint64_t capture_id) {
    const auto reports = rt.recent_capture_lifecycle_timing_reports();
    return std::any_of(reports.begin(), reports.end(), [capture_id](const CoreCaptureLifecycleTimingReport& report) {
      return report.capture_id == capture_id;
    });
  };

  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (rig orchestration imaging-spec propagation smoke)\n";
    return 1;
  }
  if (!wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "CoreRuntime did not reach LIVE before rig orchestration imaging-spec propagation smoke\n";
    rt.stop();
    return 1;
  }
  const CoreRuntime::RigPreflightResult unavailable_preflight =
      make_manual_rig_preflight(kRigId, {"camA", "camB"});
  const auto first_fail =
      rt.smoke_orchestrate_rig_capture_from_preflight(
          kRigId,
          kFirstCaptureId,
          unavailable_preflight);
  if (first_fail.ok ||
      first_fail.failure != CoreRuntime::RigOrchestrationFailure::AdmissionFailed ||
      first_fail.admission_failure != CoreRuntime::RigCohortAdmissionFailure::ImagingSpecUnavailable ||
      first_fail.preflight_failure != CoreRuntime::RigPreflightFailure::None ||
      first_fail.submission_failure != CoreRuntime::RigSubmissionFailure::None ||
      first_fail.submitted_count != 0 ||
      first_fail.provider_error_code != 0) {
    std::cerr << "Expected rig orchestration helper to surface unavailable grouped concurrency truth distinctly\n";
    rt.stop();
    return 1;
  }
  if (has_capture_lifecycle_report(kFirstCaptureId) ||
      !rt.get_capture_result_set(kFirstCaptureId).empty()) {
    std::cerr << "Unavailable grouped concurrency truth must not reach provider submission\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  const auto configured_supported = rt.ingest_camera_concurrency_json_for_server(
      make_adc_camera_concurrency_json(
          {"camA", "camB", "camC", "camD"},
          true,
          {{"camA", "camB", "camC"}},
          3));
  if (configured_supported.status != CoreRuntime::IngestCameraConcurrencyStatus::Ok) {
    std::cerr << "Expected supported camera concurrency ingestion for rig orchestration propagation smoke\n";
    return 1;
  }
  if (!rt.start()) {
    std::cerr << "CoreRuntime restart failed (rig orchestration imaging-spec propagation smoke)\n";
    return 1;
  }
  if (!wait_until([&]() { return rt.state_copy() == CoreRuntimeState::LIVE; }, 200, 1)) {
    std::cerr << "CoreRuntime restart did not reach LIVE before rejected rig orchestration propagation smoke\n";
    rt.stop();
    return 1;
  }

  const CoreRuntime::RigPreflightResult rejected_preflight =
      make_manual_rig_preflight(kRigId, {"camA", "camD"});

  const auto second_fail =
      rt.smoke_orchestrate_rig_capture_from_preflight(
          kRigId,
          kSecondCaptureId,
          rejected_preflight);
  if (second_fail.ok ||
      second_fail.failure != CoreRuntime::RigOrchestrationFailure::AdmissionFailed ||
      second_fail.admission_failure != CoreRuntime::RigCohortAdmissionFailure::ImagingSpecRejected ||
      second_fail.preflight_failure != CoreRuntime::RigPreflightFailure::None ||
      second_fail.submission_failure != CoreRuntime::RigSubmissionFailure::None ||
      second_fail.submitted_count != 0 ||
      second_fail.provider_error_code != 0) {
    std::cerr << "Expected repeated rig orchestration helper rejection to stay deterministic\n";
    rt.stop();
    return 1;
  }
  const auto third_fail =
      rt.smoke_orchestrate_rig_capture_from_preflight(
          kRigId,
          kThirdCaptureId,
          rejected_preflight);
  if (third_fail.ok ||
      third_fail.failure != CoreRuntime::RigOrchestrationFailure::AdmissionFailed ||
      third_fail.admission_failure != CoreRuntime::RigCohortAdmissionFailure::ImagingSpecRejected ||
      third_fail.preflight_failure != CoreRuntime::RigPreflightFailure::None ||
      third_fail.submission_failure != CoreRuntime::RigSubmissionFailure::None ||
      third_fail.submitted_count != 0 ||
      third_fail.provider_error_code != 0) {
    std::cerr << "Expected repeated grouped concurrency rejection to stay deterministic\n";
    rt.stop();
    return 1;
  }
  if (has_capture_lifecycle_report(kSecondCaptureId) ||
      has_capture_lifecycle_report(kThirdCaptureId) ||
      !rt.get_capture_result_set(kSecondCaptureId).empty() ||
      !rt.get_capture_result_set(kThirdCaptureId).empty()) {
    std::cerr << "Rejected grouped concurrency truth must not reach provider capture submission\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_rig_bundle_submission_smoke() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (rig submit smoke)\n";
    return 1;
  }
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed (rig submit smoke)\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    std::cerr << "Failed to enumerate endpoints (rig submit smoke)\n";
    rt.stop();
    return 1;
  }
  const std::string live_hw = eps[0].hardware_id;
  if (!rt.smoke_set_rig_member_hardware_ids(8101, {live_hw})) {
    std::cerr << "Failed to set rig members (rig submit smoke)\n";
    rt.stop();
    return 1;
  }

  const auto preflight_ok = wait_for_rig_preflight_ok(rt, 8101);
  if (!preflight_ok.ok) {
    print_rig_preflight_result("Preflight failed before rig submit smoke success admission", preflight_ok);
    rt.stop();
    return 1;
  }
  const auto admitted_ok = rt.smoke_admit_rig_cohort_from_preflight(8101, 9101, preflight_ok);
  if (!admitted_ok.ok) {
    std::cerr << "Failed to admit cohort for submit smoke success path\n";
    rt.stop();
    return 1;
  }
  const auto submit_ok = rt.smoke_submit_admitted_rig_bundle(admitted_ok);
  if (!submit_ok.ok || submit_ok.submitted_count != admitted_ok.participants.size()) {
    std::cerr << "Expected successful participant submissions\n";
    rt.stop();
    return 1;
  }
  if (admitted_ok.participants[0].request.still_image_bundle.members.size() != 1 ||
      admitted_ok.participants[0].request.still_image_bundle.members[0].image_member_index != 0 ||
      admitted_ok.participants[0].request.still_image_bundle.members[0].role != CaptureStillImageMemberRole::DEFAULT_METERED) {
    std::cerr << "Expected one-member default-metered sequence in admitted request\n";
    rt.stop();
    return 1;
  }

  // Multi-image request rejected when provider lacks multi-image capability.
  auto multi_member_invalid = admitted_ok;
  multi_member_invalid.capture_id = 9104;
  multi_member_invalid.participants[0].request.capture_id = 9104;
  multi_member_invalid.participants[0].request.still_image_bundle.members.push_back(
      CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET});
  if (!rt.smoke_admit_rig_cohort_from_preflight(8101, 9104, preflight_ok).ok) {
    std::cerr << "Failed to admit cohort for multi-image rejection case\n";
    rt.stop();
    return 1;
  }
  const auto multi_member_fail = rt.smoke_submit_admitted_rig_bundle(multi_member_invalid);
  if (multi_member_fail.ok ||
      multi_member_fail.failure != CoreRuntime::RigSubmissionFailure::TriggerFailed ||
      multi_member_fail.provider_error_code != static_cast<uint32_t>(ProviderError::ERR_INVALID_ARGUMENT)) {
    std::cerr << "Expected deterministic rejection for two-member sequence without provider support\n";
    rt.stop();
    return 1;
  }

  // First participant synchronous failure: invalid device id in first entry.
  auto bad_first = admitted_ok;
  bad_first.capture_id = 9102;
  bad_first.participants[0].request.capture_id = 9102;
  bad_first.participants[0].request.device_instance_id = 999999;
  if (!rt.smoke_admit_rig_cohort_from_preflight(8101, 9102, preflight_ok).ok) {
    std::cerr << "Failed to admit cohort for first-fail case\n";
    rt.stop();
    return 1;
  }
  const auto first_fail = rt.smoke_submit_admitted_rig_bundle(bad_first);
  if (first_fail.ok || first_fail.failure != CoreRuntime::RigSubmissionFailure::TriggerFailed ||
      first_fail.submitted_count != 0 || first_fail.failed_index != 0) {
    std::cerr << "Expected first participant trigger failure\n";
    rt.stop();
    return 1;
  }

  // Grouped provider submission failure: an invalid participant in the bundle
  // causes the provider submission to fail; grouped submission does not expose
  // per-participant partial progress.
  auto two_part = admitted_ok;
  two_part.capture_id = 9103;
  two_part.participants[0].request.capture_id = 9103;
  CoreRuntime::RigAdmittedParticipantRequest second = two_part.participants[0];
  second.request.capture_id = 9103;
  second.request.device_instance_id = 888888;
  two_part.participants.push_back(second);
  if (!rt.smoke_admit_rig_cohort_from_preflight(8101, 9103, preflight_ok).ok) {
    std::cerr << "Failed to admit cohort for grouped-fail case\n";
    rt.stop();
    return 1;
  }
  const auto later_fail = rt.smoke_submit_admitted_rig_bundle(two_part);
  if (later_fail.ok || later_fail.failure != CoreRuntime::RigSubmissionFailure::TriggerFailed ||
      later_fail.submitted_count != 0 || later_fail.failed_index != 0 ||
      later_fail.provider_error_code != static_cast<uint32_t>(ProviderError::ERR_BAD_STATE)) {
    std::cerr << "Expected grouped submission failure for invalid participant bundle\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_cohort_aware_capture_result_set_smoke() {
  constexpr uint64_t kSecondDeviceInstanceId = 2;
  constexpr uint64_t kSecondRootId = 2;

  CoreRuntime rt;
  if (!rt.start()) return 1;
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) { rt.stop(); return 1; }
  rt.attach_provider(&prov);

  auto emit_capture = [&](uint64_t capture_id, uint64_t device_id, uint8_t fill) {
    static std::vector<uint8_t> bytes(2 * 2 * 4, 0);
    std::fill(bytes.begin(), bytes.end(), fill);
    FrameView frame{};
    frame.capture_id = capture_id;
    frame.device_instance_id = device_id;
    frame.stream_id = 0;
    frame.width = 2;
    frame.height = 2;
    frame.format_fourcc = FOURCC_RGBA;
    frame.data = bytes.data();
    frame.size_bytes = bytes.size();
    frame.stride_bytes = 0;
    frame.release = [](void*, const FrameView*) {};
    frame.release_user = nullptr;
    rt.provider_callbacks()->on_capture_started(capture_id, device_id);
    rt.provider_callbacks()->on_frame(frame);
    rt.provider_callbacks()->on_capture_completed(capture_id, device_id);
  };

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) { rt.stop(); return 1; }

  const auto retain_identity = rt.retain_device_identity(kSecondDeviceInstanceId, eps[0].hardware_id);
  if (retain_identity != CoreThread::PostResult::Enqueued) {
    std::cerr << "Cohort result-set smoke: failed to retain second device identity\n";
    rt.stop();
    return 1;
  }
  if (rt.try_open_device(eps[0].hardware_id, kSecondDeviceInstanceId, kSecondRootId) !=
      TryOpenDeviceStatus::OK) {
    std::cerr << "Cohort result-set smoke: failed to open second device for non-cohort control case\n";
    rt.stop();
    return 1;
  }
  if (!converge_stub_provider_core(rt, prov)) {
    std::cerr << "Cohort result-set smoke: second device open did not converge\n";
    rt.stop();
    return 1;
  }

  CaptureRequest second_capture_req{};
  if (!wait_until([&]() { return rt.materialize_capture_request(kSecondDeviceInstanceId, second_capture_req); }, 200, 1)) {
    std::cerr << "Cohort result-set smoke: second device capture request did not materialize\n";
    rt.stop();
    return 1;
  }

  // No cohort path: accept-all assembly-successful candidates.
  emit_capture(9201, kDeviceInstanceId, 1);
  emit_capture(9201, kSecondDeviceInstanceId, 2);
  if (!wait_until([&]() { return rt.get_capture_result_set(9201).size() == 2; }, 400, 5)) {
    std::cerr << "Cohort result-set smoke: non-cohort capture_id=9201 never reached size 2\n";
    rt.stop();
    return 1;
  }

  if (rt.try_close_device(kSecondDeviceInstanceId) != TryCloseDeviceStatus::OK) {
    std::cerr << "Cohort result-set smoke: failed to close second device before cohort phase\n";
    rt.stop();
    return 1;
  }
  if (!converge_stub_provider_core(rt, prov)) {
    std::cerr << "Cohort result-set smoke: second device close did not converge\n";
    rt.stop();
    return 1;
  }

  if (!rt.smoke_set_rig_member_hardware_ids(8201, {eps[0].hardware_id})) { rt.stop(); return 1; }
  const auto preflight = wait_for_rig_preflight_ok(rt, 8201);
  if (!preflight.ok) {
    print_rig_preflight_result("Preflight failed before cohort result-set smoke admission", preflight);
    rt.stop();
    return 1;
  }
  const auto admitted = rt.smoke_admit_rig_cohort_from_preflight(8201, 9202, preflight);
  if (!admitted.ok) {
    std::cerr << "Cohort result-set smoke: cohort admission failed for capture_id=9202\n";
    rt.stop();
    return 1;
  }

  // Cohort OPEN but incomplete => empty.
  if (!rt.get_capture_result_set(9202).empty()) {
    std::cerr << "Cohort result-set smoke: open incomplete cohort unexpectedly exposed results\n";
    rt.stop();
    return 1;
  }

  // Emit expected participant + extra successful non-expected participant.
  emit_capture(9202, admitted.participants[0].request.device_instance_id, 3);
  emit_capture(9202, 4242, 4);
  if (!wait_until([&]() { return rt.get_capture_result_set(9202).size() == 1; }, 400, 5)) {
    std::cerr << "Cohort result-set smoke: admitted one-member cohort never converged to size 1\n";
    rt.stop();
    return 1;
  }
  auto cohort_set = rt.get_capture_result_set(9202);
  if (cohort_set.size() != 1 ||
      cohort_set[0]->device_instance_id != admitted.participants[0].request.device_instance_id) {
    std::cerr << "Cohort result-set smoke: converged cohort result-set did not match admitted participant device\n";
    rt.stop();
    return 1;
  }

  // FAILED cohort => empty.
  rt.smoke_submit_admitted_rig_bundle(admitted); // may succeed; keep cohort OPEN
  // Force a new failed cohort via bad submit
  const auto admitted_fail = rt.smoke_admit_rig_cohort_from_preflight(8201, 9203, preflight);
  auto bad = admitted_fail;
  bad.participants[0].request.device_instance_id = 999999;
  (void)rt.smoke_submit_admitted_rig_bundle(bad);
  if (!rt.get_capture_result_set(9203).empty()) {
    std::cerr << "Cohort result-set smoke: failed cohort unexpectedly exposed results\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}


static int test_server_facing_rig_orchestration_adapter_smoke() {
  CoreRuntime rt;
  if (!rt.start()) return 1;
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) { rt.stop(); return 1; }
  rt.attach_provider(&prov);

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) { rt.stop(); return 1; }
  const std::string live_hw = eps[0].hardware_id;

  // Simulate CamBANGServer allocator shape: monotonic counter, skip 0.
  uint64_t next_capture_id = 1;
  auto allocate_capture_id = [&]() {
    uint64_t capture_id = next_capture_id++;
    if (capture_id == 0) {
      capture_id = next_capture_id++;
    }
    return capture_id;
  };

  if (!rt.smoke_set_rig_member_hardware_ids(8401, {live_hw})) { rt.stop(); return 1; }
  const auto server_preflight = wait_for_rig_preflight_ok(rt, 8401);
  if (!server_preflight.ok) {
    print_rig_preflight_result("Preflight failed before server-facing rig orchestration success", server_preflight);
    rt.stop();
    return 1;
  }

  const uint64_t ok_capture_id = allocate_capture_id();
  const auto success = rt.orchestrate_rig_capture_with_capture_id_for_server(8401, ok_capture_id);
  if (!success.ok || success.capture_id != ok_capture_id || success.rig_id != 8401 || success.submitted_count != 1) {
    std::cerr << "Expected server-facing rig orchestration success. capture_id=" << ok_capture_id
              << " rig_id=8401"
              << " result_ok=" << (success.ok ? "true" : "false")
              << " result_capture_id=" << success.capture_id
              << " result_rig_id=" << success.rig_id
              << " failure=" << static_cast<int>(success.failure)
              << " preflight_failure=" << static_cast<int>(success.preflight_failure)
              << " admission_failure=" << static_cast<int>(success.admission_failure)
              << " submission_failure=" << static_cast<int>(success.submission_failure)
              << " submitted_count=" << success.submitted_count
              << " failed_index=" << success.failed_index
              << " failed_device_instance_id=" << success.failed_device_instance_id
              << " provider_error_code=" << success.provider_error_code << "\n";
    rt.stop();
    return 1;
  }

  // StubProvider trigger_capture is lifecycle-only: it emits capture_started and
  // capture_completed but no capture frame. A non-empty CaptureResultSet requires
  // retained default-image assembly, which is covered by the cohort/result-set
  // smoke path that injects capture frames. This adapter smoke only verifies the
  // server-facing orchestration/admission/submission path succeeds.

  // Failure path: missing rig should fail and should not expose cohort result set.
  const uint64_t bad_capture_id = allocate_capture_id();
  const auto fail = rt.orchestrate_rig_capture_with_capture_id_for_server(999991, bad_capture_id);
  if (fail.ok || fail.failure != CoreRuntime::RigOrchestrationFailure::PreflightFailed) {
    rt.stop();
    return 1;
  }
  if (!rt.get_capture_result_set(bad_capture_id).empty()) {
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_result_store_identity_survives_runtime_restart_smoke() {
  auto wait_for_stream_result = [](CoreRuntime& rt, StubProvider& prov) -> SharedStreamResultData {
    SharedStreamResultData retained{};
    const bool observed = wait_until([&]() {
      prov.flush_callbacks_for_smoke();
      if (!wait_for_core_barrier(rt, std::chrono::milliseconds(50))) {
        return false;
      }
      retained = rt.get_latest_stream_result(kStreamId);
      return static_cast<bool>(retained);
    }, 200, 1);
    return observed ? retained : nullptr;
  };

  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "Restart identity smoke: first CoreRuntime start failed\n";
    return 1;
  }

  StubProvider first_provider;
  if (!setup_one_runtime_created_stream(rt, first_provider)) {
    rt.stop();
    return 1;
  }
  if (rt.try_start_stream(kStreamId) != TryStartStreamStatus::OK) {
    std::cerr << "Restart identity smoke: first stream start failed\n";
    rt.stop();
    return 1;
  }
  first_provider.emit_test_frames(kStreamId, 1);
  const auto before_restart = wait_for_stream_result(rt, first_provider);
  if (!before_restart || before_restart->retained_frame_id == 0) {
    std::cerr << "Restart identity smoke: no retained stream result before restart\n";
    rt.stop();
    return 1;
  }
  const uint64_t before_restart_id = before_restart->retained_frame_id;
  rt.stop();

  if (!rt.start()) {
    std::cerr << "Restart identity smoke: second CoreRuntime start failed\n";
    return 1;
  }

  StubProvider second_provider;
  if (!setup_one_runtime_created_stream(rt, second_provider)) {
    rt.stop();
    return 1;
  }
  if (rt.try_start_stream(kStreamId) != TryStartStreamStatus::OK) {
    std::cerr << "Restart identity smoke: second stream start failed\n";
    rt.stop();
    return 1;
  }
  second_provider.emit_test_frames(kStreamId, 1);
  const auto after_restart = wait_for_stream_result(rt, second_provider);
  if (!after_restart) {
    std::cerr << "Restart identity smoke: no retained stream result after restart\n";
    rt.stop();
    return 1;
  }
  if (after_restart->retained_frame_id <= before_restart_id) {
    std::cerr << "Restart identity smoke: retained_frame_id reset across restart. before="
              << before_restart_id << " after=" << after_restart->retained_frame_id << "\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_rig_orchestration_helper_smoke() {
  CoreRuntime rt;
  if (!rt.start()) return 1;
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) { rt.stop(); return 1; }
  rt.attach_provider(&prov);

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) { rt.stop(); return 1; }
  const std::string live_hw = eps[0].hardware_id;

  // Preflight failure: missing rig, no cohort/provider submit.
  const auto preflight_fail = rt.smoke_orchestrate_rig_capture_with_capture_id(9901, 9301);
  if (preflight_fail.ok ||
      preflight_fail.failure != CoreRuntime::RigOrchestrationFailure::PreflightFailed ||
      preflight_fail.preflight_failure != CoreRuntime::RigPreflightFailure::RigNotFound ||
      preflight_fail.admission_failure != CoreRuntime::RigCohortAdmissionFailure::None ||
      preflight_fail.submission_failure != CoreRuntime::RigSubmissionFailure::None) {
    rt.stop();
    return 1;
  }

  if (!rt.smoke_set_rig_member_hardware_ids(8301, {live_hw})) { rt.stop(); return 1; }
  const auto orchestration_preflight = wait_for_rig_preflight_ok(rt, 8301);
  if (!orchestration_preflight.ok) {
    print_rig_preflight_result("Preflight failed before rig orchestration helper success", orchestration_preflight);
    rt.stop();
    return 1;
  }

  // Invalid capture_id.
  const auto bad_capture_id = rt.smoke_orchestrate_rig_capture_with_capture_id(8301, 0);
  if (bad_capture_id.ok ||
      bad_capture_id.failure != CoreRuntime::RigOrchestrationFailure::InvalidCaptureId ||
      bad_capture_id.preflight_failure != CoreRuntime::RigPreflightFailure::None ||
      bad_capture_id.admission_failure != CoreRuntime::RigCohortAdmissionFailure::None ||
      bad_capture_id.submission_failure != CoreRuntime::RigSubmissionFailure::None) {
    rt.stop();
    return 1;
  }

  // Success.
  const auto success = rt.smoke_orchestrate_rig_capture_with_capture_id(8301, 9302);
  if (!success.ok || success.capture_id != 9302 || success.submitted_count != 1) {
    rt.stop();
    return 1;
  }
  const auto admission_fail = rt.smoke_orchestrate_rig_capture_with_capture_id(8301, 9302);
  if (admission_fail.ok ||
      admission_fail.failure != CoreRuntime::RigOrchestrationFailure::AdmissionFailed ||
      admission_fail.preflight_failure != CoreRuntime::RigPreflightFailure::None ||
      admission_fail.admission_failure != CoreRuntime::RigCohortAdmissionFailure::DuplicateCaptureId ||
      admission_fail.submission_failure != CoreRuntime::RigSubmissionFailure::None) {
    rt.stop();
    return 1;
  }

  // Submission failure leaves cohort failed.
  if (!rt.smoke_set_rig_member_hardware_ids(8302, {live_hw, live_hw})) { rt.stop(); return 1; }
  // keep preflight/admission valid by using one member then mutate device in submit path via bad endpoint
  if (!rt.smoke_set_rig_member_hardware_ids(8302, {live_hw})) { rt.stop(); return 1; }
  auto preflight = wait_for_rig_preflight_ok(rt, 8302);
  if (!preflight.ok) {
    print_rig_preflight_result("Preflight failed before rig orchestration submit-failure admission", preflight);
    rt.stop();
    return 1;
  }
  auto admitted = rt.smoke_admit_rig_cohort_from_preflight(8302, 9303, preflight);
  admitted.participants[0].request.device_instance_id = 777777;
  const auto submit_fail = rt.smoke_submit_admitted_rig_bundle(admitted);
  if (submit_fail.ok) { rt.stop(); return 1; }
  if (!rt.get_capture_result_set(9303).empty()) { rt.stop(); return 1; }

  // Orchestration-level submission failure case.
  if (!rt.smoke_set_rig_member_hardware_ids(8303, {"missing:hw"})) { rt.stop(); return 1; }
  const auto orchestration_fail = rt.smoke_orchestrate_rig_capture_with_capture_id(8303, 9304);
  if (orchestration_fail.ok ||
      orchestration_fail.failure != CoreRuntime::RigOrchestrationFailure::PreflightFailed ||
      orchestration_fail.preflight_failure != CoreRuntime::RigPreflightFailure::HardwareIdUnresolved ||
      orchestration_fail.admission_failure != CoreRuntime::RigCohortAdmissionFailure::None ||
      orchestration_fail.submission_failure != CoreRuntime::RigSubmissionFailure::None) {
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

#endif // CAMBANG_SMOKE_WITH_STUB_PROVIDER

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
// Lightweight per-iteration stress loop body (stub-provider mode).
static int stress_iteration(const Options& opt, std::mt19937& rng, int iter_index) {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);
  if (!rt.start()) {
    std::cerr << "[iter " << iter_index << "] CoreRuntime failed to start\n";
    return 1;
  }

  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "[iter " << iter_index << "] Stub provider setup failed\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);

  maybe_jitter(opt, rng);

  const StreamRequest req = make_req();
  if (!prov.start_stream(kStreamId, req.profile, req.picture).ok()) {
    std::cerr << "[iter " << iter_index << "] start_stream failed\n";
    rt.stop();
    return 1;
  }

  maybe_jitter(opt, rng);

  // Emit a small burst and ensure all released.
  prov.emit_test_frames(kStreamId, 20);
  const uint64_t emitted = prov.frames_emitted();

  if (!wait_until([&]() { return prov.frames_released() >= emitted; }, 500, 2)) {
    std::cerr << "[iter " << iter_index << "] Timeout waiting for releases. emitted="
              << emitted << " released=" << prov.frames_released() << "\n";
    rt.stop();
    return 1;
  }

  // Occasionally validate overload accounting (every 10th iteration).
  if ((iter_index % 10) == 0) {
    const int r = test_overload_queuefull_release_accounting(rt, prov);
    if (r != 0) {
      std::cerr << "[iter " << iter_index << "] overload test failed\n";
      rt.stop();
      return r;
    }
    const int essential_r = test_non_frame_provider_fact_survives_ordinary_queue_full(rt, prov);
    if (essential_r != 0) {
      std::cerr << "[iter " << iter_index << "] essential provider fact overload test failed\n";
      rt.stop();
      return essential_r;
    }
  }

  maybe_jitter(opt, rng);

  rt.stop();

  if (!prov.shutting_down()) {
    std::cerr << "[iter " << iter_index << "] Expected provider shutting_down after stop\n";
    return 1;
  }

  if (prov.frames_released() != prov.frames_emitted()) {
    std::cerr << "[iter " << iter_index << "] Leak risk: emitted=" << prov.frames_emitted()
              << " released=" << prov.frames_released() << "\n";
    return 1;
  }

  const auto diag = rt.shutdown_diag_copy();
  if (diag.phase_changes == 0 || diag.phase_code != CoreRuntime::shutdown_phase_exit_code()) {
    std::cerr << "[iter " << iter_index << "] Shutdown diag unexpected. phase_code="
              << static_cast<int>(diag.phase_code)
              << " changes=" << diag.phase_changes << "\n";
    return 1;
  }

  return 0;
}

#endif // CAMBANG_SMOKE_WITH_STUB_PROVIDER

} // namespace
int main(int argc, char** argv) {
  Options opt;
  const ParseOptsResult parse_result = parse_opts(argc, argv, opt);
  if (parse_result == ParseOptsResult::Help) {
    return 0;
  }
  if (parse_result != ParseOptsResult::Ok) {
    std::fprintf(stdout, "FAIL core_spine_smoke reason=invalid_arguments\n");
    std::fflush(stdout);
    return 2;
  }
  CheckReporter reporter(opt.verbose);

  if (reporter.verbose()) {
    std::cout << "[smoke] compiled provider: " << compiled_provider_name() << "\n";
  }

  // Default behaviour: run once, same structure/output as original.
  if (!opt.stress) {
    if (int r = reporter.run("test_core_spec_state_imaging_spec_retention_smoke",
                             [] { return test_core_spec_state_imaging_spec_retention_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_core_spec_state_imaging_spec_retention_smoke",
                               r);
      return r;
    }
    if (int r = reporter.run("test_runtime_imaging_spec_retention_publish_smoke",
                             [] { return test_runtime_imaging_spec_retention_publish_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_runtime_imaging_spec_retention_publish_smoke",
                               r);
      return r;
    }
    if (int r = reporter.run("test_adc_camera_description_parser_and_retention_smoke",
                             [] { return test_adc_camera_description_parser_and_retention_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_adc_camera_description_parser_and_retention_smoke",
                               r);
      return r;
    }
    if (int r = reporter.run("test_runtime_camera_concurrency_ingest_lifecycle_smoke",
                             [] { return test_runtime_camera_concurrency_ingest_lifecycle_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_runtime_camera_concurrency_ingest_lifecycle_smoke",
                               r);
      return r;
    }
    if (int r = reporter.run("test_runtime_camera_concurrency_invalid_input_smoke",
                             [] { return test_runtime_camera_concurrency_invalid_input_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_runtime_camera_concurrency_invalid_input_smoke",
                               r);
      return r;
    }
    if (int r = reporter.run("test_capture_admission_context_smoke",
                             [] { return test_capture_admission_context_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_capture_admission_context_smoke", r);
      return r;
    }
    if (int r = reporter.run("test_provider_callback_ingress_null_core_thread_drop_accounting",
                             [] { return test_provider_callback_ingress_null_core_thread_drop_accounting(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_provider_callback_ingress_null_core_thread_drop_accounting",
                               r);
      return r;
    }
    if (int r = reporter.run("test_provider_callback_ingress_null_sink_frame_release_balances_telemetry",
                             [] { return test_provider_callback_ingress_null_sink_frame_release_balances_telemetry(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_provider_callback_ingress_null_sink_frame_release_balances_telemetry",
                               r);
      return r;
    }
    if (int r = reporter.run("test_publish_gating_before_start",
                             [] { return test_publish_gating_before_start(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_publish_gating_before_start", r);
      return r;
    }
    if (int r = reporter.run("test_display_demand_async_release_closed_accounting_before_start",
                             [] { return test_display_demand_async_release_closed_accounting_before_start(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_display_demand_async_release_closed_accounting_before_start",
                               r);
      return r;
    }
    if (int r = reporter.run("test_provider_open_close_refusal_visibility",
                             [] { return test_provider_open_close_refusal_visibility(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_provider_open_close_refusal_visibility", r);
      return r;
    }
    if (int r = reporter.run("test_stream_registry_non_ok_stop_ack_does_not_clobber_restart",
                             [] { return test_stream_registry_non_ok_stop_ack_does_not_clobber_restart(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_stream_registry_non_ok_stop_ack_does_not_clobber_restart",
                               r);
      return r;
    }

    CoreRuntime rt;
    StateSnapshotBuffer buf;
    rt.set_snapshot_publisher(&buf);

    // Providerless baseline (default smoke mode).
    if (int r = reporter.run("test_baseline_publish_without_provider",
                             [&] { return test_baseline_publish_without_provider(rt, buf); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_baseline_publish_without_provider", r);
      return r;
    }

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
    // Stub-provider integration tests (opt-in).
    StubProvider prov;
    if (int r = reporter.run("test_baseline_live_one_frame_and_snapshot",
                             [&] { return test_baseline_live_one_frame_and_snapshot(rt, buf, prov); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_baseline_live_one_frame_and_snapshot", r);
      return r;
    }
    if (int r = reporter.run("test_destroy_never_started_stream_smoke",
                             [&] {
                               const int rc = test_destroy_never_started_stream_smoke();
                               if (rc != 0) rt.stop();
                               return rc;
                             })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_destroy_never_started_stream_smoke", r);
      return r;
    }
    if (int r = reporter.run("test_strict_destroy_rejects_started_stream_smoke",
                             [&] {
                               const int rc = test_strict_destroy_rejects_started_stream_smoke();
                               if (rc != 0) rt.stop();
                               return rc;
                             })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_strict_destroy_rejects_started_stream_smoke", r);
      return r;
    }
    if (int r = reporter.run("test_stream_start_stop_idempotency_survives_delayed_provider_facts_smoke",
                             [&] {
                               const int rc = test_stream_start_stop_idempotency_survives_delayed_provider_facts_smoke();
                               if (rc != 0) rt.stop();
                               return rc;
                             })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_stream_start_stop_idempotency_survives_delayed_provider_facts_smoke",
                               r);
      return r;
    }
    if (int r = reporter.run("test_overload_queuefull_release_accounting",
                             [&] { return test_overload_queuefull_release_accounting(rt, prov); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_overload_queuefull_release_accounting", r);
      return r;
    }
    if (int r = reporter.run("test_non_frame_provider_fact_survives_ordinary_queue_full",
                             [&] { return test_non_frame_provider_fact_survives_ordinary_queue_full(rt, prov); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_non_frame_provider_fact_survives_ordinary_queue_full",
                               r);
      return r;
    }
    if (int r = reporter.run("test_shutdown_choreography",
                             [&] { return test_shutdown_choreography(rt, prov); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_shutdown_choreography", r);
      return r;
    }
    if (int r = reporter.run("test_device_capture_request_materialization_smoke",
                             [] { return test_device_capture_request_materialization_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_device_capture_request_materialization_smoke", r);
      return r;
    }
    if (int r = reporter.run("test_open_device_snapshot_retains_default_capture_profile_smoke",
                             [] { return test_open_device_snapshot_retains_default_capture_profile_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_open_device_snapshot_retains_default_capture_profile_smoke",
                               r);
      return r;
    }
    if (int r = reporter.run("test_still_capture_profile_version_idempotency_smoke",
                             [&] { return test_still_capture_profile_version_idempotency_smoke(buf); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_still_capture_profile_version_idempotency_smoke", r);
      return r;
    }
    if (int r = reporter.run("test_rig_preflight_materialization_smoke",
                             [] { return test_rig_preflight_materialization_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_rig_preflight_materialization_smoke", r);
      return r;
    }
    if (int r = reporter.run("test_rig_cohort_admission_from_preflight_smoke",
                             [] { return test_rig_cohort_admission_from_preflight_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_rig_cohort_admission_from_preflight_smoke", r);
      return r;
    }
    if (int r = reporter.run("test_rig_cohort_admission_imaging_spec_constraint_smoke",
                             [] { return test_rig_cohort_admission_imaging_spec_constraint_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_rig_cohort_admission_imaging_spec_constraint_smoke",
                               r);
      return r;
    }
    if (int r = reporter.run("test_rig_orchestration_imaging_spec_admission_propagation_smoke",
                             [] { return test_rig_orchestration_imaging_spec_admission_propagation_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_rig_orchestration_imaging_spec_admission_propagation_smoke",
                               r);
      return r;
    }
    if (int r = reporter.run("test_rig_bundle_submission_smoke",
                             [] { return test_rig_bundle_submission_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_rig_bundle_submission_smoke", r);
      return r;
    }
    if (int r = reporter.run("test_cohort_aware_capture_result_set_smoke",
                             [] { return test_cohort_aware_capture_result_set_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_cohort_aware_capture_result_set_smoke", r);
      return r;
    }
    if (int r = reporter.run("test_result_store_identity_survives_runtime_restart_smoke",
                             [] { return test_result_store_identity_survives_runtime_restart_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_result_store_identity_survives_runtime_restart_smoke",
                               r);
      return r;
    }
    if (int r = reporter.run("test_rig_orchestration_helper_smoke",
                             [] { return test_rig_orchestration_helper_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke", "test_rig_orchestration_helper_smoke", r);
      return r;
    }
    if (int r = reporter.run("test_server_facing_rig_orchestration_adapter_smoke",
                             [] { return test_server_facing_rig_orchestration_adapter_smoke(); })) {
      if (reporter.verbose()) reporter.print_summary();
      reporter.print_fail_line("core_spine_smoke",
                               "test_server_facing_rig_orchestration_adapter_smoke",
                               r);
      return r;
    }
#endif

    if (reporter.verbose()) {
      if (reporter.verbose()) reporter.print_summary();
    }
    reporter.print_pass_line("core_spine_smoke");
    return 0;
  }

  // Stress mode: portable churn loop.
  std::mt19937 rng(opt.seed);

  // Still do the pre-start gating check once.
  if (int r = reporter.run("test_provider_callback_ingress_null_core_thread_drop_accounting",
                           [] { return test_provider_callback_ingress_null_core_thread_drop_accounting(); })) {
    if (reporter.verbose()) reporter.print_summary();
    reporter.print_fail_line("core_spine_smoke",
                             "test_provider_callback_ingress_null_core_thread_drop_accounting",
                             r);
    return r;
  }
  if (int r = reporter.run("test_provider_callback_ingress_null_sink_frame_release_balances_telemetry",
                           [] { return test_provider_callback_ingress_null_sink_frame_release_balances_telemetry(); })) {
    if (reporter.verbose()) reporter.print_summary();
    reporter.print_fail_line("core_spine_smoke",
                             "test_provider_callback_ingress_null_sink_frame_release_balances_telemetry",
                             r);
    return r;
  }
  if (int r = reporter.run("test_publish_gating_before_start",
                           [] { return test_publish_gating_before_start(); })) {
    if (reporter.verbose()) reporter.print_summary();
    reporter.print_fail_line("core_spine_smoke", "test_publish_gating_before_start", r);
    return r;
  }
  if (int r = reporter.run("test_display_demand_async_release_closed_accounting_before_start",
                           [] { return test_display_demand_async_release_closed_accounting_before_start(); })) {
    if (reporter.verbose()) reporter.print_summary();
    reporter.print_fail_line("core_spine_smoke",
                             "test_display_demand_async_release_closed_accounting_before_start",
                             r);
    return r;
  }

  const int progress_interval = 25;

#if !defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
  std::cerr << "Stress mode requires CAMBANG_SMOKE_WITH_STUB_PROVIDER; build core_spine_smoke through the repo SCons maintainer_tools alias.\n";
  std::fprintf(stdout, "FAIL core_spine_smoke reason=stress_requires_stub_provider\n");
  std::fflush(stdout);
  return 2;
#else
  if (int r = reporter.run("stress_iteration_loop",
                           [&] {
                             for (int i = 1; i <= opt.loops; ++i) {
                               const int rc = stress_iteration(opt, rng, i);
                               if (rc != 0) return rc;

                               if (reporter.verbose() &&
                                   ((i % progress_interval) == 0 || i == opt.loops)) {
                                 std::cout << "[stress] iteration " << i << "/" << opt.loops << " OK\n";
                               }
                             }
                             return 0;
                           })) {
    if (reporter.verbose()) reporter.print_summary();
    reporter.print_fail_line("core_spine_smoke", "stress_iteration_loop", r);
    return r;
  }

  if (reporter.verbose()) {
    if (reporter.verbose()) reporter.print_summary();
  }
  std::fprintf(stdout,
               "PASS core_spine_smoke stress loops=%d run=%d ok=%d failed=%d\n",
               opt.loops,
               reporter.run_count(),
               reporter.ok_count(),
               reporter.failed_count());
  std::fflush(stdout);
  return 0;
#endif
}
