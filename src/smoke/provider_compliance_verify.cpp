// Deterministic provider compliance verifier: Stub + Synthetic only.
// No platform-backed hardware access in this tool.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "smoke/verify_case/verify_case_harness.h"
#include "imaging/api/timeline_teardown_trace.h"
#include "imaging/broker/provider_broker.h"
#include "imaging/stub/provider.h"
#include "imaging/synthetic/provider.h"
#include "imaging/synthetic/builtin_scenario_library.h"
#include "imaging/synthetic/scenario_loader.h"
#include "imaging/synthetic/scenario_model.h"

using namespace cambang;

namespace {

struct Options {
  std::string external_scenario_file;
};

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

void usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [--external_scenario_file=<path>]\n";
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

int find_index(const std::vector<EventRec>& events, const char* tag, uint64_t id) {
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].tag == tag && events[i].id == id) return static_cast<int>(i);
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

int find_frameproducer_create(const std::vector<EventRec>& events, uint64_t stream_id) {
  const uint32_t fp_type = static_cast<uint32_t>(NativeObjectType::FrameProducer);
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].tag == "native_created" && events[i].type == fp_type && events[i].owner_stream_id == stream_id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int find_native_create_id(const std::vector<EventRec>& events, uint32_t type, uint64_t owner_stream_id) {
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].tag == "native_created" && events[i].type == type && events[i].owner_stream_id == owner_stream_id) {
      return static_cast<int>(events[i].id);
    }
  }
  return -1;
}

