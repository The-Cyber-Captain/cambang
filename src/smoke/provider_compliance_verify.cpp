// Deterministic provider compliance verifier: provider contract only.
// This tool intentionally uses provider callbacks, retained snapshots,
// and deterministic timeline dispatch observations as PASS/FAIL evidence.
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "imaging/broker/provider_broker.h"
#include "imaging/stub/provider.h"
#include "imaging/synthetic/builtin_scenario_library.h"
#include "imaging/synthetic/provider.h"
#include "imaging/synthetic/scenario_loader.h"
#include "imaging/synthetic/scenario_model.h"
#include "imaging/api/timeline_teardown_trace.h"
#include "smoke/verify_case/verify_case_harness.h"

using namespace cambang;

namespace {

struct Options {
  std::string external_scenario_file;
};

constexpr int kMaxIters = 500;
constexpr int kSleepMs = 5;

constexpr uint64_t kLifecycleDeviceId = 9101;
constexpr uint64_t kLifecycleRootId = 9102;
constexpr uint64_t kLifecycleStreamId = 9103;

constexpr uint64_t kClusteredDeviceId = 121;
constexpr uint64_t kClusteredRootId = 12201;
constexpr uint64_t kClusteredStreamId = 122;


void flush_timeline_teardown_trace(std::ostream& os = std::cerr) {
  std::string line;
  while (timeline_teardown_trace_try_pop(line)) {
    os << line << "\n";
  }
}

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

void usage(const char* argv0) {
  std::cerr << "Usage: " << argv0 << " [--external_scenario_file=<path>]\n";
}

bool parse_opts(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return false;
    }
    if (starts_with(a, "--external_scenario_file=")) {
      opt.external_scenario_file = a.substr(std::string("--external_scenario_file=").size());
      continue;
    }
    std::cerr << "Unknown arg: " << a << "\n";
    usage(argv[0]);
    return false;
  }
  return true;
}

struct EventRec {
  std::string tag;
  uint64_t id = 0;
  uint32_t type = 0;
  uint64_t owner_stream_id = 0;
  uint32_t pixel_sig = 0;
  CaptureTimestamp ts{};
};

struct RecorderCallbacks final : IProviderCallbacks {
  uint64_t next_native_id = 1;
  std::vector<EventRec> events;

  uint64_t allocate_native_id(NativeObjectType) override { return next_native_id++; }
  uint64_t core_monotonic_now_ns() override { return 0; }

  void on_device_opened(uint64_t id) override { events.push_back({"device_opened", id}); }
  void on_device_closed(uint64_t id) override { events.push_back({"device_closed", id}); }
  void on_stream_created(uint64_t id) override { events.push_back({"stream_created", id}); }
  void on_stream_destroyed(uint64_t id) override { events.push_back({"stream_destroyed", id}); }
  void on_stream_started(uint64_t id) override { events.push_back({"stream_started", id}); }
  void on_stream_stopped(uint64_t id, ProviderError) override { events.push_back({"stream_stopped", id}); }
  void on_capture_started(uint64_t id) override { events.push_back({"capture_started", id}); }
  void on_capture_completed(uint64_t id) override { events.push_back({"capture_completed", id}); }
  void on_capture_failed(uint64_t id, ProviderError) override { events.push_back({"capture_failed", id}); }

  void on_frame(const FrameView& frame) override {
    EventRec ev{"frame", 0};
    ev.ts = frame.capture_timestamp;
    if (frame.data && frame.size_bytes >= 4) {
      const uint8_t* p = static_cast<const uint8_t*>(frame.data);
      ev.pixel_sig = static_cast<uint32_t>(p[0]) |
                     (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16) |
                     (static_cast<uint32_t>(p[3]) << 24);
    }
    events.push_back(ev);
    if (frame.release) {
      frame.release(frame.release_user, &frame);
    }
  }

  void on_device_error(uint64_t id, ProviderError) override { events.push_back({"device_error", id}); }
  void on_stream_error(uint64_t id, ProviderError) override { events.push_back({"stream_error", id}); }
  void on_native_object_created(const NativeObjectCreateInfo& info) override {
    events.push_back({"native_created", info.native_id, info.type, info.owner_stream_id});
  }
  void on_native_object_destroyed(const NativeObjectDestroyInfo& info) override {
    events.push_back({"native_destroyed", info.native_id});
  }
};

int find_event_index(const std::vector<EventRec>& events, const char* tag, uint64_t id) {
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].tag == tag && events[i].id == id) return static_cast<int>(i);
  }
  return -1;
}

int find_native_create_id(const std::vector<EventRec>& events, uint32_t type, uint64_t owner_stream_id) {
  for (const auto& e : events) {
    if (e.tag == "native_created" && e.type == type && e.owner_stream_id == owner_stream_id) {
      return static_cast<int>(e.id);
    }
  }
  return -1;
}

int find_frame_index_by_ts(const std::vector<EventRec>& events, uint64_t ts_ns) {
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].tag == "frame" && events[i].ts.value == ts_ns) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool assert_native_balance(const std::vector<EventRec>& events, const char* name) {
  int created = 0;
  int destroyed = 0;
  for (const auto& e : events) {
    if (e.tag == "native_created") ++created;
    if (e.tag == "native_destroyed") ++destroyed;
  }
  if (created != destroyed) {
    std::cerr << "FAIL " << name << " native create/destroy mismatch\n";
    return false;
  }
  return true;
}

