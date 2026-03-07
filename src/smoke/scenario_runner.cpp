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
#include "core/snapshot/state_snapshot.h"
#include "core/state_snapshot_buffer.h"
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
  bool trace_snapshot = false;
};

struct VerifyResult {
  bool ok = false;
  std::uint32_t actual_device_count = 0;
  std::uint32_t actual_stream_count = 0;
  std::uint64_t actual_version = 0;
  std::uint64_t actual_topology_version = 0;
  std::uint64_t actual_gen = 0;
  bool topology_changed = false;
  std::shared_ptr<const CamBANGStateSnapshot> snapshot;
};

void usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " <scenario_name> [--provider=synthetic|stub] [--trace-snapshot]\n"
      << "Scenarios:\n"
      << "  stream_lifecycle   (synthetic, stub)\n"
      << "  topology_lifecycle (synthetic, stub)\n"
      << "  stop_fact_error    (stub only)\n";
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

ScenarioProviderMask provider_mask_for_name(const std::string& provider_name) {
  if (provider_name == "synthetic") {
    return ScenarioProviderMask::Synthetic;
  }
  if (provider_name == "stub") {
    return ScenarioProviderMask::Stub;
  }
  return ScenarioProviderMask::None;
}

bool parse_opts(int argc, char** argv, Options& out) {
  bool scenario_seen = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return false;
    }
    if (a == "--trace-snapshot") {
      out.trace_snapshot = true;
      continue;
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

void print_snapshot_trace(ScenarioTime time, const CamBANGStateSnapshot& s) {
  std::cout << "[snap] tick=" << time << " gen=" << s.gen << " ver=" << s.version
            << " topo=" << s.topology_version << " devices=" << s.devices.size()
            << " streams=" << s.streams.size() << " flowing=" << flowing_stream_count(s)
            << " ts=" << s.timestamp_ns << "\n";
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
    if (ev.type == ScenarioEventType::InjectStopError) {
      if (auto* stub = dynamic_cast<StubProvider*>(provider_.get())) {
        stub->emit_fact_stream_stopped(kStreamId, ProviderError::ERR_PROVIDER_FAILED);
        return true;
      }
      return false;
    }
    return false;
  }

  VerifyResult verify(const ScenarioExpectation& exp) {
    VerifyResult out{};

    const auto before = snapshot_copy(snapshot_buffer_);
    const std::uint64_t before_topology = before ? before->topology_version : 0;

    runtime_.request_publish();
    out.ok = wait_until([this, &exp, before_topology, &out]() {
      auto s = snapshot_copy(snapshot_buffer_);
      if (!s) {
        return false;
      }

      out.actual_device_count = static_cast<std::uint32_t>(s->devices.size());
      out.actual_stream_count = flowing_stream_count(*s);
      out.actual_version = s->version;
      out.actual_topology_version = s->topology_version;
      out.actual_gen = s->gen;
      out.topology_changed = s->topology_version > before_topology;
      out.snapshot = s;

      if (out.actual_device_count != exp.device_count) {
        return false;
      }
      if (out.actual_stream_count != exp.stream_count) {
        return false;
      }
      if (exp.require_topology_change && !out.topology_changed) {
        return false;
      }
      return true;
    });

    if (!out.snapshot) {
      out.snapshot = snapshot_copy(snapshot_buffer_);
      if (out.snapshot) {
        out.actual_device_count = static_cast<std::uint32_t>(out.snapshot->devices.size());
        out.actual_stream_count = flowing_stream_count(*out.snapshot);
        out.actual_version = out.snapshot->version;
        out.actual_topology_version = out.snapshot->topology_version;
        out.actual_gen = out.snapshot->gen;
        out.topology_changed = out.snapshot->topology_version > before_topology;
      }
    }

    return out;
  }

private:
  std::string provider_name_;
  std::string hardware_id_;
  CoreRuntime runtime_;
  StateSnapshotBuffer snapshot_buffer_;
  std::unique_ptr<ICameraProvider> provider_;
};

int run_scenario(const Scenario& scenario, const Options& opt) {
  const auto provider_mask = provider_mask_for_name(opt.provider_name);
  if (!provider_mask_contains(scenario.provider_mask, provider_mask)) {
    std::cerr << "Scenario '" << scenario.name << "' does not support provider='" << opt.provider_name << "'\n";
    return 1;
  }

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
      if (ev.at != time) {
        continue;
      }
      if (!runtime.apply(ev)) {
        std::cerr << "tick " << time << " FAILED: event apply failed\n";
        shutdown();
        return 1;
      }
    }

    for (const auto& exp : scenario.expectations) {
      if (exp.at != time) {
        continue;
      }

      const VerifyResult vr = runtime.verify(exp);
      if (opt.trace_snapshot && vr.snapshot) {
        print_snapshot_trace(time, *vr.snapshot);
      }
      if (!vr.ok) {
        std::cerr << "tick " << time << " FAILED\n"
                  << "expected: device_count=" << exp.device_count
                  << " stream_count=" << exp.stream_count
                  << " topology_changed=" << (exp.require_topology_change ? 1 : 0) << "\n"
                  << "actual:   device_count=" << vr.actual_device_count
                  << " stream_count=" << vr.actual_stream_count
                  << " topology_changed=" << (vr.topology_changed ? 1 : 0)
                  << " gen=" << vr.actual_gen
                  << " version=" << vr.actual_version
                  << " topology_version=" << vr.actual_topology_version << "\n";
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