bool assert_start_boundary(const std::vector<EventRec>& events, uint64_t stream_id, const char* name) {
  int fp_create = -1;
  int started = -1;
  // Provider callbacks are delivered on the strand worker; allow a short bounded wait
  // for posted start-boundary events to arrive before asserting relative ordering.
  for (int i = 0; i < 100; ++i) {
    fp_create = find_frameproducer_create(events, stream_id);
    started = find_index(events, "stream_started", stream_id);
    if (fp_create >= 0 && started >= 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (fp_create < 0 || started < 0 || fp_create > started) {
    std::cerr << "FAIL " << name << " start boundary ordering\n";
    return false;
  }
  return true;
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

bool advance_and_expect_snapshot(VerifyCaseHarness& harness,
                                 SyntheticProvider& synthetic,
                                 uint64_t dt_ns,
                                 const std::function<bool(const CamBANGStateSnapshot&)>& predicate,
                                 std::string& error,
                                 const char* timeout_message) {
  synthetic.advance(dt_ns);

  // Request/observe in a bounded loop because request-like timeline actions
  // are dispatched asynchronously through CoreRuntime and may require multiple
  // publish turns before the target milestone is snapshot-visible.
  constexpr int kMaxIters = 500;
  constexpr int kSleepMs = 5;
  for (int i = 0; i < kMaxIters; ++i) {
    harness.runtime().request_publish();
    auto snap = harness.snapshot_buffer().snapshot_copy();
    if (snap && predicate(*snap)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
  }

  error = timeout_message;
  return false;
}

void clear_timeline_teardown_trace_queue() {
  std::string line;
  while (timeline_teardown_trace_try_pop(line)) {
  }
}

bool wait_for_timeline_teardown_trace_contains(const std::string& needle,
                                               std::string& error,
                                               int timeout_ms = 500) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  std::string line;
  while (std::chrono::steady_clock::now() < deadline) {
    while (timeline_teardown_trace_try_pop(line)) {
      if (line.find(needle) != std::string::npos) {
        return true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  error = "timed out waiting for timeline trace: " + needle;
  return false;
}

bool run_synthetic_scenario_materialization_check() {
  SyntheticCanonicalScenario canonical{};

  SyntheticScenarioDeviceDeclaration d{};
  d.key = "cam_a";
  d.endpoint_index = 0;
  canonical.devices.push_back(d);

  SyntheticScenarioStreamDeclaration s0{};
  s0.key = "preview_a";
  s0.device_key = "cam_a";
  s0.intent = StreamIntent::PREVIEW;
  s0.baseline_capture_profile.width = 64;
  s0.baseline_capture_profile.height = 64;
  s0.baseline_capture_profile.format_fourcc = FOURCC_RGBA;
  s0.baseline_capture_profile.target_fps_min = 30;
  s0.baseline_capture_profile.target_fps_max = 30;
  canonical.streams.push_back(s0);

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
    std::cerr << "FAIL synthetic scenario materialization rejected valid canonical scenario: " << error << "\n";
    return false;
  }

  if (result.devices.size() != 1 || result.streams.size() != 1 || result.executable_schedule.events.size() != 4) {
    std::cerr << "FAIL synthetic scenario materialization unexpected output sizes\n";
    return false;
  }

  if (result.devices[0].device_instance_id != 201 || result.devices[0].root_id != 401) {
    std::cerr << "FAIL synthetic scenario materialization unexpected device id mapping\n";
    return false;
  }
  if (result.streams[0].stream_id != 601 || result.streams[0].device_instance_id != 201) {
    std::cerr << "FAIL synthetic scenario materialization unexpected stream id mapping\n";
    return false;
  }

  const auto& events = result.executable_schedule.events;
  if (events[0].type != SyntheticEventType::OpenDevice ||
      events[1].type != SyntheticEventType::CreateStream ||
      events[2].type != SyntheticEventType::StartStream ||
      events[3].type != SyntheticEventType::StopStream) {
    std::cerr << "FAIL synthetic scenario materialization did not stable-order by at_ns\n";
    return false;
  }
  for (const auto& ev : events) {
    if (ev.stream_id != 0 && ev.stream_id != 601) {
      std::cerr << "FAIL synthetic scenario materialization produced unstable stream id\n";
      return false;
    }
    if (ev.device_instance_id != 0 && ev.device_instance_id != 201) {
      std::cerr << "FAIL synthetic scenario materialization produced unstable device id\n";
      return false;
    }
  }

  return true;
}

bool run_synthetic_builtin_scenario_library_build_check() {
  CaptureProfile baseline_profile{};
  baseline_profile.width = 64;
  baseline_profile.height = 64;
  baseline_profile.format_fourcc = FOURCC_RGBA;
  baseline_profile.target_fps_min = 30;
  baseline_profile.target_fps_max = 30;

  const SyntheticBuiltinScenarioLibraryId ids[] = {
      SyntheticBuiltinScenarioLibraryId::StreamLifecycleVersions,
      SyntheticBuiltinScenarioLibraryId::TopologyChangeVersions,
      SyntheticBuiltinScenarioLibraryId::PublicationCoalescing,
  };

  for (SyntheticBuiltinScenarioLibraryId id : ids) {
    SyntheticCanonicalScenario canonical{};
    std::string error;
    if (!build_synthetic_builtin_scenario_library_canonical_scenario(
            id,
            baseline_profile,
            canonical,
            &error)) {
      std::cerr << "FAIL synthetic builtin scenario library build failed for "
                << synthetic_builtin_scenario_library_name(id) << ": " << error << "\n";
      return false;
    }
    if (canonical.devices.empty() || canonical.streams.empty() || canonical.timeline.empty()) {
      std::cerr << "FAIL synthetic builtin scenario library built empty scenario for "
                << synthetic_builtin_scenario_library_name(id) << "\n";
      return false;
    }
  }

  return true;
}

bool run_synthetic_external_scenario_loader_check() {
  CaptureProfile baseline_profile{};
  baseline_profile.width = 64;
  baseline_profile.height = 64;
  baseline_profile.format_fourcc = FOURCC_RGBA;
  baseline_profile.target_fps_min = 30;
  baseline_profile.target_fps_max = 30;

  SyntheticCanonicalScenario builtin{};
  std::string error;
  if (!build_synthetic_builtin_scenario_library_canonical_scenario(
          SyntheticBuiltinScenarioLibraryId::StreamLifecycleVersions,
          baseline_profile,
          builtin,
          &error)) {
    std::cerr << "FAIL synthetic external loader builtin build failed: " << error << "\n";
    return false;
  }

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
  if (!load_synthetic_canonical_scenario_from_json_text(json, loaded, &error)) {
    std::cerr << "FAIL synthetic external loader parse/validate/convert failed: " << error << "\n";
    return false;
  }

  if (loaded.devices.size() != builtin.devices.size() ||
      loaded.streams.size() != builtin.streams.size() ||
      loaded.timeline.size() != builtin.timeline.size()) {
    std::cerr << "FAIL synthetic external loader canonical shape mismatch against builtin\n";
    return false;
  }

  for (size_t i = 0; i < loaded.devices.size(); ++i) {
    if (loaded.devices[i].key != builtin.devices[i].key ||
        loaded.devices[i].endpoint_index != builtin.devices[i].endpoint_index) {
      std::cerr << "FAIL synthetic external loader device mismatch at index " << i << "\n";
      return false;
    }
  }

  for (size_t i = 0; i < loaded.streams.size(); ++i) {
    if (loaded.streams[i].key != builtin.streams[i].key ||
        loaded.streams[i].device_key != builtin.streams[i].device_key ||
        loaded.streams[i].intent != builtin.streams[i].intent ||
        loaded.streams[i].baseline_capture_profile.width != builtin.streams[i].baseline_capture_profile.width ||
        loaded.streams[i].baseline_capture_profile.height != builtin.streams[i].baseline_capture_profile.height ||
        loaded.streams[i].baseline_capture_profile.format_fourcc != builtin.streams[i].baseline_capture_profile.format_fourcc ||
        loaded.streams[i].baseline_capture_profile.target_fps_min != builtin.streams[i].baseline_capture_profile.target_fps_min ||
        loaded.streams[i].baseline_capture_profile.target_fps_max != builtin.streams[i].baseline_capture_profile.target_fps_max) {
      std::cerr << "FAIL synthetic external loader stream mismatch at index " << i << "\n";
      return false;
    }
  }

  for (size_t i = 0; i < loaded.timeline.size(); ++i) {
    const auto& a = loaded.timeline[i];
    const auto& b = builtin.timeline[i];
    if (a.at_ns != b.at_ns ||
        a.type != b.type ||
        a.device_key != b.device_key ||
        a.stream_key != b.stream_key ||
        a.has_picture != b.has_picture) {
      std::cerr << "FAIL synthetic external loader timeline mismatch at index " << i << "\n";
      return false;
    }
    if (a.has_picture) {
      if (a.picture.preset != b.picture.preset ||
          a.picture.seed != b.picture.seed ||
          a.picture.overlay_frame_index_offsets != b.picture.overlay_frame_index_offsets ||
          a.picture.overlay_moving_bar != b.picture.overlay_moving_bar ||
          a.picture.solid_r != b.picture.solid_r ||
          a.picture.solid_g != b.picture.solid_g ||
          a.picture.solid_b != b.picture.solid_b ||
          a.picture.solid_a != b.picture.solid_a ||
          a.picture.checker_size_px != b.picture.checker_size_px) {
        std::cerr << "FAIL synthetic external loader picture mismatch at timeline index " << i << "\n";
        return false;
      }
    }
  }

  SyntheticScenarioMaterializationResult loaded_materialized{};
  SyntheticScenarioMaterializationResult builtin_materialized{};
  SyntheticScenarioMaterializationOptions opts{};
  if (!materialize_synthetic_canonical_scenario(loaded, opts, loaded_materialized, &error)) {
    std::cerr << "FAIL synthetic external loader materialization failed: " << error << "\n";
    return false;
  }
  if (!materialize_synthetic_canonical_scenario(builtin, opts, builtin_materialized, &error)) {
    std::cerr << "FAIL synthetic external loader builtin materialization failed: " << error << "\n";
    return false;
  }
  if (loaded_materialized.executable_schedule.events.size() != builtin_materialized.executable_schedule.events.size()) {
    std::cerr << "FAIL synthetic external loader materialized schedule size mismatch\n";
    return false;
  }
  for (size_t i = 0; i < loaded_materialized.executable_schedule.events.size(); ++i) {
    const auto& l = loaded_materialized.executable_schedule.events[i];
    const auto& r = builtin_materialized.executable_schedule.events[i];
    if (l.at_ns != r.at_ns ||
        l.seq != r.seq ||
        l.type != r.type ||
        l.endpoint_index != r.endpoint_index ||
        l.device_instance_id != r.device_instance_id ||
        l.root_id != r.root_id ||
        l.stream_id != r.stream_id ||
        l.has_picture != r.has_picture) {
      std::cerr << "FAIL synthetic external loader materialized event mismatch at index " << i << "\n";
      return false;
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
  synthetic.set_timeline_request_dispatch_hook_for_host(
      [&dispatched](const SyntheticScheduledEvent& ev) {
        dispatched.push_back(ev.type);
      });

  if (!synthetic.load_timeline_canonical_scenario_from_json_text_for_host(json, &error).ok()) {
    std::cerr << "FAIL synthetic external loader provider-facing load+stage failed: " << error << "\n";
    return false;
  }
  if (!synthetic.start_timeline_scenario_for_host().ok()) return false;

  synthetic.advance(0);
  synthetic.advance(15'000'000);
  synthetic.advance(45'000'000);
  synthetic.advance(1);
  synthetic.advance(1);

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
    std::cerr << "FAIL synthetic external loader authored/materialized/dispatched mismatch\n";
    (void)synthetic.shutdown();
    return false;
  }

  if (!synthetic.shutdown().ok()) return false;
  return true;
}

bool run_synthetic_external_scenario_loader_negative_check() {
  struct NegativeCase {
    const char* name = nullptr;
    const char* json = nullptr;
    const char* error_hint = nullptr;
  };

  const std::vector<NegativeCase> cases{
      {
          "unknown_top_level_field",
          R"JSON({"schema_version":1,"devices":[],"streams":[],"timeline":[],"extra":1})JSON",
          "unknown field",
      },
      {
          "missing_required_top_level_field",
          R"JSON({"schema_version":1,"devices":[],"streams":[]})JSON",
          "missing required field",
      },
      {
          "wrong_type_required_field",
          R"JSON({"schema_version":"1","devices":[],"streams":[],"timeline":[]})JSON",
          "wrong type",
      },
      {
          "unknown_action_type",
          R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"DoThing","stream_key":"stream0"}]})JSON",
          "type is unknown",
      },
      {
          "emit_frame_rejected",
          R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"EmitFrame","stream_key":"stream0"}]})JSON",
          "EmitFrame",
      },
      {
          "stream_device_key_unknown",
          R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"missing_cam","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[]})JSON",
          "unknown device key",
      },
      {
          "timeline_unknown_stream_key",
          R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"StartStream","stream_key":"missing_stream"}]})JSON",
          "unknown stream key",
      },
      {
          "device_action_with_stream_key",
          R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"OpenDevice","device_key":"cam0","stream_key":"stream0"}]})JSON",
          "must contain only device_key",
      },
      {
          "stream_action_with_device_key",
          R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"StartStream","stream_key":"stream0","device_key":"cam0"}]})JSON",
          "must contain only stream_key",
      },
      {
          "update_picture_missing_picture_payload",
          R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"UpdateStreamPicture","stream_key":"stream0"}]})JSON",
          "stream_key and picture only",
      },
      {
          "non_picture_action_carries_picture",
          R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"StopStream","stream_key":"stream0","picture":{"preset":"checker","seed":1,"overlay_frame_index_offsets":false,"overlay_moving_bar":true,"solid_r":0,"solid_g":0,"solid_b":0,"solid_a":255,"checker_size_px":8}}]})JSON",
          "must contain only stream_key",
      },
      {
          "invalid_picture_preset_token",
          R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[{"at_ns":0,"type":"UpdateStreamPicture","stream_key":"stream0","picture":{"preset":"not_a_preset","seed":1,"overlay_frame_index_offsets":false,"overlay_moving_bar":true,"solid_r":0,"solid_g":0,"solid_b":0,"solid_a":255,"checker_size_px":8}}]})JSON",
          "picture.preset",
      },
      {
          "duplicate_device_key",
          R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0},{"key":"cam0","endpoint_index":1}],"streams":[],"timeline":[]})JSON",
          "duplicate device key",
      },
      {
          "duplicate_stream_key",
          R"JSON({"schema_version":1,"devices":[{"key":"cam0","endpoint_index":0}],"streams":[{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}},{"key":"stream0","device_key":"cam0","intent":"PREVIEW","capture_profile":{"width":64,"height":64,"format_fourcc":1094862674,"target_fps_min":30,"target_fps_max":30}}],"timeline":[]})JSON",
          "duplicate stream key",
      },
      {
          "schema_version_not_one",
          R"JSON({"schema_version":2,"devices":[],"streams":[],"timeline":[]})JSON",
          "schema_version",
      },
  };

  for (const auto& c : cases) {
    SyntheticCanonicalScenario out{};
    std::string error;
    if (load_synthetic_canonical_scenario_from_json_text(c.json, out, &error)) {
      std::cerr << "FAIL synthetic external loader negative case unexpectedly succeeded: " << c.name << "\n";
      return false;
    }
    if (error.empty()) {
      std::cerr << "FAIL synthetic external loader negative case returned empty error: " << c.name << "\n";
      return false;
    }
    if (error.find(c.error_hint) == std::string::npos) {
      std::cerr << "FAIL synthetic external loader negative case error mismatch: " << c.name
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
  SyntheticScenarioMaterializationOptions opts{};
  if (!materialize_synthetic_canonical_scenario(canonical, opts, materialized, &error)) {
    std::cerr << "FAIL external scenario file materialization failed: " << error << "\n";
    return false;
  }

  std::vector<SyntheticEventType> expected_dispatch;
  expected_dispatch.reserve(materialized.executable_schedule.events.size());
  uint64_t max_at_ns = 0;
  for (const auto& ev : materialized.executable_schedule.events) {
    if (ev.at_ns > max_at_ns) {
      max_at_ns = ev.at_ns;
    }
    if (ev.type == SyntheticEventType::EmitFrame) {
      continue;
    }
    expected_dispatch.push_back(ev.type);
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
  synthetic.set_timeline_request_dispatch_hook_for_host(
      [&dispatched](const SyntheticScheduledEvent& ev) {
        dispatched.push_back(ev.type);
      });

  if (!synthetic.load_timeline_canonical_scenario_from_json_file_for_host(path, &error).ok()) {
    std::cerr << "FAIL external scenario file load+stage through provider failed: " << error << "\n";
    (void)synthetic.shutdown();
    return false;
  }
  if (!dispatched.empty()) {
    std::cerr << "FAIL external scenario file load unexpectedly dispatched before explicit start\n";
    (void)synthetic.shutdown();
    return false;
  }

  if (!synthetic.start_timeline_scenario_for_host().ok()) {
    std::cerr << "FAIL external scenario file explicit start failed\n";
    (void)synthetic.shutdown();
    return false;
  }

  synthetic.advance(max_at_ns + 1);

  if (dispatched != expected_dispatch) {
    std::cerr << "FAIL external scenario file dispatch/order mismatch\n";
    (void)synthetic.shutdown();
    return false;
  }

  if (!synthetic.shutdown().ok()) return false;
  return true;
}

bool run_synthetic_timeline_canonical_submission_check() {
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
  synthetic.set_timeline_request_dispatch_hook_for_host(
      [&dispatched](const SyntheticScheduledEvent& ev) {
        dispatched.push_back(ev.type);
      });

  SyntheticCanonicalScenario canonical{};
  SyntheticScenarioDeviceDeclaration d{};
  d.key = "cam";
  d.endpoint_index = 0;
  canonical.devices.push_back(d);

  SyntheticScenarioStreamDeclaration s{};
  s.key = "preview";
  s.device_key = "cam";
  s.intent = StreamIntent::PREVIEW;
  canonical.streams.push_back(s);

  SyntheticScenarioTimelineAction a{};
  a.at_ns = 0;
  a.type = SyntheticEventType::OpenDevice;
  a.device_key = "cam";
  canonical.timeline.push_back(a);

  a = {};
  a.at_ns = 0;
  a.type = SyntheticEventType::CreateStream;
  a.stream_key = "preview";
  canonical.timeline.push_back(a);

  a = {};
  a.at_ns = 10;
  a.type = SyntheticEventType::StartStream;
  a.stream_key = "preview";
  canonical.timeline.push_back(a);

  a = {};
  a.at_ns = 20;
  a.type = SyntheticEventType::StopStream;
  a.stream_key = "preview";
  canonical.timeline.push_back(a);

  if (!synthetic.set_timeline_scenario_for_host(canonical).ok()) return false;
  if (!synthetic.start_timeline_scenario_for_host().ok()) return false;

  synthetic.advance(0);
  synthetic.advance(10);
  synthetic.advance(10);

  const std::vector<SyntheticEventType> expected{
      SyntheticEventType::OpenDevice,
      SyntheticEventType::CreateStream,
      SyntheticEventType::StartStream,
      SyntheticEventType::StopStream,
  };
  if (dispatched != expected) {
    std::cerr << "FAIL synthetic canonical host scenario submission did not emit expected authored actions\n";
    (void)synthetic.shutdown();
    return false;
  }

  if (!synthetic.shutdown().ok()) return false;
  return true;
}

bool run_stub_check() {
  RecorderCallbacks cb;
  StubProvider p;
  StreamRequest req{};
  req.stream_id = 11;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 64;
  req.profile.height = 64;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  if (!p.initialize(&cb).ok()) return false;
  if (!p.open_device("stub0", 1, 1001).ok()) return false;
  if (!p.create_stream(req).ok()) return false;
  if (p.close_device(req.device_instance_id).ok()) {
    std::cerr << "FAIL stub close_device unexpectedly succeeded with live stream\n";
    return false;
  }
  CaptureProfile invalid_profile = req.profile;
  invalid_profile.width = 0;
  if (p.start_stream(req.stream_id, invalid_profile, req.picture).ok()) {
    std::cerr << "FAIL stub start_stream unexpectedly accepted incomplete effective profile\n";
    return false;
  }
  if (!p.start_stream(req.stream_id, req.profile, req.picture).ok()) return false;
  if (p.destroy_stream(req.stream_id).ok()) {
    std::cerr << "FAIL stub destroy_stream unexpectedly succeeded while started\n";
    return false;
  }
  if (!assert_start_boundary(cb.events, req.stream_id, "stub")) return false;

  bool saw_valid_timestamp = false;
  // Frame callbacks are asynchronous via provider strand; allow a short bounded
  // wait for at least one frame with contract-valid timestamp fields.
  for (int i = 0; i < 100 && !saw_valid_timestamp; ++i) {
    for (const auto& e : cb.events) {
      if (e.tag != "frame") {
        continue;
      }
      if (e.ts.domain == CaptureTimestampDomain::PROVIDER_MONOTONIC &&
          e.ts.tick_ns != 0 &&
          e.ts.value != 0) {
        saw_valid_timestamp = true;
        break;
      }
    }
    if (!saw_valid_timestamp) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  if (!saw_valid_timestamp) {
    std::cerr << "FAIL stub frame timestamp not contract-valid\n";
    return false;
  }

  if (!p.shutdown().ok()) return false;
  return assert_native_balance(cb.events, "stub");
}

bool run_synthetic_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider p(cfg);

  StreamRequest req{};
  req.stream_id = 12;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 64;
  req.profile.height = 64;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  if (!p.initialize(&cb).ok()) return false;
  if (!p.open_device("synthetic:0", 1, 2001).ok()) return false;
  if (!p.create_stream(req).ok()) return false;
  if (p.close_device(req.device_instance_id).ok()) {
    std::cerr << "FAIL synthetic close_device unexpectedly succeeded with live stream\n";
    return false;
  }
  if (!p.start_stream(req.stream_id, req.profile, req.picture).ok()) return false;
  if (p.destroy_stream(req.stream_id).ok()) {
    std::cerr << "FAIL synthetic destroy_stream unexpectedly succeeded while started\n";
    return false;
  }
  if (!assert_start_boundary(cb.events, req.stream_id, "synthetic")) return false;
  if (!p.shutdown().ok()) return false;

  const int fp_native_id = find_native_create_id(cb.events, static_cast<uint32_t>(NativeObjectType::FrameProducer), req.stream_id);
  const int stream_native_id = find_native_create_id(cb.events, static_cast<uint32_t>(NativeObjectType::Stream), req.stream_id);
  const int device_native_id = find_native_create_id(cb.events, static_cast<uint32_t>(NativeObjectType::Device), 0);

  const int stopped_idx = find_index(cb.events, "stream_stopped", req.stream_id);
  const int destroyed_idx = find_index(cb.events, "stream_destroyed", req.stream_id);
  const int device_closed_idx = find_index(cb.events, "device_closed", req.device_instance_id);

  const int fp_native_destroy_idx = find_index(cb.events, "native_destroyed", static_cast<uint64_t>(fp_native_id));
  const int stream_native_destroy_idx = find_index(cb.events, "native_destroyed", static_cast<uint64_t>(stream_native_id));
  const int device_native_destroy_idx = find_index(cb.events, "native_destroyed", static_cast<uint64_t>(device_native_id));

  if (!(stopped_idx >= 0 && fp_native_destroy_idx >= 0 && stopped_idx < fp_native_destroy_idx)) {
    std::cerr << "FAIL synthetic stop ordering (lifecycle before frame-producer destroy)\n";
    return false;
  }
  if (!(destroyed_idx >= 0 && stream_native_destroy_idx >= 0 && destroyed_idx < stream_native_destroy_idx)) {
    std::cerr << "FAIL synthetic destroy ordering (lifecycle before stream destroy)\n";
    return false;
  }
  if (!(device_closed_idx >= 0 && device_native_destroy_idx >= 0 && device_closed_idx < device_native_destroy_idx)) {
    std::cerr << "FAIL synthetic device close ordering (lifecycle before device destroy)\n";
    return false;
  }

  return assert_native_balance(cb.events, "synthetic");
}

bool run_synthetic_timeline_scenario_check() {
  VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
  std::string error;
  if (!harness.start_runtime(error)) {
    std::cerr << "FAIL synthetic timeline scenario harness start: " << error << "\n";
    return false;
  }
  auto* synthetic = dynamic_cast<SyntheticProvider*>(harness.runtime().attached_provider());
  if (!synthetic) {
    std::cerr << "FAIL synthetic timeline scenario provider cast failed\n";
    harness.stop_runtime();
    return false;
  }

  SyntheticTimelineScenario scenario{};
  const uint64_t device_id = 21;
  const uint64_t root_id = 2201;
  const uint64_t stream_id = 22;
  const uint64_t period_ns = 1'000'000'000ull / 30ull;

  SyntheticScheduledEvent ev{};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::OpenDevice;
  ev.endpoint_index = 0;
  ev.device_instance_id = device_id;
  ev.root_id = root_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::CreateStream;
  ev.device_instance_id = device_id;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::StartStream;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns * 2;
  ev.type = SyntheticEventType::StopStream;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns * 2 + 1;
  ev.type = SyntheticEventType::DestroyStream;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns * 2 + 2;
  ev.type = SyntheticEventType::CloseDevice;
  ev.device_instance_id = device_id;
  scenario.events.push_back(ev);

  if (!synthetic->set_timeline_scenario_for_host(scenario).ok()) return false;
  if (!synthetic->start_timeline_scenario_for_host().ok()) return false;

  if (!advance_and_expect_snapshot(harness, *synthetic, 0, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return VerifyCaseHarness::has_device(s, device_id) &&
               stream && stream->mode == CBStreamMode::FLOWING;
      }, error, "timed out waiting for timeline open/create/start")) {
    std::cerr << "FAIL synthetic timeline scenario did not realize open/create/start: " << error << "\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, period_ns, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return stream && stream->frames_received >= 1;
      }, error, "timed out waiting for timeline frame")) {
    std::cerr << "FAIL synthetic timeline scenario did not emit frame\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, period_ns, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return stream && stream->mode == CBStreamMode::STOPPED;
      }, error, "timed out waiting for timeline stop")) {
    std::cerr << "FAIL synthetic timeline scenario did not stop stream\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        return !VerifyCaseHarness::has_stream(s, stream_id) &&
               VerifyCaseHarness::has_device(s, device_id);
      }, error, "timed out waiting for timeline destroy")) {
    std::cerr << "FAIL synthetic timeline scenario did not destroy stream: " << error << "\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        return !VerifyCaseHarness::has_stream(s, stream_id) &&
               !VerifyCaseHarness::has_device(s, device_id);
      }, error, "timed out waiting for timeline close")) {
    std::cerr << "FAIL synthetic timeline scenario did not close device: " << error << "\n";
    return false;
  }

  harness.stop_runtime();
  return true;
}