bool wait_for_snapshot_with_progress(VerifyCaseHarness& harness,
                                     SyntheticProvider& synthetic,
                                     uint64_t advance_step_ns,
                                     const std::function<bool(const CamBANGStateSnapshot&)>& predicate,
                                     std::string& error,
                                     const char* timeout_msg,
                                     int max_iters = kMaxIters,
                                     int sleep_ms = kSleepMs) {
  for (int i = 0; i < max_iters; ++i) {
    synthetic.advance(advance_step_ns);
    harness.runtime().request_publish();
    flush_timeline_teardown_trace();
    auto snap = harness.snapshot_buffer().snapshot_copy();
    if (snap && predicate(*snap)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  error = timeout_msg;
  return false;
}

SyntheticTimelineScenario build_clustered_destructive_scenario(uint64_t period_ns) {
  SyntheticTimelineScenario scenario{};
  SyntheticScheduledEvent ev{};

  ev.at_ns = 0;
  ev.type = SyntheticEventType::OpenDevice;
  ev.endpoint_index = 0;
  ev.device_instance_id = kClusteredDeviceId;
  ev.root_id = kClusteredRootId;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::CreateStream;
  ev.device_instance_id = kClusteredDeviceId;
  ev.stream_id = kClusteredStreamId;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::StartStream;
  ev.stream_id = kClusteredStreamId;
  scenario.events.push_back(ev);

  // Shared clustered destructive boundary.
  ev = {};
  ev.at_ns = period_ns * 2;
  ev.type = SyntheticEventType::StopStream;
  ev.stream_id = kClusteredStreamId;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns * 2;
  ev.type = SyntheticEventType::DestroyStream;
  ev.stream_id = kClusteredStreamId;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns * 2;
  ev.type = SyntheticEventType::CloseDevice;
  ev.device_instance_id = kClusteredDeviceId;
  scenario.events.push_back(ev);

  return scenario;
}

// ===== Family A: Scenario materialization / loader compliance =====

bool run_synthetic_scenario_materialization_check() {
  SyntheticCanonicalScenario canonical{};

  SyntheticScenarioDeviceDeclaration d{};
  d.key = "cam_a";
  d.endpoint_index = 0;
  canonical.devices.push_back(d);

  SyntheticScenarioStreamDeclaration s{};
  s.key = "preview_a";
  s.device_key = "cam_a";
  s.intent = StreamIntent::PREVIEW;
  s.baseline_capture_profile.width = 64;
  s.baseline_capture_profile.height = 64;
  s.baseline_capture_profile.format_fourcc = FOURCC_RGBA;
  s.baseline_capture_profile.target_fps_min = 30;
  s.baseline_capture_profile.target_fps_max = 30;
  canonical.streams.push_back(s);

  SyntheticScenarioTimelineAction a{};
  a.at_ns = 10;
  a.type = SyntheticEventType::StartStream;
  a.stream_key = "preview_a";
  canonical.timeline.push_back(a);

  a = {};
  a.at_ns = 0;
  a.type = SyntheticEventType::OpenDevice;
  a.device_key = "cam_a";
  canonical.timeline.push_back(a);

  a = {};
  a.at_ns = 0;
  a.type = SyntheticEventType::CreateStream;
  a.stream_key = "preview_a";
  canonical.timeline.push_back(a);

  a = {};
  a.at_ns = 20;
  a.type = SyntheticEventType::StopStream;
  a.stream_key = "preview_a";
  canonical.timeline.push_back(a);

  SyntheticScenarioMaterializationOptions opts{};
  opts.device_instance_id_base = 200;
  opts.root_id_base = 400;
  opts.stream_id_base = 600;

  SyntheticScenarioMaterializationResult result{};
  std::string error;
  if (!materialize_synthetic_canonical_scenario(canonical, opts, result, &error)) {
    std::cerr << "FAIL scenario materialization rejected valid canonical scenario: " << error << "\n";
    return false;
  }

  if (result.devices.size() != 1 || result.streams.size() != 1 || result.executable_schedule.events.size() != 4) {
    std::cerr << "FAIL scenario materialization unexpected output sizes\n";
    return false;
  }
  if (result.devices[0].device_instance_id != 201 || result.devices[0].root_id != 401 ||
      result.streams[0].stream_id != 601 || result.streams[0].device_instance_id != 201) {
    std::cerr << "FAIL scenario materialization unstable id mapping\n";
    return false;
  }

  const auto& events = result.executable_schedule.events;
  const std::vector<SyntheticEventType> expected{
      SyntheticEventType::OpenDevice,
      SyntheticEventType::CreateStream,
      SyntheticEventType::StartStream,
      SyntheticEventType::StopStream,
  };
  for (size_t i = 0; i < expected.size(); ++i) {
    if (events[i].type != expected[i]) {
      std::cerr << "FAIL scenario materialization did not stable-order by at_ns\n";
      return false;
    }
  }

  return true;
}

bool run_synthetic_builtin_scenario_library_build_check() {
  CaptureProfile baseline{};
  baseline.width = 64;
  baseline.height = 64;
  baseline.format_fourcc = FOURCC_RGBA;
  baseline.target_fps_min = 30;
  baseline.target_fps_max = 30;

  const SyntheticBuiltinScenarioLibraryId ids[] = {
      SyntheticBuiltinScenarioLibraryId::StreamLifecycleVersions,
      SyntheticBuiltinScenarioLibraryId::TopologyChangeVersions,
      SyntheticBuiltinScenarioLibraryId::PublicationCoalescing,
  };

  for (SyntheticBuiltinScenarioLibraryId id : ids) {
    SyntheticCanonicalScenario canonical{};
    std::string error;
    if (!build_synthetic_builtin_scenario_library_canonical_scenario(id, baseline, canonical, &error)) {
      std::cerr << "FAIL builtin scenario library build failed for "
                << synthetic_builtin_scenario_library_name(id) << ": " << error << "\n";
      return false;
    }
    if (canonical.devices.empty() || canonical.streams.empty() || canonical.timeline.empty()) {
      std::cerr << "FAIL builtin scenario library produced empty scenario for "
                << synthetic_builtin_scenario_library_name(id) << "\n";
      return false;
    }
  }

  return true;
}

bool run_synthetic_external_scenario_loader_check() {
  const std::string json = R"JSON(
{
  "schema_version": 1,
  "devices": [
    { "key": "builtin_device", "endpoint_index": 0 }
  ],
  "streams": [
    {
      "key": "builtin_main_stream",
      "device_key": "builtin_device",
      "intent": "PREVIEW",
      "capture_profile": {
        "width": 64,
        "height": 64,
        "format_fourcc": 1094862674,
        "target_fps_min": 30,
        "target_fps_max": 30
      }
    }
  ],
  "timeline": [
    { "at_ns": 0, "type": "OpenDevice", "device_key": "builtin_device" },
    { "at_ns": 0, "type": "CreateStream", "stream_key": "builtin_main_stream" },
    { "at_ns": 0, "type": "StartStream", "stream_key": "builtin_main_stream" },
    {
      "at_ns": 15000000,
      "type": "UpdateStreamPicture",
      "stream_key": "builtin_main_stream",
      "picture": {
        "preset": "checker",
        "seed": 3,
        "overlay_frame_index_offsets": false,
        "overlay_moving_bar": true,
        "solid_r": 0,
        "solid_g": 0,
        "solid_b": 0,
        "solid_a": 255,
        "checker_size_px": 12
      }
    },
    { "at_ns": 60000000, "type": "StopStream", "stream_key": "builtin_main_stream" },
    { "at_ns": 60000001, "type": "DestroyStream", "stream_key": "builtin_main_stream" },
    { "at_ns": 60000002, "type": "CloseDevice", "device_key": "builtin_device" }
  ]
}
)JSON";

  SyntheticCanonicalScenario loaded{};
  std::string error;
  if (!load_synthetic_canonical_scenario_from_json_text(json, loaded, &error)) {
    std::cerr << "FAIL external loader parse/validate/convert failed: " << error << "\n";
    return false;
  }

  SyntheticScenarioMaterializationResult materialized{};
  if (!materialize_synthetic_canonical_scenario(loaded, {}, materialized, &error)) {
    std::cerr << "FAIL external loader materialization failed: " << error << "\n";
    return false;
  }

  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.synthetic_role = SyntheticRole::Timeline;
  cfg.timing_driver = TimingDriver::VirtualTime;
  cfg.endpoint_count = 1;
  cfg.nominal.fps_num = 30;
  cfg.nominal.fps_den = 1;

  SyntheticProvider synthetic(cfg);
  if (!synthetic.initialize(&cb).ok()) return false;

  std::vector<SyntheticEventType> dispatched;
  synthetic.set_timeline_request_dispatch_hook_for_host([&dispatched](const SyntheticScheduledEvent& ev) {
    dispatched.push_back(ev.type);
  });

  if (!synthetic.load_timeline_canonical_scenario_from_json_text_for_host(json, &error).ok()) {
    std::cerr << "FAIL external loader provider-facing load failed: " << error << "\n";
    (void)synthetic.shutdown();
    return false;
  }
  if (!synthetic.start_timeline_scenario_for_host().ok()) {
    (void)synthetic.shutdown();
    return false;
  }

  synthetic.advance(60'000'002);
  const std::vector<SyntheticEventType> expected{
      SyntheticEventType::OpenDevice,
      SyntheticEventType::CreateStream,
      SyntheticEventType::StartStream,
      SyntheticEventType::UpdateStreamPicture,
      SyntheticEventType::StopStream,
      SyntheticEventType::DestroyStream,
      SyntheticEventType::CloseDevice,
  };
  if (dispatched != expected) {
    std::cerr << "FAIL external loader authored/materialized/dispatched mismatch\n";
    (void)synthetic.shutdown();
    return false;
  }

  return synthetic.shutdown().ok();
}

bool run_synthetic_external_scenario_loader_negative_check() {
  struct NegativeCase {
    const char* name;
    const char* json;
    const char* error_hint;
  };

  const std::vector<NegativeCase> cases{
      {"unknown_top_level_field", R"JSON({"schema_version":1,"devices":[],"streams":[],"timeline":[],"extra":1})JSON", "unknown field"},
      {"missing_required_top_level_field", R"JSON({"schema_version":1,"devices":[],"streams":[]})JSON", "missing required field"},
      {"wrong_type_required_field", R"JSON({"schema_version":"1","devices":[],"streams":[],"timeline":[]})JSON", "wrong type"},
      {"unknown_action_type", R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"DoThing","stream_key":"stream0"}]})JSON", "type is unknown"},
      {"emit_frame_rejected", R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"EmitFrame","stream_key":"stream0"}]})JSON", "EmitFrame"},
      {"stream_device_key_unknown", R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"missing_cam","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[]})JSON", "unknown device key"},
      {"timeline_unknown_stream_key", R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"StartStream","stream_key":"missing_stream"}]})JSON", "unknown stream key"},
      {"schema_version_not_one", R"JSON({"schema_version":2,"devices":[],"streams":[],"timeline":[]})JSON", "schema_version"},
  };

  for (const auto& c : cases) {
    SyntheticCanonicalScenario out{};
    std::string error;
    if (load_synthetic_canonical_scenario_from_json_text(c.json, out, &error)) {
      std::cerr << "FAIL external loader negative unexpectedly succeeded: " << c.name << "\n";
      return false;
    }
    if (error.find(c.error_hint) == std::string::npos) {
      std::cerr << "FAIL external loader negative mismatch: " << c.name
                << " error=" << error << " expected_hint=" << c.error_hint << "\n";
      return false;
    }
  }

  return true;
}

