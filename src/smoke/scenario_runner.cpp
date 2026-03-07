#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "scenario_runner: build with -DCAMBANG_INTERNAL_SMOKE=1 (via SCons: smoke=1)."
#endif

#include "core/core_runtime.h"
#include "core/state_snapshot_buffer.h"
#include "core/snapshot/state_snapshot.h"
#include "imaging/stub/provider.h"
#include "imaging/synthetic/provider.h"
#include "smoke/scenario/scenario_catalog.h"

using namespace cambang;

namespace {

constexpr std::uint64_t kDeviceInstanceId = 1;
constexpr std::uint64_t kRootId = 1;
constexpr std::uint64_t kStreamId = 1;
constexpr std::uint64_t kFramePeriodNs = 33'333'333ull;

struct Options {
  std::string scenario_name = "stream_lifecycle";
  std::string provider_name = "synthetic";
};

void usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " <scenario_name> [--provider=synthetic|stub]\n"
      << "Scenarios:\n"
      << "  stream_lifecycle\n";
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

bool parse_opts(int argc, char** argv, Options& out) {
  bool scenario_seen = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return false;
    }
    if (starts_with(a, "--provider=")) {
      out.provider_name = a.substr(std::string("--provider=").size());
      continue;
    }
    if (!scenario_seen && !a.empty() && a[0] != '-') {
      out.scenario_name = a;
      scenario_seen = true;
      continue;
    }
    std::cerr << "Unknown arg: " << a << "\n";
    usage(argv[0]);
    return false;
  }
  if (out.provider_name != "synthetic" && out.provider_name != "stub") {
    std::cerr << "Unsupported provider: " << out.provider_name << "\n";
    return false;
  }
  return true;
}

bool wait_until(const std::function<bool()>& pred, int max_iters = 400, int sleep_ms = 5) {
  for (int i = 0; i < max_iters; ++i) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return false;
}

std::shared_ptr<const CamBANGStateSnapshot> snapshot_copy(StateSnapshotBuffer& buf) {
  return buf.snapshot_copy();
}

std::uint32_t flowing_stream_count(const CamBANGStateSnapshot& s) {
  std::uint32_t count = 0;
  for (const auto& stream : s.streams) {
    if (stream.mode == CBStreamMode::FLOWING) {
      ++count;
    }
  }
  return count;
}

class ScenarioRuntime final {
public:
  explicit ScenarioRuntime(const std::string& provider_name) : provider_name_(provider_name) {}

  bool start() {
    runtime_.set_snapshot_publisher(&snapshot_buffer_);
    if (!runtime_.start()) {
      std::cerr << "FAIL: runtime start\n";
      return false;
    }

    // Observe the runtime baseline before provider initialization can advance
    // the latest-only snapshot beyond the baseline version.
    if (!wait_until([this]() {
          auto s = snapshot_copy(snapshot_buffer_);
          return static_cast<bool>(s);
        })) {
      std::cerr << "FAIL: baseline snapshot not observed\n";
      stop();
      return false;
    }

    if (provider_name_ == "synthetic") {
      SyntheticProviderConfig cfg{};
      cfg.endpoint_count = 1;
      cfg.nominal.width = 64;
      cfg.nominal.height = 64;
      cfg.nominal.format_fourcc = FOURCC_RGBA;
      provider_ = std::make_unique<SyntheticProvider>(cfg);
      hardware_id_ = "synthetic:0";
    } else {
      provider_ = std::make_unique<StubProvider>();
      hardware_id_ = "stub0";
    }

    runtime_.attach_provider(provider_.get());
    if (!provider_->initialize(runtime_.provider_callbacks()).ok()) {
      std::cerr << "FAIL: provider initialize\n";
      stop();
      return false;
    }

    return true;
  }

  void stop() {
    // Let CoreRuntime own deterministic shutdown while the attached provider
    // object is still valid.
    runtime_.stop();
    runtime_.attach_provider(nullptr);
    provider_.reset();
  }

  bool apply(const ScenarioEvent& ev) {
    if (ev.type == ScenarioEventType::OpenDevice) {
      return provider_->open_device(hardware_id_, kDeviceInstanceId, kRootId).ok();
    }
    if (ev.type == ScenarioEventType::CloseDevice) {
      return provider_->close_device(kDeviceInstanceId).ok();
    }
    if (ev.type == ScenarioEventType::CreateStream) {
      return runtime_.try_create_stream(kStreamId, kDeviceInstanceId, StreamIntent::PREVIEW, nullptr, nullptr, 1) ==
             TryCreateStreamStatus::OK;
    }
    if (ev.type == ScenarioEventType::StartStream) {
      return runtime_.try_start_stream(kStreamId) == TryStartStreamStatus::OK;
    }
    if (ev.type == ScenarioEventType::StopStream) {
      return runtime_.try_stop_stream(kStreamId) == TryStopStreamStatus::OK;
    }
    if (ev.type == ScenarioEventType::DestroyStream) {
      return runtime_.try_destroy_stream(kStreamId) == TryDestroyStreamStatus::OK;
    }
    if (ev.type == ScenarioEventType::EmitFrame) {
      if (auto* syn = dynamic_cast<SyntheticProvider*>(provider_.get())) {
        syn->advance(kFramePeriodNs);
        return true;
      }
      if (auto* stub = dynamic_cast<StubProvider*>(provider_.get())) {
        stub->advance(kFramePeriodNs);
        return true;
      }
      return false;
    }
    return false;
  }

  bool verify(const ScenarioExpectation& exp) {
    runtime_.request_publish();
    return wait_until([this, &exp]() {
      auto s = snapshot_copy(snapshot_buffer_);
      return s &&
             s->devices.size() == exp.device_count &&
             flowing_stream_count(*s) == exp.stream_count;
    });
  }

private:
  std::string provider_name_;
  std::string hardware_id_;
  CoreRuntime runtime_;
  StateSnapshotBuffer snapshot_buffer_;
  std::unique_ptr<ICameraProvider> provider_;
};

int run_scenario(const Scenario& scenario, const Options& opt) {
  ScenarioRuntime runtime(opt.provider_name);
  if (!runtime.start()) {
    runtime.stop();
    return 1;
  }

  const auto shutdown = [&runtime]() { runtime.stop(); };

  ScenarioTime max_time = 0;
  for (const auto& ev : scenario.events) max_time = std::max(max_time, ev.at);
  for (const auto& exp : scenario.expectations) max_time = std::max(max_time, exp.at);

  for (ScenarioTime time = 0; time <= max_time; ++time) {
    for (const auto& ev : scenario.events) {
      if (ev.at != time) continue;
      if (!runtime.apply(ev)) {
        std::cerr << "tick " << time << " FAILED: event apply failed\n";
        shutdown();
        return 1;
      }
    }

    for (const auto& exp : scenario.expectations) {
      if (exp.at != time) continue;
      if (!runtime.verify(exp)) {
        std::cerr << "tick " << time << " FAILED: expected devices=" << exp.device_count
                  << " streams=" << exp.stream_count << "\n";
        shutdown();
        return 1;
      }
      std::cout << "tick " << time << " OK\n";
    }
  }

  shutdown();
  std::cout << "Scenario PASSED\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_opts(argc, argv, opt)) {
    return 1;
  }

  Scenario scenario("invalid");
  if (!load_scenario(opt.scenario_name, scenario)) {
    std::cerr << "Unknown scenario: " << opt.scenario_name << "\n";
    return 1;
  }

  return run_scenario(scenario, opt);
}