bool run_synthetic_timeline_completion_gated_destructive_sequencing_check() {
  const uint64_t device_id = 121;
  const uint64_t root_id = 12201;
  const uint64_t stream_id = 122;
  const uint64_t period_ns = 1'000'000'000ull / 30ull;

  auto build_scenario = [&]() {
    SyntheticTimelineScenario scenario{};
    SyntheticScheduledEvent ev{};
    ev.at_ns = 0;
    ev.type = SyntheticEventType::OpenDevice;
    ev.endpoint_index = 0;
    ev.device_instance_id = device_id;
    ev.root_id = root_id;
    scenario.events.push_back(ev);

    ev = {};
    ev.at_ns = 0;
    ev.type = SyntheticEventType::CreateStream;
    ev.device_instance_id = device_id;
    ev.stream_id = stream_id;
    scenario.events.push_back(ev);

    ev = {};
    ev.at_ns = 0;
    ev.type = SyntheticEventType::StartStream;
    ev.stream_id = stream_id;
    scenario.events.push_back(ev);

    ev = {};
    ev.at_ns = period_ns * 2;
    ev.type = SyntheticEventType::StopStream;
    ev.stream_id = stream_id;
    scenario.events.push_back(ev);

    ev = {};
    ev.at_ns = period_ns * 2;
    ev.type = SyntheticEventType::DestroyStream;
    ev.stream_id = stream_id;
    scenario.events.push_back(ev);

    ev = {};
    ev.at_ns = period_ns * 2;
    ev.type = SyntheticEventType::CloseDevice;
    ev.device_instance_id = device_id;
    scenario.events.push_back(ev);
    return scenario;
  };

  {
    VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
    std::string error;
    if (!harness.start_runtime(error)) {
      std::cerr << "FAIL synthetic timeline strict clustered teardown harness start: " << error << "\n";
      return false;
    }
    auto* synthetic = dynamic_cast<SyntheticProvider*>(harness.runtime().attached_provider());
    if (!synthetic) {
      std::cerr << "FAIL synthetic timeline strict clustered teardown provider cast failed\n";
      harness.stop_runtime();
      return false;
    }

    if (!synthetic->set_completion_gated_destructive_sequencing_for_host(false).ok()) {
      std::cerr << "FAIL synthetic timeline strict clustered teardown disable option failed\n";
      harness.stop_runtime();
      return false;
    }
    if (!synthetic->set_timeline_scenario_for_host(build_scenario()).ok() ||
        !synthetic->start_timeline_scenario_for_host().ok()) {
      std::cerr << "FAIL synthetic timeline strict clustered teardown scenario start failed\n";
      harness.stop_runtime();
      return false;
    }

    if (!advance_and_expect_snapshot(harness, *synthetic, 0, [&](const CamBANGStateSnapshot& s) {
          const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
          return VerifyCaseHarness::has_device(s, device_id) &&
                 stream && stream->mode == CBStreamMode::FLOWING;
        }, error, "timed out waiting for strict clustered teardown precondition")) {
      std::cerr << "FAIL synthetic timeline strict clustered teardown did not reach precondition: " << error << "\n";
      harness.stop_runtime();
      return false;
    }

    synthetic->advance(period_ns * 2);
    if (!harness.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
          const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
          if (!stream) {
            return true;
          }
          return stream->mode == CBStreamMode::STOPPED;
        }, error, 500, 5, "timed out waiting for strict clustered teardown post-boundary transition")) {
      std::cerr << "FAIL synthetic timeline strict clustered teardown did not leave FLOWING state: " << error << "\n";
      harness.stop_runtime();
      return false;
    }

    auto strict_snap = harness.snapshot_buffer().snapshot_copy();
    if (!strict_snap) {
      std::cerr << "FAIL synthetic timeline strict clustered teardown missing snapshot after post-boundary transition\n";
      harness.stop_runtime();
      return false;
    }
    const bool has_stream = VerifyCaseHarness::has_stream(*strict_snap, stream_id);
    const bool has_device = VerifyCaseHarness::has_device(*strict_snap, device_id);
    if (has_stream) {
      const auto* stream = VerifyCaseHarness::find_stream(*strict_snap, stream_id);
      if (!stream || stream->mode == CBStreamMode::FLOWING) {
        std::cerr << "FAIL synthetic timeline strict clustered teardown invalid stream truth after transition\n";
        harness.stop_runtime();
        return false;
      }
      if (!has_device) {
        std::cerr << "FAIL synthetic timeline strict clustered teardown stream present while device absent\n";
        harness.stop_runtime();
        return false;
      }
    }
    harness.stop_runtime();
  }

  {
    VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
    std::string error;
    if (!harness.start_runtime(error)) {
      std::cerr << "FAIL synthetic timeline completion-gated teardown harness start: " << error << "\n";
      return false;
    }
    auto* synthetic = dynamic_cast<SyntheticProvider*>(harness.runtime().attached_provider());
    if (!synthetic) {
      std::cerr << "FAIL synthetic timeline completion-gated teardown provider cast failed\n";
      harness.stop_runtime();
      return false;
    }

    if (!synthetic->set_completion_gated_destructive_sequencing_for_host(true).ok()) {
      std::cerr << "FAIL synthetic timeline completion-gated teardown enable option failed\n";
      harness.stop_runtime();
      return false;
    }
    clear_timeline_teardown_trace_queue();
    if (!synthetic->set_timeline_scenario_for_host(build_scenario()).ok() ||
        !synthetic->start_timeline_scenario_for_host().ok()) {
      std::cerr << "FAIL synthetic timeline completion-gated teardown scenario start failed\n";
      harness.stop_runtime();
      return false;
    }

    if (!advance_and_expect_snapshot(harness, *synthetic, 0, [&](const CamBANGStateSnapshot& s) {
          return VerifyCaseHarness::has_stream(s, stream_id) && VerifyCaseHarness::has_device(s, device_id);
        }, error, "timed out waiting for completion-gated teardown precondition")) {
      std::cerr << "FAIL synthetic timeline completion-gated teardown did not reach precondition: " << error << "\n";
      harness.stop_runtime();
      return false;
    }
    synthetic->advance(period_ns * 2);

    struct CallbackOrderEvidence {
      int stopped_index = -1;
      int destroyed_index = -1;
      int closed_index = -1;
      bool fail_destroy = false;
      bool fail_close = false;
      int seq = 0;
    };
    CallbackOrderEvidence evidence{};
    auto drain_teardown_trace = [&]() {
      std::string line;
      while (timeline_teardown_trace_try_pop(line)) {
        ++evidence.seq;
        if (line.find("callback on_stream_stopped stream_id=" + std::to_string(stream_id)) != std::string::npos &&
            evidence.stopped_index < 0) {
          evidence.stopped_index = evidence.seq;
        }
        if (line.find("callback on_stream_destroyed stream_id=" + std::to_string(stream_id)) != std::string::npos &&
            evidence.destroyed_index < 0) {
          evidence.destroyed_index = evidence.seq;
        }
        if (line.find("callback on_device_closed instance_id=" + std::to_string(device_id)) != std::string::npos &&
            evidence.closed_index < 0) {
          evidence.closed_index = evidence.seq;
        }
        if (line.find("fail DestroyStream stream_id=" + std::to_string(stream_id) + " reason=provider_rc_") != std::string::npos) {
          evidence.fail_destroy = true;
        }
        if (line.find("fail CloseDevice device_instance_id=" + std::to_string(device_id) + " reason=provider_rc_") != std::string::npos) {
          evidence.fail_close = true;
        }
      }
    };
    const auto callback_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
    while (std::chrono::steady_clock::now() < callback_deadline &&
           (evidence.stopped_index < 0 || evidence.destroyed_index < 0 || evidence.closed_index < 0)) {
      drain_teardown_trace();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    drain_teardown_trace();

    if (evidence.stopped_index < 0 || evidence.destroyed_index < 0 || evidence.closed_index < 0) {
      std::cerr << "FAIL synthetic timeline completion-gated teardown missing required callback evidence\n";
      harness.stop_runtime();
      return false;
    }
    if (!(evidence.stopped_index < evidence.destroyed_index &&
          evidence.destroyed_index < evidence.closed_index)) {
      std::cerr << "FAIL synthetic timeline completion-gated teardown callback order mismatch\n";
      harness.stop_runtime();
      return false;
    }
    if (evidence.fail_destroy || evidence.fail_close) {
      std::cerr << "FAIL synthetic timeline completion-gated teardown observed provider failure traces\n";
      harness.stop_runtime();
      return false;
    }

    if (!harness.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
          return !VerifyCaseHarness::has_stream(s, stream_id) &&
                 !VerifyCaseHarness::has_device(s, device_id);
        }, error, 800, 5, "timed out waiting for completion-gated final snapshot truth")) {
      std::cerr << "FAIL synthetic timeline completion-gated teardown final snapshot wait failed: " << error << "\n";
      harness.stop_runtime();
      return false;
    }
    harness.stop_runtime();
  }

  return true;
}