bool run_external_scenario_file_execution_check(const std::string& path) {
  SyntheticCanonicalScenario canonical{};
  std::string error;
  if (!load_synthetic_canonical_scenario_from_json_file(path, canonical, &error)) {
    std::cerr << "FAIL external scenario file pre-load failed: " << error << "\n";
    return false;
  }

  SyntheticScenarioMaterializationResult materialized{};
  if (!materialize_synthetic_canonical_scenario(canonical, {}, materialized, &error)) {
    std::cerr << "FAIL external scenario file materialization failed: " << error << "\n";
    return false;
  }

  std::vector<SyntheticEventType> expected_dispatch;
  uint64_t max_at_ns = 0;
  expected_dispatch.reserve(materialized.executable_schedule.events.size());
  for (const auto& ev : materialized.executable_schedule.events) {
    if (ev.at_ns > max_at_ns) max_at_ns = ev.at_ns;
    if (ev.type != SyntheticEventType::EmitFrame) {
      expected_dispatch.push_back(ev.type);
    }
  }

  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.synthetic_role = SyntheticRole::Timeline;
  cfg.timing_driver = TimingDriver::VirtualTime;
  cfg.endpoint_count = 1;
  cfg.nominal.fps_num = 30;
  cfg.nominal.fps_den = 1;

  SyntheticProvider synthetic(cfg);
  if (!synthetic.initialize(&cb).ok()) return false;

  std::vector<SyntheticEventType> dispatched;
  synthetic.set_timeline_request_dispatch_hook_for_host([&dispatched](const SyntheticScheduledEvent& ev) {
    dispatched.push_back(ev.type);
  });

  if (!synthetic.load_timeline_canonical_scenario_from_json_file_for_host(path, &error).ok()) {
    std::cerr << "FAIL external scenario file provider load+stage failed: " << error << "\n";
    (void)synthetic.shutdown();
    return false;
  }
  if (!dispatched.empty()) {
    std::cerr << "FAIL external scenario file dispatched before explicit start\n";
    (void)synthetic.shutdown();
    return false;
  }

  if (!synthetic.start_timeline_scenario_for_host().ok()) {
    (void)synthetic.shutdown();
    return false;
  }
  synthetic.advance(max_at_ns + 1);

  if (dispatched != expected_dispatch) {
    std::cerr << "FAIL external scenario file dispatch/order mismatch\n";
    (void)synthetic.shutdown();
    return false;
  }

  return synthetic.shutdown().ok();
}