bool run_synthetic_primitive_lifecycle_completion_aware_check() {
  VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
  std::string error;
  if (!harness.start_runtime(error)) {
    std::cerr << "FAIL synthetic primitive lifecycle harness start: " << error << "\n";
    return false;
  }

  constexpr uint64_t kDeviceId = 9101;
  constexpr uint64_t kRootId = 9102;
  constexpr uint64_t kStreamId = 9103;

  clear_timeline_teardown_trace_queue();

  if (!harness.open_device_id(kDeviceId, 0, kRootId, error)) {
    std::cerr << "FAIL synthetic primitive lifecycle open_device: " << error << "\n";
    return false;
  }
  if (!harness.create_stream_id(kStreamId, kDeviceId, 1, error)) {
    std::cerr << "FAIL synthetic primitive lifecycle create_stream: " << error << "\n";
    return false;
  }
  if (!harness.start_stream_id(kStreamId, error)) {
    std::cerr << "FAIL synthetic primitive lifecycle start_stream: " << error << "\n";
    return false;
  }

  if (!harness.stop_stream_id(kStreamId, error)) {
    std::cerr << "FAIL synthetic primitive lifecycle stop_stream: " << error << "\n";
    return false;
  }
  if (!wait_for_timeline_teardown_trace_contains(
          "callback on_stream_stopped stream_id=" + std::to_string(kStreamId),
          error)) {
    std::cerr << "FAIL synthetic primitive lifecycle stop completion callback: " << error << "\n";
    return false;
  }

  if (!harness.destroy_stream_id(kStreamId, error)) {
    std::cerr << "FAIL synthetic primitive lifecycle destroy_stream: " << error << "\n";
    return false;
  }
  if (!wait_for_timeline_teardown_trace_contains(
          "callback on_stream_destroyed stream_id=" + std::to_string(kStreamId),
          error)) {
    std::cerr << "FAIL synthetic primitive lifecycle destroy completion callback: " << error << "\n";
    return false;
  }

  if (!harness.close_device_id(kDeviceId, error)) {
    std::cerr << "FAIL synthetic primitive lifecycle close_device: " << error << "\n";
    return false;
  }
  if (!wait_for_timeline_teardown_trace_contains(
          "callback on_device_closed instance_id=" + std::to_string(kDeviceId),
          error)) {
    std::cerr << "FAIL synthetic primitive lifecycle close completion callback: " << error << "\n";
    return false;
  }

  if (!harness.wait_for_core_snapshot([&](const CamBANGStateSnapshot& s) {
        return !VerifyCaseHarness::has_stream(s, kStreamId) &&
               !VerifyCaseHarness::has_device(s, kDeviceId);
      }, error, 500, 5, "timed out waiting for final primitive lifecycle teardown")) {
    std::cerr << "FAIL synthetic primitive lifecycle final integrated teardown: " << error << "\n";
    return false;
  }

  harness.stop_runtime();
  return true;
}