// ===== Family B: Primitive lifecycle compliance (foundational) =====

bool run_synthetic_primitive_lifecycle_foundation_check() {
  VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
  harness.set_callback_diagnostics_enabled(true);

  std::string error;
  if (!harness.start_runtime(error)) {
    std::cerr << "FAIL primitive lifecycle harness start: " << error << "\n";
    return false;
  }

  if (!harness.open_device_id(kLifecycleDeviceId, 0, kLifecycleRootId, error) ||
      !harness.create_stream_id(kLifecycleStreamId, kLifecycleDeviceId, 1, error) ||
      !harness.start_stream_id(kLifecycleStreamId, error) ||
      !harness.stop_stream_id(kLifecycleStreamId, error) ||
      !harness.destroy_stream_id(kLifecycleStreamId, error) ||
      !harness.close_device_id(kLifecycleDeviceId, error)) {
    std::cerr << "FAIL primitive lifecycle action failed: " << error << "\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  if (!harness.wait_for_core_snapshot(
          [&](const CamBANGStateSnapshot& s) {
            return !VerifyCaseHarness::has_stream(s, kLifecycleStreamId) &&
                   !VerifyCaseHarness::has_device(s, kLifecycleDeviceId);
          },
          error,
          kMaxIters,
          kSleepMs,
          "timed out waiting for primitive lifecycle final absence")) {
    std::cerr << "FAIL primitive lifecycle final truth failed: " << error << "\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  const int opened = harness.find_recorded_callback_index("device_opened", kLifecycleDeviceId);
  const int created = harness.find_recorded_callback_index("stream_created", kLifecycleStreamId);
  const int started = harness.find_recorded_callback_index("stream_started", kLifecycleStreamId);
  const int stopped = harness.find_recorded_callback_index("stream_stopped", kLifecycleStreamId);
  const int destroyed = harness.find_recorded_callback_index("stream_destroyed", kLifecycleStreamId);
  const int closed = harness.find_recorded_callback_index("device_closed", kLifecycleDeviceId);

  if (opened < 0 || created < 0 || started < 0 || stopped < 0 || destroyed < 0 || closed < 0) {
    std::cerr << "FAIL primitive lifecycle missing callback evidence\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }
  if (!(opened < created && created < started && started < stopped && stopped < destroyed && destroyed < closed)) {
    std::cerr << "FAIL primitive lifecycle callback order mismatch\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  flush_timeline_teardown_trace();
  harness.stop_runtime();
  return true;
}

// ===== Family C: Clustered destructive sequencing interpretation =====

bool run_clustered_strict_branch_check() {
  VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
  std::string error;
  if (!harness.start_runtime(error)) {
    std::cerr << "FAIL clustered strict harness start: " << error << "\n";
    return false;
  }

  auto* synthetic = dynamic_cast<SyntheticProvider*>(harness.runtime().attached_provider());
  if (!synthetic) {
    std::cerr << "FAIL clustered strict provider cast failed\n";
    flush_timeline_teardown_trace();
    harness.stop_runtime();
    return false;
  }

  const uint64_t period_ns = 1'000'000'000ull / 30ull;
  if (!synthetic->set_completion_gated_destructive_sequencing_for_host(false).ok() ||
      !synthetic->set_timeline_scenario_for_host(build_clustered_destructive_scenario(period_ns)).ok() ||
      !synthetic->start_timeline_scenario_for_host().ok()) {
    std::cerr << "FAIL clustered strict setup failed\n";
    flush_timeline_teardown_trace();
    harness.stop_runtime();
    return false;
  }

  if (!wait_for_snapshot_with_progress(
          harness,
          *synthetic,
          0,
          [&](const CamBANGStateSnapshot& s) {
            const auto* stream = VerifyCaseHarness::find_stream(s, kClusteredStreamId);
            return VerifyCaseHarness::has_device(s, kClusteredDeviceId) &&
                   stream && stream->mode == CBStreamMode::FLOWING;
          },
          error,
          "timed out waiting for clustered strict precondition")) {
    std::cerr << "FAIL clustered strict precondition failed: " << error << "\n";
    flush_timeline_teardown_trace();
    harness.stop_runtime();
    return false;
  }

  harness.clear_recorded_callbacks();
  const size_t strict_watermark = harness.recorded_callback_count();

  synthetic->advance(period_ns * 2);
  harness.runtime().request_publish();
  flush_timeline_teardown_trace();

  const auto strict_progress = [&]() {
    synthetic->advance(1);
    harness.runtime().request_publish();
    flush_timeline_teardown_trace();
  };

  if (!harness.wait_for_recorded_callback_with_progress(
          "stream_stopped",
          kClusteredStreamId,
          strict_watermark,
          strict_progress,
          error,
          kMaxIters,
          kSleepMs,
          "timed out waiting for clustered strict stop callback")) {
    std::cerr << "FAIL clustered strict missing stop callback: " << error << "\n";
    flush_timeline_teardown_trace();
    harness.stop_runtime();
    return false;
  }

  enum class StrictOutcome {
    Pending,
    RetainedStopped,
    FullyTornDown,
  };

  auto classify_strict_outcome = [&]() -> StrictOutcome {
    const int destroyed_idx = harness.find_recorded_callback_index_after(
        "stream_destroyed", kClusteredStreamId, strict_watermark);
    const int closed_idx = harness.find_recorded_callback_index_after(
        "device_closed", kClusteredDeviceId, strict_watermark);

    auto snap = harness.snapshot_buffer().snapshot_copy();
    const bool has_stream = snap && VerifyCaseHarness::has_stream(*snap, kClusteredStreamId);
    const bool has_device = snap && VerifyCaseHarness::has_device(*snap, kClusteredDeviceId);
    const auto* stream = (snap ? VerifyCaseHarness::find_stream(*snap, kClusteredStreamId) : nullptr);

    const bool retained_stopped =
        has_stream && has_device && stream && stream->mode == CBStreamMode::STOPPED &&
        destroyed_idx < 0 && closed_idx < 0;

    const bool fully_torn_down =
        destroyed_idx >= 0 && closed_idx >= 0 && destroyed_idx < closed_idx &&
        !has_stream && !has_device;

    if (fully_torn_down) return StrictOutcome::FullyTornDown;
    if (retained_stopped) return StrictOutcome::RetainedStopped;
    return StrictOutcome::Pending;
  };

  StrictOutcome strict_outcome = StrictOutcome::Pending;
  for (int i = 0; i < kMaxIters; ++i) {
    strict_outcome = classify_strict_outcome();
    if (strict_outcome != StrictOutcome::Pending) break;
    strict_progress();
  }

  if (strict_outcome == StrictOutcome::Pending) {
    const int destroyed_idx = harness.find_recorded_callback_index_after(
        "stream_destroyed", kClusteredStreamId, strict_watermark);
    const int closed_idx = harness.find_recorded_callback_index_after(
        "device_closed", kClusteredDeviceId, strict_watermark);
    auto snap = harness.snapshot_buffer().snapshot_copy();
    const bool has_stream = snap && VerifyCaseHarness::has_stream(*snap, kClusteredStreamId);
    const bool has_device = snap && VerifyCaseHarness::has_device(*snap, kClusteredDeviceId);
    const auto* stream = (snap ? VerifyCaseHarness::find_stream(*snap, kClusteredStreamId) : nullptr);
    std::cerr << "FAIL clustered strict valid outcome missing"
              << " callback_indices=(destroyed=" << destroyed_idx
              << ", closed=" << closed_idx << ")"
              << " retained=(has_stream=" << (has_stream ? 1 : 0)
              << ", has_device=" << (has_device ? 1 : 0)
              << ", mode=" << (stream ? static_cast<int>(stream->mode) : -1)
              << ")\n";
    flush_timeline_teardown_trace();
    harness.stop_runtime();
    return false;
  }

  flush_timeline_teardown_trace();
  harness.stop_runtime();
  return true;
}

bool run_clustered_completion_gated_branch_check() {
  VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
  harness.set_callback_diagnostics_enabled(true);
  std::string error;
  if (!harness.start_runtime(error)) {
    std::cerr << "FAIL clustered gated harness start: " << error << "\n";
    return false;
  }
  auto* synthetic = dynamic_cast<SyntheticProvider*>(harness.runtime().attached_provider());
  if (!synthetic) {
    std::cerr << "FAIL clustered gated provider cast failed\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  const uint64_t period_ns = 1'000'000'000ull / 30ull;
  if (!synthetic->set_completion_gated_destructive_sequencing_for_host(true).ok() ||
      !synthetic->set_timeline_scenario_for_host(build_clustered_destructive_scenario(period_ns)).ok() ||
      !synthetic->start_timeline_scenario_for_host().ok()) {
    std::cerr << "FAIL clustered gated setup failed\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  auto progress = [&]() {
    synthetic->advance(1);
    harness.runtime().request_publish();
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  };

  if (!harness.wait_for_core_snapshot_with_progress(
          [&](const CamBANGStateSnapshot& s) {
            const auto* stream = VerifyCaseHarness::find_stream(s, kClusteredStreamId);
            return VerifyCaseHarness::has_device(s, kClusteredDeviceId) &&
                   stream && stream->mode == CBStreamMode::FLOWING;
          },
          progress,
          error,
          kMaxIters,
          kSleepMs,
          "timed out waiting for clustered gated startup realization")) {
    std::cerr << "FAIL clustered gated startup realization failed: " << error << "\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  int opened = harness.find_recorded_callback_index("device_opened", kClusteredDeviceId);
  int created = harness.find_recorded_callback_index("stream_created", kClusteredStreamId);
  int started = harness.find_recorded_callback_index("stream_started", kClusteredStreamId);
  if (opened < 0 || created < 0 || started < 0) {
    std::cerr << "DIAG clustered gated startup callback timeout expected stream_id=" << kClusteredStreamId
              << " device_id=" << kClusteredDeviceId
              << " callback_indices=(opened=" << opened
              << ", created=" << created
              << ", started=" << started << ")\n";
    std::cerr << "FAIL clustered gated startup callback evidence missing\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }
  if (!(opened < created && created < started)) {
    std::cerr << "FAIL clustered gated startup callback order mismatch\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  harness.clear_recorded_callbacks();
  const size_t teardown_watermark = harness.recorded_callback_count();
  synthetic->advance(period_ns * 2);
  harness.runtime().request_publish();
  flush_timeline_teardown_trace();

  int stopped = -1;
  int destroyed = -1;
  int closed = -1;

  if (!harness.wait_for_recorded_callback_with_progress("stream_stopped",
                                                        kClusteredStreamId,
                                                        teardown_watermark,
                                                        progress,
                                                        error,
                                                        kMaxIters * 2,
                                                        kSleepMs,
                                                        "timed out waiting for clustered gated stream_stopped")) {
    stopped = harness.find_recorded_callback_index_after("stream_stopped", kClusteredStreamId, teardown_watermark);
    destroyed = harness.find_recorded_callback_index_after("stream_destroyed", kClusteredStreamId, teardown_watermark);
    closed = harness.find_recorded_callback_index_after("device_closed", kClusteredDeviceId, teardown_watermark);
    std::cerr << "DIAG clustered gated callback wait timeout stage=stream_stopped expected stream_id=" << kClusteredStreamId
              << " device_id=" << kClusteredDeviceId
              << " callback_indices=(stopped=" << stopped
              << ", destroyed=" << destroyed
              << ", closed=" << closed << ")\n";
    std::cerr << "FAIL clustered gated missing callback evidence at stage stream_stopped\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  if (!harness.wait_for_recorded_callback_with_progress("stream_destroyed",
                                                        kClusteredStreamId,
                                                        teardown_watermark,
                                                        progress,
                                                        error,
                                                        kMaxIters * 2,
                                                        kSleepMs,
                                                        "timed out waiting for clustered gated stream_destroyed")) {
    stopped = harness.find_recorded_callback_index_after("stream_stopped", kClusteredStreamId, teardown_watermark);
    destroyed = harness.find_recorded_callback_index_after("stream_destroyed", kClusteredStreamId, teardown_watermark);
    closed = harness.find_recorded_callback_index_after("device_closed", kClusteredDeviceId, teardown_watermark);
    std::cerr << "DIAG clustered gated callback wait timeout stage=stream_destroyed expected stream_id=" << kClusteredStreamId
              << " device_id=" << kClusteredDeviceId
              << " callback_indices=(stopped=" << stopped
              << ", destroyed=" << destroyed
              << ", closed=" << closed << ")\n";
    std::cerr << "FAIL clustered gated missing callback evidence at stage stream_destroyed\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  if (!harness.wait_for_recorded_callback_with_progress("device_closed",
                                                        kClusteredDeviceId,
                                                        teardown_watermark,
                                                        progress,
                                                        error,
                                                        kMaxIters * 2,
                                                        kSleepMs,
                                                        "timed out waiting for clustered gated device_closed")) {
    stopped = harness.find_recorded_callback_index_after("stream_stopped", kClusteredStreamId, teardown_watermark);
    destroyed = harness.find_recorded_callback_index_after("stream_destroyed", kClusteredStreamId, teardown_watermark);
    closed = harness.find_recorded_callback_index_after("device_closed", kClusteredDeviceId, teardown_watermark);
    std::cerr << "DIAG clustered gated callback wait timeout stage=device_closed expected stream_id=" << kClusteredStreamId
              << " device_id=" << kClusteredDeviceId
              << " callback_indices=(stopped=" << stopped
              << ", destroyed=" << destroyed
              << ", closed=" << closed << ")\n";
    std::cerr << "FAIL clustered gated missing callback evidence at stage device_closed\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  stopped = harness.find_recorded_callback_index_after("stream_stopped", kClusteredStreamId, teardown_watermark);
  destroyed = harness.find_recorded_callback_index_after("stream_destroyed", kClusteredStreamId, teardown_watermark);
  closed = harness.find_recorded_callback_index_after("device_closed", kClusteredDeviceId, teardown_watermark);
  if (stopped < 0 || destroyed < 0 || closed < 0) {
    std::cerr << "FAIL clustered gated missing callback evidence after completion\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }
  if (!(stopped < destroyed && destroyed < closed)) {
    std::cerr << "FAIL clustered gated callback order mismatch\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  if (!harness.wait_for_core_snapshot_with_progress(
          [&](const CamBANGStateSnapshot& s) {
            return !VerifyCaseHarness::has_stream(s, kClusteredStreamId) &&
                   !VerifyCaseHarness::has_device(s, kClusteredDeviceId);
          },
          progress,
          error,
          kMaxIters * 2,
          kSleepMs,
          "timed out waiting for clustered gated teardown convergence")) {
    stopped = harness.find_recorded_callback_index_after("stream_stopped", kClusteredStreamId, teardown_watermark);
    destroyed = harness.find_recorded_callback_index_after("stream_destroyed", kClusteredStreamId, teardown_watermark);
    closed = harness.find_recorded_callback_index_after("device_closed", kClusteredDeviceId, teardown_watermark);
    std::cerr << "DIAG clustered gated teardown convergence timeout expected stream_id=" << kClusteredStreamId
              << " device_id=" << kClusteredDeviceId
              << " callback_indices=(stopped=" << stopped
              << ", destroyed=" << destroyed
              << ", closed=" << closed << ")\n";
    std::cerr << "FAIL clustered gated teardown did not converge: " << error << "\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  harness.stop_runtime();
    return false;
  }

  flush_timeline_teardown_trace();
  harness.stop_runtime();
  return true;
}

// ===== Family D: Broker / host surface compliance =====

bool run_broker_timeline_host_surface_check() {
  if (!ProviderBroker::check_mode_supported_in_build(RuntimeMode::synthetic).ok()) {
    std::cout << "SKIP broker timeline host surface: synthetic mode not built\n";
    return true;
  }

  RecorderCallbacks cb;
  ProviderBroker broker;
  if (!broker.set_runtime_mode_requested(RuntimeMode::synthetic).ok() ||
      !broker.set_synthetic_role_requested(SyntheticRole::Timeline).ok() ||
      !broker.set_synthetic_timing_driver_requested(TimingDriver::VirtualTime).ok() ||
      !broker.initialize(&cb).ok()) {
    std::cerr << "FAIL broker timeline host surface setup failed\n";
    return false;
  }

  std::vector<SyntheticScheduledEvent> dispatched;
  broker.set_synthetic_timeline_request_dispatch_hook([&](const SyntheticScheduledEvent& ev) {
    dispatched.push_back(ev);
  });

  if (!broker.select_timeline_builtin_scenario_for_host("stream_lifecycle_versions").ok()) {
    std::cerr << "FAIL broker timeline host surface builtin selection failed\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  (void)broker.shutdown();
    return false;
  }

  if (!broker.advance_timeline_for_host(0).ok() || !dispatched.empty()) {
    std::cerr << "FAIL broker timeline host surface staged scenario dispatched before start\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  (void)broker.shutdown();
    return false;
  }

  if (!broker.start_timeline_scenario_for_host().ok() ||
      !broker.set_completion_gated_destructive_sequencing_for_host(true).ok() ||
      !broker.set_completion_gated_destructive_sequencing_for_host(false).ok() ||
      !broker.advance_timeline_for_host(60'000'002).ok()) {
    std::cerr << "FAIL broker timeline host surface run failed\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  (void)broker.shutdown();
    return false;
  }

  bool saw_open = false;
  bool saw_create = false;
  bool saw_start = false;
  bool saw_stop = false;
  bool saw_destroy = false;
  bool saw_close = false;
  for (const auto& ev : dispatched) {
    if (!saw_open && ev.type == SyntheticEventType::OpenDevice) saw_open = true;
    if (saw_open && !saw_create && ev.type == SyntheticEventType::CreateStream) saw_create = true;
    if (saw_create && !saw_start && ev.type == SyntheticEventType::StartStream) saw_start = true;
    if (saw_start && !saw_stop && ev.type == SyntheticEventType::StopStream) saw_stop = true;
    if (saw_stop && !saw_destroy && ev.type == SyntheticEventType::DestroyStream) saw_destroy = true;
    if (saw_destroy && !saw_close && ev.type == SyntheticEventType::CloseDevice) saw_close = true;
  }
  if (!(saw_open && saw_create && saw_start && saw_stop && saw_destroy && saw_close)) {
    std::cerr << "FAIL broker timeline host surface missing expected dispatch progression\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  (void)broker.shutdown();
    return false;
  }

  const std::string valid_json = R"JSON(
{
  "schema_version": 1,
  "devices": [{ "key": "cam0", "endpoint_index": 0 }],
  "streams": [{
    "key": "stream0",
    "device_key": "cam0",
    "intent": "PREVIEW",
    "capture_profile": {
      "width": 64,
      "height": 64,
      "format_fourcc": 1094862674,
      "target_fps_min": 30,
      "target_fps_max": 30
    }
  }],
  "timeline": [
    { "at_ns": 0, "type": "OpenDevice", "device_key": "cam0" },
    { "at_ns": 0, "type": "CreateStream", "stream_key": "stream0" },
    { "at_ns": 0, "type": "StartStream", "stream_key": "stream0" }
  ]
}
)JSON";

  dispatched.clear();
  if (!broker.load_timeline_canonical_scenario_from_json_text_for_host(valid_json, nullptr).ok() ||
      !broker.advance_timeline_for_host(0).ok() || !dispatched.empty()) {
    std::cerr << "FAIL broker timeline host surface external stage-only behavior changed\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  (void)broker.shutdown();
    return false;
  }

  std::string load_error;
  if (broker.load_timeline_canonical_scenario_from_json_text_for_host("{\"schema_version\":\"bad\"}", &load_error).ok()) {
    std::cerr << "FAIL broker timeline host surface strict loader accepted invalid json\n";
    flush_timeline_teardown_trace();
    flush_timeline_teardown_trace();
  (void)broker.shutdown();
    return false;
  }

  if (!broker.shutdown().ok()) {
    std::cerr << "FAIL broker timeline host surface shutdown failed\n";
    return false;
  }

  return true;
}

// ===== Family E: Synthetic frame/picture integration compliance =====

bool run_synthetic_timeline_picture_appearance_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.synthetic_role = SyntheticRole::Timeline;
  cfg.timing_driver = TimingDriver::VirtualTime;
  cfg.endpoint_count = 1;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  cfg.nominal.fps_num = 30;
  cfg.nominal.fps_den = 1;
  cfg.nominal.start_stream_warmup_ns = 0;

  SyntheticProvider synthetic(cfg);
  if (!synthetic.initialize(&cb).ok()) return false;

  const uint64_t device_id = 51;
  const uint64_t root_id = 5201;
  const uint64_t stream_id = 52;
  const StreamTemplate st = synthetic.stream_template();
  const uint32_t fps_num = st.profile.target_fps_max != 0 ? st.profile.target_fps_max : st.profile.target_fps_min;
  if (fps_num == 0) {
    std::cerr << "FAIL synthetic picture check invalid fps\n";
    (void)synthetic.shutdown();
    return false;
  }
  const uint64_t period_ns = 1'000'000'000ull / static_cast<uint64_t>(fps_num);

  StreamRequest req{};
  req.stream_id = stream_id;
  req.device_instance_id = device_id;
  req.intent = StreamIntent::PREVIEW;
  req.profile = st.profile;
  req.picture = st.picture;

  if (!synthetic.open_device("synthetic:0", device_id, root_id).ok() ||
      !synthetic.create_stream(req).ok() ||
      !synthetic.start_stream(stream_id, req.profile, req.picture).ok()) {
    (void)synthetic.shutdown();
    return false;
  }

  SyntheticTimelineScenario scenario{};
  for (uint64_t i = 0; i < 3; ++i) {
    SyntheticScheduledEvent ev{};
    ev.at_ns = period_ns * i;
    ev.type = SyntheticEventType::EmitFrame;
    ev.stream_id = stream_id;
    scenario.events.push_back(ev);
  }

  if (!synthetic.set_timeline_scenario_for_host(scenario).ok() ||
      !synthetic.start_timeline_scenario_for_host().ok()) {
    (void)synthetic.shutdown();
    return false;
  }

  synthetic.advance(0);

  PictureConfig updated = req.picture;
  updated.preset = PatternPreset::Solid;
  updated.overlay_frame_index_offsets = false;
  updated.overlay_moving_bar = false;
  updated.solid_r = 25;
  updated.solid_g = 200;
  updated.solid_b = 75;
  updated.solid_a = 255;
  if (!synthetic.set_stream_picture_config(stream_id, updated).ok()) {
    (void)synthetic.shutdown();
    return false;
  }

  synthetic.advance(period_ns * 2);

  const int f0 = find_frame_index_by_ts(cb.events, 0);
  const int f1 = find_frame_index_by_ts(cb.events, period_ns);
  const int f2 = find_frame_index_by_ts(cb.events, period_ns * 2);
  if (f0 < 0 || f1 < 0 || f2 < 0) {
    std::cerr << "FAIL synthetic picture check frame evidence missing\n";
    (void)synthetic.shutdown();
    return false;
  }

  const uint32_t sig0 = cb.events[static_cast<size_t>(f0)].pixel_sig;
  const uint32_t sig1 = cb.events[static_cast<size_t>(f1)].pixel_sig;
  const uint32_t sig2 = cb.events[static_cast<size_t>(f2)].pixel_sig;
  if (sig0 == sig1 || sig1 != sig2) {
    std::cerr << "FAIL synthetic picture check rendered appearance contract mismatch\n";
    (void)synthetic.shutdown();
    return false;
  }

  if (!synthetic.stop_stream(stream_id).ok() ||
      !synthetic.destroy_stream(stream_id).ok() ||
      !synthetic.close_device(device_id).ok() ||
      !synthetic.shutdown().ok()) {
    return false;
  }
  return assert_native_balance(cb.events, "synthetic_picture_appearance");
}

bool run_stub_provider_sanity_check() {
  RecorderCallbacks cb;
  StubProvider provider;

  StreamRequest req{};
  req.stream_id = 11;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 64;
  req.profile.height = 64;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  if (!provider.initialize(&cb).ok() ||
      !provider.open_device("stub0", 1, 1001).ok() ||
      !provider.create_stream(req).ok() ||
      !provider.start_stream(req.stream_id, req.profile, req.picture).ok() ||
      !provider.stop_stream(req.stream_id).ok() ||
      !provider.destroy_stream(req.stream_id).ok() ||
      !provider.close_device(req.device_instance_id).ok() ||
      !provider.shutdown().ok()) {
    return false;
  }

  return assert_native_balance(cb.events, "stub");
}

bool run_synthetic_provider_direct_sanity_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);

  StreamRequest req{};
  req.stream_id = 12;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 64;
  req.profile.height = 64;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  if (!provider.initialize(&cb).ok() ||
      !provider.open_device("synthetic:0", 1, 2001).ok() ||
      !provider.create_stream(req).ok() ||
      !provider.start_stream(req.stream_id, req.profile, req.picture).ok() ||
      !provider.stop_stream(req.stream_id).ok() ||
      !provider.destroy_stream(req.stream_id).ok() ||
      !provider.close_device(req.device_instance_id).ok() ||
      !provider.shutdown().ok()) {
    return false;
  }

  const int stopped = find_event_index(cb.events, "stream_stopped", req.stream_id);
  const int destroyed = find_event_index(cb.events, "stream_destroyed", req.stream_id);
  const int closed = find_event_index(cb.events, "device_closed", req.device_instance_id);
  const int fp_native_id = find_native_create_id(cb.events, static_cast<uint32_t>(NativeObjectType::FrameProducer), req.stream_id);
  const int stream_native_id = find_native_create_id(cb.events, static_cast<uint32_t>(NativeObjectType::Stream), req.stream_id);
  const int device_native_id = find_native_create_id(cb.events, static_cast<uint32_t>(NativeObjectType::Device), 0);

  if (stopped < 0 || destroyed < 0 || closed < 0 || fp_native_id < 0 || stream_native_id < 0 || device_native_id < 0) {
    std::cerr << "FAIL synthetic direct sanity missing callback/native evidence\n";
    return false;
  }

  return assert_native_balance(cb.events, "synthetic_direct");
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_opts(argc, argv, opt)) {
    return 2;
  }

  // 1) Materialization / loader compliance.
  if (!run_synthetic_scenario_materialization_check()) return 1;
  if (!run_synthetic_builtin_scenario_library_build_check()) return 1;
  if (!run_synthetic_external_scenario_loader_check()) return 1;
  if (!run_synthetic_external_scenario_loader_negative_check()) return 1;

  // 2) Primitive lifecycle foundation.
  if (!run_synthetic_primitive_lifecycle_foundation_check()) return 1;

  // 3) Clustered strict/gated destructive sequencing interpretation.
  if (!run_clustered_strict_branch_check()) return 1;
  if (!run_clustered_completion_gated_branch_check()) return 1;

  // 4) Broker / host surface compliance.
  if (!run_broker_timeline_host_surface_check()) return 1;

  // 5) Synthetic frame/picture integration checks.
  if (!run_synthetic_timeline_picture_appearance_check()) return 1;

  // Additional provider direct sanity coverage retained.
  if (!run_stub_provider_sanity_check()) return 1;
  if (!run_synthetic_provider_direct_sanity_check()) return 1;

  // 6) External scenario file path (first-class, optional input).
  if (!opt.external_scenario_file.empty()) {
    if (!run_external_scenario_file_execution_check(opt.external_scenario_file)) return 1;
  }

  flush_timeline_teardown_trace();
  std::cout << "PASS provider_compliance_verify\n";
  return 0;
}