bool run_synthetic_timeline_invalid_order_check() {
  VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
  std::string error;
  if (!harness.start_runtime(error)) {
    std::cerr << "FAIL synthetic timeline invalid-order harness start: " << error << "\n";
    return false;
  }
  auto* synthetic = dynamic_cast<SyntheticProvider*>(harness.runtime().attached_provider());
  if (!synthetic) {
    std::cerr << "FAIL synthetic timeline invalid-order provider cast failed\n";
    harness.stop_runtime();
    return false;
  }

  SyntheticTimelineScenario scenario{};

  const uint64_t device_id = 31;
  const uint64_t root_id = 3201;
  const uint64_t stream_id = 32;

  SyntheticScheduledEvent ev{};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::StopStream; // invalid before stream exists
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::DestroyStream; // invalid before stream exists
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::CloseDevice; // invalid before device exists
  ev.device_instance_id = device_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 1;
  ev.type = SyntheticEventType::OpenDevice;
  ev.endpoint_index = 0;
  ev.device_instance_id = device_id;
  ev.root_id = root_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 2;
  ev.type = SyntheticEventType::CreateStream;
  ev.device_instance_id = device_id;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 3;
  ev.type = SyntheticEventType::StartStream;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 4;
  ev.type = SyntheticEventType::StopStream;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 5;
  ev.type = SyntheticEventType::DestroyStream;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 6;
  ev.type = SyntheticEventType::CloseDevice;
  ev.device_instance_id = device_id;
  scenario.events.push_back(ev);

  if (!synthetic->set_timeline_scenario_for_host(scenario).ok()) return false;
  if (!synthetic->start_timeline_scenario_for_host().ok()) return false;

  // Phase 1: invalid operations only. They must be no-ops.
  if (!advance_and_expect_snapshot(harness, *synthetic, 0, [&](const CamBANGStateSnapshot& s) {
        return !VerifyCaseHarness::has_stream(s, stream_id) &&
               !VerifyCaseHarness::has_device(s, device_id);
      }, error, "timed out waiting for invalid-order no-op boundary")) {
    std::cerr << "FAIL synthetic timeline invalid-order invalid actions had side effects: " << error << "\n";
    return false;
  }

  // Phase 2: explicit valid lifecycle in order.
  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        return VerifyCaseHarness::has_device(s, device_id) &&
               !VerifyCaseHarness::has_stream(s, stream_id);
      }, error, "timed out waiting for invalid-order open")) {
    std::cerr << "FAIL synthetic timeline invalid-order valid open missing\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return VerifyCaseHarness::has_device(s, device_id) &&
               stream && stream->mode == CBStreamMode::STOPPED;
      }, error, "timed out waiting for invalid-order create")) {
    std::cerr << "FAIL synthetic timeline invalid-order valid create missing\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return stream && stream->mode == CBStreamMode::FLOWING;
      }, error, "timed out waiting for invalid-order start")) {
    std::cerr << "FAIL synthetic timeline invalid-order valid start missing\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return stream && stream->mode == CBStreamMode::STOPPED;
      }, error, "timed out waiting for invalid-order stop")) {
    std::cerr << "FAIL synthetic timeline invalid-order valid stop missing\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        return !VerifyCaseHarness::has_stream(s, stream_id) &&
               VerifyCaseHarness::has_device(s, device_id);
      }, error, "timed out waiting for invalid-order destroy")) {
    std::cerr << "FAIL synthetic timeline invalid-order valid destroy missing\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        return !VerifyCaseHarness::has_stream(s, stream_id) &&
               !VerifyCaseHarness::has_device(s, device_id);
      }, error, "timed out waiting for invalid-order close")) {
    std::cerr << "FAIL synthetic timeline invalid-order valid close missing\n";
    return false;
  }

  harness.stop_runtime();
  return true;
}

bool run_synthetic_timeline_host_controls_check() {
  VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
  std::string error;
  if (!harness.start_runtime(error)) {
    std::cerr << "FAIL synthetic timeline host-controls harness start: " << error << "\n";
    return false;
  }
  auto* synthetic = dynamic_cast<SyntheticProvider*>(harness.runtime().attached_provider());
  if (!synthetic) {
    std::cerr << "FAIL synthetic timeline host-controls provider cast failed\n";
    harness.stop_runtime();
    return false;
  }

  const uint64_t device_id = 41;
  const uint64_t root_id = 4201;
  const uint64_t stream_id = 42;

  SyntheticTimelineScenario scenario{};
  SyntheticScheduledEvent ev{};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::OpenDevice;
  ev.endpoint_index = 0;
  ev.device_instance_id = device_id;
  ev.root_id = root_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::CreateStream;
  ev.device_instance_id = device_id;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::StartStream;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  if (!synthetic->set_timeline_scenario_for_host(scenario).ok()) return false;
  if (!synthetic->start_timeline_scenario_for_host().ok()) return false;
  if (!synthetic->set_timeline_scenario_paused_for_host(true).ok()) return false;
  if (!advance_and_expect_snapshot(harness, *synthetic, 0, [&](const CamBANGStateSnapshot& s) {
        return !VerifyCaseHarness::has_device(s, device_id);
      }, error, "timed out waiting for paused scenario")) {
    std::cerr << "FAIL synthetic timeline host pause did not hold execution\n";
    return false;
  }
  if (!synthetic->set_timeline_scenario_paused_for_host(false).ok()) return false;
  if (!advance_and_expect_snapshot(harness, *synthetic, 0, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return VerifyCaseHarness::has_device(s, device_id) &&
               stream && stream->mode == CBStreamMode::FLOWING;
      }, error, "timed out waiting for unpaused scenario")) {
    std::cerr << "FAIL synthetic timeline host controls did not execute scenario\n";
    return false;
  }
  if (!synthetic->stop_timeline_scenario_for_host().ok()) return false;

  harness.stop_runtime();
  return true;
}

bool run_broker_timeline_host_surface_check() {
  if (!ProviderBroker::check_mode_supported_in_build(RuntimeMode::synthetic).ok()) {
    std::cout << "SKIP broker timeline host surface: synthetic mode not built\n";
    return true;
  }

  RecorderCallbacks cb;
  ProviderBroker broker;
  if (!broker.set_runtime_mode_requested(RuntimeMode::synthetic).ok()) {
    std::cerr << "FAIL broker timeline host surface: synthetic mode request failed\n";
    return false;
  }
  if (!broker.set_synthetic_role_requested(SyntheticRole::Timeline).ok()) {
    std::cerr << "FAIL broker timeline host surface: synthetic role request failed\n";
    return false;
  }
  if (!broker.set_synthetic_timing_driver_requested(TimingDriver::VirtualTime).ok()) {
    std::cerr << "FAIL broker timeline host surface: synthetic timing driver request failed\n";
    return false;
  }
  if (!broker.initialize(&cb).ok()) {
    std::cerr << "FAIL broker timeline host surface: initialize failed\n";
    return false;
  }
  if (broker.synthetic_role_latched() != SyntheticRole::Timeline ||
      broker.synthetic_timing_driver_latched() != TimingDriver::VirtualTime) {
    std::cerr << "FAIL broker timeline host surface: latched synthetic config mismatch\n";
    (void)broker.shutdown();
    return false;
  }

  std::vector<SyntheticScheduledEvent> dispatched;
  broker.set_synthetic_timeline_request_dispatch_hook(
      [&](const SyntheticScheduledEvent& ev) { dispatched.push_back(ev); });

  if (!broker.select_timeline_builtin_scenario_for_host("stream_lifecycle_versions").ok()) {
    std::cerr << "FAIL broker timeline host surface: builtin scenario select failed\n";
    (void)broker.shutdown();
    return false;
  }

  // Stage-only behavior: explicit start is still required.
  if (!broker.advance_timeline_for_host(0).ok()) {
    std::cerr << "FAIL broker timeline host surface: advance before start failed unexpectedly\n";
    (void)broker.shutdown();
    return false;
  }
  if (!dispatched.empty()) {
    std::cerr << "FAIL broker timeline host surface: staged builtin dispatched before explicit start\n";
    (void)broker.shutdown();
    return false;
  }

  if (!broker.start_timeline_scenario_for_host().ok()) {
    std::cerr << "FAIL broker timeline host surface: start failed\n";
    (void)broker.shutdown();
    return false;
  }
  if (!broker.set_completion_gated_destructive_sequencing_for_host(true).ok()) {
    std::cerr << "FAIL broker timeline host surface: completion-gated destructive sequencing enable failed\n";
    (void)broker.shutdown();
    return false;
  }
  if (!broker.set_completion_gated_destructive_sequencing_for_host(false).ok()) {
    std::cerr << "FAIL broker timeline host surface: completion-gated destructive sequencing disable failed\n";
    (void)broker.shutdown();
    return false;
  }
  if (!broker.advance_timeline_for_host(0).ok()) {
    std::cerr << "FAIL broker timeline host surface: advance after start failed\n";
    (void)broker.shutdown();
    return false;
  }
  if (dispatched.size() < 3 ||
      dispatched[0].type != SyntheticEventType::OpenDevice ||
      dispatched[1].type != SyntheticEventType::CreateStream ||
      dispatched[2].type != SyntheticEventType::StartStream) {
    std::cerr << "FAIL broker timeline host surface: builtin dispatch order mismatch\n";
    (void)broker.shutdown();
    return false;
  }
  if (!broker.advance_timeline_for_host(60'000'002).ok()) {
    std::cerr << "FAIL broker timeline host surface: advance to teardown boundary failed\n";
    (void)broker.shutdown();
    return false;
  }
  bool saw_stop = false;
  bool saw_destroy = false;
  bool saw_close = false;
  for (const auto& ev : dispatched) {
    if (!saw_stop && ev.type == SyntheticEventType::StopStream) {
      saw_stop = true;
      continue;
    }
    if (saw_stop && !saw_destroy && ev.type == SyntheticEventType::DestroyStream) {
      saw_destroy = true;
      continue;
    }
    if (saw_stop && saw_destroy && ev.type == SyntheticEventType::CloseDevice) {
      saw_close = true;
      break;
    }
  }
  if (!(saw_stop && saw_destroy && saw_close)) {
    std::cerr << "FAIL broker timeline host surface: builtin teardown dispatch order mismatch\n";
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
  if (!broker.load_timeline_canonical_scenario_from_json_text_for_host(valid_json, nullptr).ok()) {
    std::cerr << "FAIL broker timeline host surface: valid external json load failed\n";
    (void)broker.shutdown();
    return false;
  }
  if (!broker.advance_timeline_for_host(0).ok()) {
    std::cerr << "FAIL broker timeline host surface: advance on staged external scenario failed\n";
    (void)broker.shutdown();
    return false;
  }
  if (!dispatched.empty()) {
    std::cerr << "FAIL broker timeline host surface: external scenario dispatched before explicit start\n";
    (void)broker.shutdown();
    return false;
  }

  std::string load_error;
  if (broker.load_timeline_canonical_scenario_from_json_text_for_host("{\"schema_version\":\"bad\"}", &load_error).ok()) {
    std::cerr << "FAIL broker timeline host surface: strict loader accepted invalid json\n";
    (void)broker.shutdown();
    return false;
  }

  if (!broker.shutdown().ok()) {
    std::cerr << "FAIL broker timeline host surface: shutdown failed\n";
    return false;
  }

  {
    RecorderCallbacks cb_nominal;
    ProviderBroker nominal_broker;
    if (!nominal_broker.set_runtime_mode_requested(RuntimeMode::synthetic).ok()) {
      std::cerr << "FAIL broker timeline host surface: nominal mode request failed\n";
      return false;
    }
    if (!nominal_broker.set_synthetic_role_requested(SyntheticRole::Nominal).ok()) {
      std::cerr << "FAIL broker timeline host surface: nominal role request failed\n";
      return false;
    }
    if (!nominal_broker.initialize(&cb_nominal).ok()) {
      std::cerr << "FAIL broker timeline host surface: nominal broker initialize failed\n";
      return false;
    }
    if (nominal_broker.start_timeline_scenario_for_host().code != ProviderError::ERR_NOT_SUPPORTED ||
        nominal_broker.load_timeline_canonical_scenario_from_json_text_for_host(valid_json, nullptr).code != ProviderError::ERR_NOT_SUPPORTED) {
      std::cerr << "FAIL broker timeline host surface: nominal synthetic role must reject timeline host operations\n";
      (void)nominal_broker.shutdown();
      return false;
    }
    if (!nominal_broker.shutdown().ok()) {
      std::cerr << "FAIL broker timeline host surface: nominal shutdown failed\n";
      return false;
    }
  }

  {
    RecorderCallbacks cb_realtime;
    ProviderBroker realtime_broker;
    if (!realtime_broker.set_runtime_mode_requested(RuntimeMode::synthetic).ok()) {
      std::cerr << "FAIL broker timeline host surface: realtime mode request failed\n";
      return false;
    }
    if (!realtime_broker.set_synthetic_role_requested(SyntheticRole::Timeline).ok()) {
      std::cerr << "FAIL broker timeline host surface: realtime role request failed\n";
      return false;
    }
    if (!realtime_broker.set_synthetic_timing_driver_requested(TimingDriver::RealTime).ok()) {
      std::cerr << "FAIL broker timeline host surface: realtime timing request failed\n";
      return false;
    }
    if (!realtime_broker.initialize(&cb_realtime).ok()) {
      std::cerr << "FAIL broker timeline host surface: realtime broker initialize failed\n";
      return false;
    }
    if (realtime_broker.advance_timeline_for_host(0).code != ProviderError::ERR_NOT_SUPPORTED) {
      std::cerr << "FAIL broker timeline host surface: realtime timing should reject advance_timeline_for_host\n";
      (void)realtime_broker.shutdown();
      return false;
    }
    if (!realtime_broker.shutdown().ok()) {
      std::cerr << "FAIL broker timeline host surface: realtime shutdown failed\n";
      return false;
    }
  }

  if (ProviderBroker::check_mode_supported_in_build(RuntimeMode::platform_backed).ok()) {
    RecorderCallbacks cb_platform;
    ProviderBroker platform_broker;
    if (!platform_broker.set_runtime_mode_requested(RuntimeMode::platform_backed).ok()) {
      std::cerr << "FAIL broker timeline host surface: platform mode request failed\n";
      return false;
    }
    if (!platform_broker.initialize(&cb_platform).ok()) {
      std::cerr << "FAIL broker timeline host surface: platform broker initialize failed\n";
      return false;
    }

    const bool unsupported =
        platform_broker.select_timeline_builtin_scenario_for_host("stream_lifecycle_versions").code == ProviderError::ERR_NOT_SUPPORTED &&
        platform_broker.load_timeline_canonical_scenario_from_json_text_for_host(valid_json, nullptr).code == ProviderError::ERR_NOT_SUPPORTED &&
        platform_broker.start_timeline_scenario_for_host().code == ProviderError::ERR_NOT_SUPPORTED &&
        platform_broker.stop_timeline_scenario_for_host().code == ProviderError::ERR_NOT_SUPPORTED &&
        platform_broker.set_timeline_scenario_paused_for_host(true).code == ProviderError::ERR_NOT_SUPPORTED &&
        platform_broker.advance_timeline_for_host(0).code == ProviderError::ERR_NOT_SUPPORTED;
    if (!unsupported) {
      std::cerr << "FAIL broker timeline host surface: unsupported-mode behavior changed\n";
      (void)platform_broker.shutdown();
      return false;
    }
    if (!platform_broker.shutdown().ok()) {
      std::cerr << "FAIL broker timeline host surface: platform shutdown failed\n";
      return false;
    }
  }

  return true;
}

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
    std::cerr << "FAIL synthetic timeline picture invalid synthetic fps\n";
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

  if (!synthetic.open_device("synthetic:0", device_id, root_id).ok()) return false;
  if (!synthetic.create_stream(req).ok()) return false;
  if (!synthetic.start_stream(stream_id, req.profile, req.picture).ok()) return false;

  SyntheticTimelineScenario scenario{};
  SyntheticScheduledEvent ev{};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::EmitFrame;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns;
  ev.type = SyntheticEventType::EmitFrame;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns * 2;
  ev.type = SyntheticEventType::EmitFrame;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  if (!synthetic.set_timeline_scenario_for_host(scenario).ok()) return false;
  if (!synthetic.start_timeline_scenario_for_host().ok()) return false;

  synthetic.advance(0);

  PictureConfig updated = req.picture;
  updated.preset = PatternPreset::Solid;
  updated.overlay_frame_index_offsets = false;
  updated.overlay_moving_bar = false;
  updated.solid_r = 25;
  updated.solid_g = 200;
  updated.solid_b = 75;
  updated.solid_a = 255;
  if (!synthetic.set_stream_picture_config(stream_id, updated).ok()) return false;

  synthetic.advance(period_ns);
  synthetic.advance(period_ns);

  const int f0 = find_frame_index_by_ts(cb.events, 0);
  const int f1 = find_frame_index_by_ts(cb.events, period_ns);
  const int f2 = find_frame_index_by_ts(cb.events, period_ns * 2);
  if (f0 < 0 || f1 < 0 || f2 < 0) {
    std::cerr << "FAIL synthetic timeline picture appearance frames missing\n";
    return false;
  }
  const uint32_t sig0 = cb.events[static_cast<size_t>(f0)].pixel_sig;
  const uint32_t sig1 = cb.events[static_cast<size_t>(f1)].pixel_sig;
  const uint32_t sig2 = cb.events[static_cast<size_t>(f2)].pixel_sig;
  if (sig0 == sig1) {
    std::cerr << "FAIL synthetic timeline picture update did not change rendered appearance\n";
    return false;
  }
  if (sig1 != sig2) {
    std::cerr << "FAIL synthetic timeline picture state did not persist until changed\n";
    return false;
  }

  if (!synthetic.stop_stream(stream_id).ok()) return false;
  if (!synthetic.destroy_stream(stream_id).ok()) return false;
  if (!synthetic.close_device(device_id).ok()) return false;
  if (!synthetic.shutdown().ok()) return false;
  return assert_native_balance(cb.events, "synthetic_timeline_picture_appearance");
}


} // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_opts(argc, argv, opt)) {
    return 2;
  }

  if (!run_synthetic_scenario_materialization_check()) return 1;
  if (!run_synthetic_builtin_scenario_library_build_check()) return 1;
  if (!run_synthetic_external_scenario_loader_check()) return 1;
  if (!run_synthetic_external_scenario_loader_negative_check()) return 1;
  if (!opt.external_scenario_file.empty()) {
    if (!run_external_scenario_file_execution_check(opt.external_scenario_file)) return 1;
  }
  if (!run_synthetic_timeline_canonical_submission_check()) return 1;
  if (!run_stub_check()) return 1;
  if (!run_synthetic_check()) return 1;
  if (!run_synthetic_timeline_scenario_check()) return 1;
  if (!run_synthetic_timeline_completion_gated_destructive_sequencing_check()) return 1;
  if (!run_synthetic_primitive_lifecycle_completion_aware_check()) return 1;
  if (!run_synthetic_timeline_invalid_order_check()) return 1;
  if (!run_synthetic_timeline_host_controls_check()) return 1;
  if (!run_broker_timeline_host_surface_check()) return 1;
  if (!run_synthetic_timeline_picture_appearance_check()) return 1;
  std::cout << "PASS provider_compliance_verify\n";
  return 0;
}
