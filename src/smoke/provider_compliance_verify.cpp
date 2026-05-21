// Deterministic provider compliance verifier: provider contract only.
// This tool intentionally uses provider callbacks, retained snapshots,
// and deterministic timeline dispatch observations as PASS/FAIL evidence.
#include <cstdint>
#include <algorithm>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/core_runtime.h"
#include "imaging/broker/provider_broker.h"
#include "imaging/stub/provider.h"
#include "imaging/synthetic/builtin_scenario_library.h"
#include "imaging/synthetic/gpu_backing_runtime.h"
#include "imaging/synthetic/provider.h"
#include "imaging/synthetic/scenario_loader.h"
#include "imaging/synthetic/scenario_model.h"
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

template <typename Fn>
bool run_named_check(const char* name, Fn&& fn) {
  std::cout << "BEGIN " << name << "\n";
  std::cout.flush();
  const bool ok = fn();
  if (ok) {
    std::cout << "PASS " << name << "\n";
  } else {
    std::cout << "FAIL " << name << "\n";
  }
  std::cout.flush();
  return ok;
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
  uint64_t capture_id = 0;
  uint32_t type = 0;
  uint64_t owner_acquisition_session_id = 0;
  uint64_t owner_stream_id = 0;
  uint32_t pixel_sig = 0;
  uint64_t payload_hash = 0;
  size_t payload_size_bytes = 0;
  uint32_t format_fourcc = 0;
  double sampled_luma = 0.0;
  uint8_t sample_r = 0;
  uint8_t sample_g = 0;
  uint8_t sample_b = 0;
  CaptureImageRouting capture_image_routing = CaptureImageRouting::DEFAULT_METERED;
  uint32_t capture_image_member_index = 0;
  int32_t capture_image_applied_exposure_compensation_milli_ev = 0;
  bool capture_image_has_realized_exposure_compensation_milli_ev = false;
  int32_t capture_image_realized_exposure_compensation_milli_ev = 0;
  CaptureTimestamp ts{};
};

uint64_t fnv1a64_hash_bytes(const uint8_t* data, size_t size_bytes) {
  constexpr uint64_t kOffset = 1469598103934665603ull;
  constexpr uint64_t kPrime = 1099511628211ull;
  uint64_t h = kOffset;
  for (size_t i = 0; i < size_bytes; ++i) {
    h ^= static_cast<uint64_t>(data[i]);
    h *= kPrime;
  }
  return h;
}

double luma_from_rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (0.2126 * static_cast<double>(r)) +
         (0.7152 * static_cast<double>(g)) +
         (0.0722 * static_cast<double>(b));
}

struct RecorderCallbacks final : IProviderCallbacks {
  uint64_t next_native_id = 1;
  std::vector<EventRec> events;
  std::unordered_map<uint64_t, uint32_t> native_type_by_id;
  std::unordered_map<uint64_t, uint64_t> native_owner_stream_by_id;
  mutable std::mutex mu;

  std::vector<EventRec> snapshot_events() const {
    std::lock_guard<std::mutex> lk(mu);
    return events;
  }

  uint64_t allocate_native_id(NativeObjectType) override { return next_native_id++; }
  uint64_t core_monotonic_now_ns() override { return 0; }
  bool is_stream_display_demand_active(uint64_t) override { return false; }

  void on_device_opened(uint64_t id) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"device_opened", id}); }
  void on_device_closed(uint64_t id) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"device_closed", id}); }
  void on_stream_created(uint64_t id) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"stream_created", id}); }
  void on_stream_destroyed(uint64_t id) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"stream_destroyed", id}); }
  void on_stream_started(uint64_t id) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"stream_started", id}); }
  void on_stream_stopped(uint64_t id, ProviderError) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"stream_stopped", id}); }
  void on_capture_started(uint64_t id, uint64_t) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"capture_started", id}); }
  void on_capture_completed(uint64_t id, uint64_t) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"capture_completed", id}); }
  void on_capture_failed(uint64_t id, uint64_t, ProviderError) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"capture_failed", id}); }

  void on_frame(const FrameView& frame) override {
    EventRec ev{"frame", 0};
    ev.capture_id = frame.capture_id;
    ev.format_fourcc = frame.format_fourcc;
    ev.ts = frame.capture_timestamp;
    if (frame.data && frame.size_bytes >= 4) {
      const uint8_t* p = static_cast<const uint8_t*>(frame.data);
      ev.pixel_sig = static_cast<uint32_t>(p[0]) |
                     (static_cast<uint32_t>(p[1]) << 8) |
                     (static_cast<uint32_t>(p[2]) << 16) |
                     (static_cast<uint32_t>(p[3]) << 24);
    }
    if (frame.data && frame.size_bytes > 0) {
      const uint8_t* p = static_cast<const uint8_t*>(frame.data);
      ev.payload_size_bytes = frame.size_bytes;
      ev.payload_hash = fnv1a64_hash_bytes(p, frame.size_bytes);
      const bool valid_shape = (frame.width > 0 && frame.height > 0 && frame.stride_bytes >= frame.width * 4u);
      const bool rgba_like = (frame.format_fourcc == FOURCC_RGBA || frame.format_fourcc == FOURCC_BGRA);
      if (valid_shape && rgba_like) {
        const uint32_t sx = frame.width / 2u;
        const uint32_t sy = frame.height / 2u;
        const size_t off = static_cast<size_t>(sy) * static_cast<size_t>(frame.stride_bytes) + static_cast<size_t>(sx) * 4u;
        if (off + 3 < frame.size_bytes) {
          const uint8_t c0 = p[off + 0];
          const uint8_t c1 = p[off + 1];
          const uint8_t c2 = p[off + 2];
          uint8_t r = c0;
          uint8_t g = c1;
          uint8_t b = c2;
          if (frame.format_fourcc == FOURCC_BGRA) {
            b = c0;
            g = c1;
            r = c2;
          }
          ev.sample_r = r;
          ev.sample_g = g;
          ev.sample_b = b;
          ev.sampled_luma = luma_from_rgb(r, g, b);
        }
      }
    }
    ev.capture_image_routing = frame.capture_image.routing;
    ev.capture_image_member_index = frame.capture_image.image_member_index;
    ev.capture_image_applied_exposure_compensation_milli_ev =
        frame.capture_image.applied_exposure_compensation_milli_ev;
    ev.capture_image_has_realized_exposure_compensation_milli_ev =
        frame.capture_image.has_realized_exposure_compensation_milli_ev;
    ev.capture_image_realized_exposure_compensation_milli_ev =
        frame.capture_image.realized_exposure_compensation_milli_ev;
    std::lock_guard<std::mutex> lk(mu);
    events.push_back(ev);
    if (frame.release) {
      frame.release(frame.release_user, &frame);
    }
  }

  void on_device_error(uint64_t id, ProviderError) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"device_error", id}); }
  void on_stream_error(uint64_t id, ProviderError) override { std::lock_guard<std::mutex> lk(mu); events.push_back({"stream_error", id}); }
  void on_native_object_created(const NativeObjectCreateInfo& info) override {
    EventRec ev{"native_created", info.native_id};
    ev.type = info.type;
    ev.owner_acquisition_session_id = info.owner_acquisition_session_id;
    ev.owner_stream_id = info.owner_stream_id;
    std::lock_guard<std::mutex> lk(mu);
    native_type_by_id[info.native_id] = info.type;
    native_owner_stream_by_id[info.native_id] = info.owner_stream_id;
    events.push_back(ev);
  }
  void on_native_object_destroyed(const NativeObjectDestroyInfo& info) override {
    std::lock_guard<std::mutex> lk(mu);
    EventRec ev{"native_destroyed", info.native_id};
    auto type_it = native_type_by_id.find(info.native_id);
    if (type_it != native_type_by_id.end()) {
      ev.type = type_it->second;
    }
    auto owner_stream_it = native_owner_stream_by_id.find(info.native_id);
    if (owner_stream_it != native_owner_stream_by_id.end()) {
      ev.owner_stream_id = owner_stream_it->second;
    }
    events.push_back(ev);
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

int find_native_create_id_with_owners(const std::vector<EventRec>& events,
                                      uint32_t type,
                                      uint64_t owner_acquisition_session_id,
                                      uint64_t owner_stream_id) {
  for (const auto& e : events) {
    if (e.tag == "native_created" &&
        e.type == type &&
        e.owner_acquisition_session_id == owner_acquisition_session_id &&
        e.owner_stream_id == owner_stream_id) {
      return static_cast<int>(e.id);
    }
  }
  return -1;
}

int find_native_create_id_by_type(const std::vector<EventRec>& events, uint32_t type) {
  for (const auto& e : events) {
    if (e.tag == "native_created" && e.type == type) {
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

int count_events_by_tag_and_type(const std::vector<EventRec>& events, const char* tag, uint32_t type) {
  int count = 0;
  for (const auto& e : events) {
    if (e.tag == tag && e.type == type) {
      ++count;
    }
  }
  return count;
}

int count_events_by_tag_type_and_owner_stream(const std::vector<EventRec>& events,
                                              const char* tag,
                                              uint32_t type,
                                              uint64_t owner_stream_id) {
  int count = 0;
  for (const auto& e : events) {
    if (e.tag == tag && e.type == type && e.owner_stream_id == owner_stream_id) {
      ++count;
    }
  }
  return count;
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
                                     uint64_t min_published_seq,
                                     const std::function<bool(const CamBANGStateSnapshot&)>& predicate,
                                     std::string& error,
                                     const char* timeout_msg,
                                     int max_iters = kMaxIters,
                                     int sleep_ms = kSleepMs) {
  for (int i = 0; i < max_iters; ++i) {
    synthetic.advance(advance_step_ns);
    harness.runtime().request_publish();
    if (harness.runtime().published_seq() < min_published_seq) {
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
      continue;
    }
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
  std::mutex dispatched_mu;
  synthetic.set_timeline_request_dispatch_hook_for_host([&dispatched, &dispatched_mu](const SyntheticScheduledEvent& ev) {
    std::lock_guard<std::mutex> lk(dispatched_mu);
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
  std::mutex dispatched_mu;
  synthetic.set_timeline_request_dispatch_hook_for_host([&dispatched, &dispatched_mu](const SyntheticScheduledEvent& ev) {
    std::lock_guard<std::mutex> lk(dispatched_mu);
    dispatched.push_back(ev.type);
  });

  if (!synthetic.load_timeline_canonical_scenario_from_json_file_for_host(path, &error).ok()) {
    std::cerr << "FAIL external scenario file provider load+stage failed: " << error << "\n";
    (void)synthetic.shutdown();
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(dispatched_mu);
    if (!dispatched.empty()) {
      std::cerr << "FAIL external scenario file dispatched before explicit start\n";
      (void)synthetic.shutdown();
      return false;
    }
  }

  if (!synthetic.start_timeline_scenario_for_host().ok()) {
    (void)synthetic.shutdown();
    return false;
  }
  synthetic.advance(max_at_ns + 1);

  std::vector<SyntheticEventType> dispatched_snapshot;
  {
    std::lock_guard<std::mutex> lk(dispatched_mu);
    dispatched_snapshot = dispatched;
  }
  if (dispatched_snapshot != expected_dispatch) {
    std::cerr << "FAIL external scenario file dispatch/order mismatch\n";
    synthetic.set_timeline_request_dispatch_hook_for_host({});
    (void)synthetic.shutdown();
    return false;
  }
  synthetic.set_timeline_request_dispatch_hook_for_host({});
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
    harness.stop_runtime();
    return false;
  }
  if (!(opened < created && created < started && started < stopped && stopped < destroyed && destroyed < closed)) {
    std::cerr << "FAIL primitive lifecycle callback order mismatch\n";
    harness.stop_runtime();
    return false;
  }

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
    harness.stop_runtime();
    return false;
  }

  const uint64_t period_ns = 1'000'000'000ull / 30ull;
  if (!synthetic->set_timeline_reconciliation_for_host(TimelineReconciliation::Strict).ok() ||
      !synthetic->set_timeline_scenario_for_host(build_clustered_destructive_scenario(period_ns)).ok() ||
      !synthetic->start_timeline_scenario_for_host().ok()) {
    std::cerr << "FAIL clustered strict setup failed\n";
    harness.stop_runtime();
    return false;
  }

  if (!wait_for_snapshot_with_progress(
          harness,
          *synthetic,
          0,
          harness.runtime().published_seq(),
          [&](const CamBANGStateSnapshot& s) {
            const auto* stream = VerifyCaseHarness::find_stream(s, kClusteredStreamId);
            return VerifyCaseHarness::has_device(s, kClusteredDeviceId) &&
                   stream && stream->mode == CBStreamMode::FLOWING;
          },
          error,
          "timed out waiting for clustered strict precondition")) {
    std::cerr << "FAIL clustered strict precondition failed: " << error << "\n";
    harness.stop_runtime();
    return false;
  }

  const uint64_t post_boundary_min_publish = harness.runtime().published_seq() + 1;
  synthetic->advance(period_ns * 2);
  harness.runtime().request_publish();

  if (!wait_for_snapshot_with_progress(
          harness,
          *synthetic,
          1,
          post_boundary_min_publish,
          [&](const CamBANGStateSnapshot& s) {
            const auto* stream = VerifyCaseHarness::find_stream(s, kClusteredStreamId);
            return !stream || stream->mode != CBStreamMode::FLOWING;
          },
          error,
          "timed out waiting for clustered strict post-boundary convergence")) {
    std::cerr << "FAIL clustered strict post-boundary convergence failed: " << error << "\n";
    harness.stop_runtime();
    return false;
  }

  auto snap = harness.snapshot_buffer().snapshot_copy();
  if (!snap) {
    std::cerr << "FAIL clustered strict missing converged post-boundary snapshot\n";
    harness.stop_runtime();
    return false;
  }

  // Strict interpretation: after clustered boundary, truth must be self-consistent.
  const bool has_stream = VerifyCaseHarness::has_stream(*snap, kClusteredStreamId);
  const bool has_device = VerifyCaseHarness::has_device(*snap, kClusteredDeviceId);
  if (has_stream) {
    const auto* stream = VerifyCaseHarness::find_stream(*snap, kClusteredStreamId);
    if (!stream || stream->mode == CBStreamMode::FLOWING) {
      std::cerr << "FAIL clustered strict impossible retained stream state\n";
      harness.stop_runtime();
      return false;
    }
    if (!has_device) {
      std::cerr << "FAIL clustered strict impossible retained topology (stream without device)\n";
      harness.stop_runtime();
      return false;
    }
  }

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
    harness.stop_runtime();
    return false;
  }

  std::vector<SyntheticEventType> dispatched;
  const auto core_dispatch = make_synthetic_timeline_request_dispatch_hook(harness.runtime());
  synthetic->set_timeline_request_dispatch_hook_for_host([&](const SyntheticScheduledEvent& ev) {
    dispatched.push_back(ev.type);
    core_dispatch(ev);
  });

  const uint64_t period_ns = 1'000'000'000ull / 30ull;
  if (!synthetic->set_timeline_reconciliation_for_host(TimelineReconciliation::CompletionGated).ok() ||
      !synthetic->set_timeline_scenario_for_host(build_clustered_destructive_scenario(period_ns)).ok() ||
      !synthetic->start_timeline_scenario_for_host().ok()) {
    std::cerr << "FAIL clustered gated setup failed\n";
    harness.stop_runtime();
    return false;
  }

  synthetic->advance(0);
  const int start_dispatch = static_cast<int>(
      std::find(dispatched.begin(), dispatched.end(), SyntheticEventType::StartStream) - dispatched.begin());
  if (start_dispatch >= static_cast<int>(dispatched.size())) {
    std::cerr << "FAIL clustered gated precondition dispatch evidence missing\n";
    harness.stop_runtime();
    return false;
  }

  int opened = -1;
  int created = -1;
  int started = -1;
  for (int i = 0; i < kMaxIters; ++i) {
    synthetic->advance(1);
    harness.runtime().request_publish();
    opened = harness.find_recorded_callback_index("device_opened", kClusteredDeviceId);
    created = harness.find_recorded_callback_index("stream_created", kClusteredStreamId);
    started = harness.find_recorded_callback_index("stream_started", kClusteredStreamId);
    if (opened >= 0 && created >= 0 && started >= 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
  }

  if (opened < 0 || created < 0 || started < 0) {
    std::cerr << "DIAG clustered gated startup precondition timeout expected stream_id=" << kClusteredStreamId
              << " device_id=" << kClusteredDeviceId
              << " callback_indices=(opened=" << opened
              << ", created=" << created
              << ", started=" << started << ")\n";
    std::cerr << "FAIL clustered gated startup precondition not realized\n";
    harness.stop_runtime();
    return false;
  }
  if (!(opened < created && created < started)) {
    std::cerr << "FAIL clustered gated startup callback order mismatch\n";
    harness.stop_runtime();
    return false;
  }

  harness.clear_recorded_callbacks();
  synthetic->advance(period_ns * 2);
  harness.runtime().request_publish();

  int stopped = -1;
  int destroyed = -1;
  int closed = -1;
  constexpr int kStageMaxIters = kMaxIters * 2;
  constexpr int kStageDrainIters = 50;
  auto wait_for_stage = [&](const char* tag,
                            uint64_t id,
                            const char* stage_name,
                            int& out_index) -> bool {
    for (int i = 0; i < kStageMaxIters; ++i) {
      synthetic->advance(1);
      harness.runtime().request_publish();
      stopped = harness.find_recorded_callback_index("stream_stopped", kClusteredStreamId);
      destroyed = harness.find_recorded_callback_index("stream_destroyed", kClusteredStreamId);
      closed = harness.find_recorded_callback_index("device_closed", kClusteredDeviceId);
      out_index = harness.find_recorded_callback_index(tag, id);
      if (out_index >= 0) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
    }

    for (int i = 0; i < kStageDrainIters; ++i) {
      synthetic->advance(1);
      harness.runtime().request_publish();
      out_index = harness.find_recorded_callback_index(tag, id);
      if (out_index >= 0) {
        stopped = harness.find_recorded_callback_index("stream_stopped", kClusteredStreamId);
        destroyed = harness.find_recorded_callback_index("stream_destroyed", kClusteredStreamId);
        closed = harness.find_recorded_callback_index("device_closed", kClusteredDeviceId);
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
    }

    stopped = harness.find_recorded_callback_index("stream_stopped", kClusteredStreamId);
    destroyed = harness.find_recorded_callback_index("stream_destroyed", kClusteredStreamId);
    closed = harness.find_recorded_callback_index("device_closed", kClusteredDeviceId);
    std::cerr << "DIAG clustered gated callback stage timeout stage=" << stage_name
              << " expected stream_id=" << kClusteredStreamId
              << " device_id=" << kClusteredDeviceId
              << " callback_indices=(stopped=" << stopped
              << ", destroyed=" << destroyed
              << ", closed=" << closed << ")\n";
    std::cerr << "FAIL clustered gated missing callback evidence at stage " << stage_name << "\n";
    return false;
  };

  if (!wait_for_stage("stream_stopped", kClusteredStreamId, "stream_stopped", stopped)) {
    harness.stop_runtime();
    return false;
  }
  if (!wait_for_stage("stream_destroyed", kClusteredStreamId, "stream_destroyed", destroyed)) {
    harness.stop_runtime();
    return false;
  }
  if (!wait_for_stage("device_closed", kClusteredDeviceId, "device_closed", closed)) {
    harness.stop_runtime();
    return false;
  }

  if (!(stopped < destroyed && destroyed < closed)) {
    std::cerr << "FAIL clustered gated callback order mismatch\n";
    harness.stop_runtime();
    return false;
  }

  const int stop_dispatch = static_cast<int>(
      std::find(dispatched.begin(), dispatched.end(), SyntheticEventType::StopStream) - dispatched.begin());
  const int destroy_dispatch = static_cast<int>(
      std::find(dispatched.begin(), dispatched.end(), SyntheticEventType::DestroyStream) - dispatched.begin());
  const int close_dispatch = static_cast<int>(
      std::find(dispatched.begin(), dispatched.end(), SyntheticEventType::CloseDevice) - dispatched.begin());
  if (stop_dispatch >= static_cast<int>(dispatched.size()) ||
      destroy_dispatch >= static_cast<int>(dispatched.size()) ||
      close_dispatch >= static_cast<int>(dispatched.size())) {
    std::cerr << "FAIL clustered gated clustered-boundary dispatch evidence missing\n";
    harness.stop_runtime();
    return false;
  }

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
    (void)broker.shutdown();
    return false;
  }

  if (!broker.advance_timeline_for_host(0).ok() || !dispatched.empty()) {
    std::cerr << "FAIL broker timeline host surface staged scenario dispatched before start\n";
    (void)broker.shutdown();
    return false;
  }

  if (!broker.start_timeline_scenario_for_host().ok() ||
      !broker.set_timeline_reconciliation_for_host(TimelineReconciliation::CompletionGated).ok() ||
      !broker.set_timeline_reconciliation_for_host(TimelineReconciliation::Strict).ok() ||
      !broker.advance_timeline_for_host(60'000'002).ok()) {
    std::cerr << "FAIL broker timeline host surface run failed\n";
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
    (void)broker.shutdown();
    return false;
  }

  std::string load_error;
  if (broker.load_timeline_canonical_scenario_from_json_text_for_host("{\"schema_version\":\"bad\"}", &load_error).ok()) {
    std::cerr << "FAIL broker timeline host surface strict loader accepted invalid json\n";
    (void)broker.shutdown();
    return false;
  }

  if (!broker.shutdown().ok()) {
    std::cerr << "FAIL broker timeline host surface shutdown failed\n";
    return false;
  }

  return true;
}


bool run_synthetic_backing_capability_advertisement_check() {
  auto verify_mode = [](SyntheticVerificationBackingAdvertisementOverride mode,
                        bool expected_cpu,
                        bool expected_gpu) -> bool {
    RecorderCallbacks cb;
    SyntheticProviderConfig cfg{};
    cfg.endpoint_count = 1;
    cfg.verification_backing_advertisement_override = mode;

    SyntheticProvider provider(cfg);
    if (!provider.initialize(&cb).ok()) {
      std::cerr << "FAIL synthetic backing capability check initialize failed\n";
      return false;
    }

    CaptureProfile profile{};
    PictureConfig picture{};
    CaptureRequest req{};

    const ProducerBackingCapabilities stream_caps = provider.stream_backing_capabilities(profile, picture);
    const ProducerBackingCapabilities capture_caps = provider.capture_backing_capabilities(req);

    const bool stream_ok = (stream_caps.cpu_backed_available == expected_cpu) &&
                           (stream_caps.gpu_backed_available == expected_gpu);
    const bool capture_ok = (capture_caps.cpu_backed_available == expected_cpu) &&
                            (capture_caps.gpu_backed_available == expected_gpu);

    if (!provider.shutdown().ok()) {
      std::cerr << "FAIL synthetic backing capability check shutdown failed\n";
      return false;
    }

    if (!stream_ok || !capture_ok) {
      std::cerr << "FAIL synthetic backing capability advertisement mismatch\n";
      return false;
    }
    return true;
  };

  const bool runtime_gpu_gate = synthetic_gpu_backing_runtime_available();

  if (!verify_mode(SyntheticVerificationBackingAdvertisementOverride::RuntimeTruth, true, runtime_gpu_gate)) return false;
  if (!verify_mode(SyntheticVerificationBackingAdvertisementOverride::ForceCpuOnly, true, false)) return false;
  if (!verify_mode(SyntheticVerificationBackingAdvertisementOverride::ForceCpuAndGpu, true, true)) return false;
  if (!verify_mode(SyntheticVerificationBackingAdvertisementOverride::ForceGpuOnly, false, true)) return false;

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

  const auto cb_events_for_frames = cb.snapshot_events();
  const int f0 = find_frame_index_by_ts(cb_events_for_frames, 0);
  const int f1 = find_frame_index_by_ts(cb_events_for_frames, period_ns);
  const int f2 = find_frame_index_by_ts(cb_events_for_frames, period_ns * 2);
  if (f0 < 0 || f1 < 0 || f2 < 0) {
    std::cerr << "FAIL synthetic picture check frame evidence missing\n";
    (void)synthetic.shutdown();
    return false;
  }

  const uint32_t sig0 = cb_events_for_frames[static_cast<size_t>(f0)].pixel_sig;
  const uint32_t sig1 = cb_events_for_frames[static_cast<size_t>(f1)].pixel_sig;
  const uint32_t sig2 = cb_events_for_frames[static_cast<size_t>(f2)].pixel_sig;
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
  const auto cb_events_after_teardown = cb.snapshot_events();
  return assert_native_balance(cb_events_after_teardown, "synthetic_picture_appearance");
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

  if (!provider.initialize(&cb).ok()) return false;
  if (!provider.open_device("stub0", 1, 1001).ok()) return false;
  if (!provider.create_stream(req).ok()) return false;
  if (!provider.start_stream(req.stream_id, req.profile, req.picture).ok()) return false;
  if (!provider.stop_stream(req.stream_id).ok()) return false;
  if (!provider.destroy_stream(req.stream_id).ok()) return false;
  if (!provider.close_device(req.device_instance_id).ok()) return false;
  if (!provider.shutdown().ok()) return false;

  const auto cb_events = cb.snapshot_events();
  const bool ok = assert_native_balance(cb_events, "stub");
  return ok;
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

  const auto cb_events = cb.snapshot_events();
  const int stopped = find_event_index(cb_events, "stream_stopped", req.stream_id);
  const int destroyed = find_event_index(cb_events, "stream_destroyed", req.stream_id);
  const int closed = find_event_index(cb_events, "device_closed", req.device_instance_id);
  const int stream_native_id = find_native_create_id(cb_events, static_cast<uint32_t>(NativeObjectType::Stream), req.stream_id);
  const int acquisition_session_native_id =
      find_native_create_id_by_type(cb_events, static_cast<uint32_t>(NativeObjectType::AcquisitionSession));
  const int device_native_id = find_native_create_id(cb_events, static_cast<uint32_t>(NativeObjectType::Device), 0);
  const int gpu_backing_created = count_events_by_tag_type_and_owner_stream(
      cb_events,
      "native_created",
      static_cast<uint32_t>(NativeObjectType::GpuBacking),
      req.stream_id);
  const int gpu_backing_destroyed = count_events_by_tag_type_and_owner_stream(
      cb_events,
      "native_destroyed",
      static_cast<uint32_t>(NativeObjectType::GpuBacking),
      req.stream_id);
  const int frame_buffer_lease_created = count_events_by_tag_type_and_owner_stream(
      cb_events,
      "native_created",
      static_cast<uint32_t>(NativeObjectType::FrameBufferLease),
      req.stream_id);
  const int frame_buffer_lease_destroyed = count_events_by_tag_type_and_owner_stream(
      cb_events,
      "native_destroyed",
      static_cast<uint32_t>(NativeObjectType::FrameBufferLease),
      req.stream_id);

  if (stopped < 0 || destroyed < 0 || closed < 0 ||
      stream_native_id < 0 || acquisition_session_native_id < 0 || device_native_id < 0) {
    std::cerr << "FAIL synthetic direct sanity missing callback/native evidence\n";
    return false;
  }
  const bool gpu_backing_realized_in_run = (gpu_backing_created > 0 || gpu_backing_destroyed > 0);
  if (gpu_backing_realized_in_run) {
    if (gpu_backing_created <= 0 || gpu_backing_destroyed <= 0) {
      std::cerr << "FAIL synthetic direct sanity incomplete gpu backing native support lifecycle\n";
      return false;
    }
  }
  const bool frame_buffer_lease_realized_in_run =
      (frame_buffer_lease_created > 0 || frame_buffer_lease_destroyed > 0);
  if (frame_buffer_lease_realized_in_run) {
    if (frame_buffer_lease_created <= 0 || frame_buffer_lease_destroyed <= 0) {
      std::cerr
          << "FAIL synthetic direct sanity incomplete frame buffer lease native support lifecycle\n";
      return false;
    }
  }

  return assert_native_balance(cb_events, "synthetic_direct");
}

bool run_synthetic_still_only_acquisition_session_truth_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);

  if (!provider.initialize(&cb).ok() || !provider.open_device("synthetic:0", 41, 4101).ok()) {
    std::cerr << "FAIL synthetic still-only setup failed\n";
    return false;
  }

  CaptureRequest cap{};
  cap.capture_id = 8001;
  cap.device_instance_id = 41;
  cap.width = 64;
  cap.height = 64;
  cap.format_fourcc = FOURCC_RGBA;
  if (!provider.trigger_capture(cap).ok()) {
    std::cerr << "FAIL synthetic still-only trigger_capture failed\n";
    return false;
  }

  if (!provider.close_device(41).ok() || !provider.shutdown().ok()) {
    std::cerr << "FAIL synthetic still-only teardown failed\n";
    return false;
  }

  const auto cb_events = cb.snapshot_events();
  const int capture_started_ix = find_event_index(cb_events, "capture_started", cap.capture_id);
  const int capture_completed_ix = find_event_index(cb_events, "capture_completed", cap.capture_id);
  const int acq_native_id =
      find_native_create_id_by_type(cb_events, static_cast<uint32_t>(NativeObjectType::AcquisitionSession));
  const int acq_create_ix = find_event_index(cb_events, "native_created", static_cast<uint64_t>(acq_native_id));
  const int acq_destroy_ix = find_event_index(cb_events, "native_destroyed", static_cast<uint64_t>(acq_native_id));

  if (capture_started_ix < 0 || capture_completed_ix < 0 || acq_native_id < 0 ||
      acq_create_ix < 0 || acq_destroy_ix < 0) {
    std::cerr << "FAIL synthetic still-only missing capture/session evidence\n";
    return false;
  }
  if (!(acq_create_ix < capture_started_ix &&
        capture_started_ix < capture_completed_ix &&
        capture_completed_ix < acq_destroy_ix)) {
    std::cerr << "FAIL synthetic still-only lifecycle ordering invalid\n";
    return false;
  }
  int frame_count = 0;
  for (const auto& ev : cb_events) {
    if (ev.tag == "frame") {
      ++frame_count;
      if (ev.capture_image_routing != CaptureImageRouting::DEFAULT_METERED) {
        std::cerr << "FAIL synthetic still-only frame routing expected DEFAULT_METERED\n";
        return false;
      }
    }
  }
  if (frame_count != 1) {
    std::cerr << "FAIL synthetic still-only expected exactly one frame\n";
    return false;
  }
  if (count_events_by_tag_and_type(
          cb_events, "native_created", static_cast<uint32_t>(NativeObjectType::Stream)) != 0) {
    std::cerr << "FAIL synthetic still-only unexpectedly realized stream native object\n";
    return false;
  }
  return assert_native_balance(cb_events, "synthetic_still_only");
}

bool run_synthetic_multi_member_still_sequence_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);

  if (!provider.initialize(&cb).ok() || !provider.open_device("synthetic:0", 43, 4301).ok()) {
    std::cerr << "FAIL synthetic multi-member setup failed\n";
    return false;
  }

  CaptureRequest cap{};
  cap.capture_id = 8003;
  cap.device_instance_id = 43;
  cap.width = 64;
  cap.height = 64;
  cap.format_fourcc = FOURCC_RGBA;
  cap.still_image_bundle = make_default_metered_still_image_bundle();
  cap.still_image_bundle.members.push_back(
      CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, 1000});
  if (!is_valid_capture_still_image_bundle(cap.still_image_bundle, provider.supports_multi_image_still_sequence())) {
    std::cerr << "FAIL synthetic multi-member request validation rejected expected valid sequence\n";
    return false;
  }
  if (!provider.trigger_capture(cap).ok()) {
    std::cerr << "FAIL synthetic multi-member trigger_capture failed\n";
    return false;
  }
  if (!provider.close_device(43).ok() || !provider.shutdown().ok()) {
    std::cerr << "FAIL synthetic multi-member teardown failed\n";
    return false;
  }

  int started_count = 0;
  int completed_count = 0;
  std::vector<EventRec> frame_events;
  const auto cb_events = cb.snapshot_events();
  for (const auto& ev : cb_events) {
    if (ev.tag == "capture_started" && ev.id == cap.capture_id) ++started_count;
    if (ev.tag == "capture_completed" && ev.id == cap.capture_id) ++completed_count;
    if (ev.tag == "frame") frame_events.push_back(ev);
  }
  if (started_count != 1 || completed_count != 1 || frame_events.size() != 2) {
    std::cerr << "FAIL synthetic multi-member lifecycle/frame count mismatch\n";
    return false;
  }
  if (frame_events[0].capture_image_routing != CaptureImageRouting::DEFAULT_METERED ||
      frame_events[1].capture_image_routing != CaptureImageRouting::ADDITIONAL_BRACKET) {
    std::cerr << "FAIL synthetic multi-member routing mismatch\n";
    return false;
  }
  if (frame_events[0].capture_image_member_index != 0 ||
      frame_events[0].capture_image_applied_exposure_compensation_milli_ev != 0 ||
      !frame_events[0].capture_image_has_realized_exposure_compensation_milli_ev ||
      frame_events[0].capture_image_realized_exposure_compensation_milli_ev !=
          frame_events[0].capture_image_applied_exposure_compensation_milli_ev ||
      frame_events[1].capture_image_member_index != 1 ||
      frame_events[1].capture_image_applied_exposure_compensation_milli_ev != 1000 ||
      !frame_events[1].capture_image_has_realized_exposure_compensation_milli_ev ||
      frame_events[1].capture_image_realized_exposure_compensation_milli_ev !=
          frame_events[1].capture_image_applied_exposure_compensation_milli_ev) {
    std::cerr << "FAIL synthetic multi-member emitted metadata mismatch\n";
    return false;
  }
  if (frame_events[0].payload_size_bytes == 0 || frame_events[1].payload_size_bytes == 0) {
    std::cerr << "FAIL synthetic multi-member expected non-empty payloads\n";
    return false;
  }
  if (frame_events[0].payload_size_bytes != frame_events[1].payload_size_bytes) {
    std::cerr << "FAIL synthetic multi-member expected equal-sized payloads\n";
    return false;
  }
  if (frame_events[0].payload_hash == frame_events[1].payload_hash) {
    std::cerr << "FAIL synthetic multi-member expected deterministic payload hash difference\n";
    return false;
  }
  return assert_native_balance(cb_events, "synthetic_multi_member_still_sequence");
}

bool run_synthetic_dynamic_still_bundle_shape_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);
  if (!provider.initialize(&cb).ok() || !provider.open_device("synthetic:0", 144, 14401).ok()) {
    std::cerr << "FAIL synthetic dynamic bundle setup failed\n";
    return false;
  }
  auto fail_with_cleanup = [&](const char* msg) -> bool {
    std::cerr << msg << "\n";
    (void)provider.close_device(144);
    (void)provider.shutdown();
    return false;
  };
  auto run_capture_and_collect = [&](uint64_t capture_id,
                                     const std::vector<int32_t>& member_evs,
                                     std::vector<EventRec>& out_frames) -> bool {
    CaptureRequest req{};
    req.capture_id = capture_id;
    req.device_instance_id = 144;
    req.width = 64;
    req.height = 64;
    req.format_fourcc = FOURCC_RGBA;
    req.still_image_bundle = make_default_metered_still_image_bundle();
    for (size_t i = 1; i < member_evs.size(); ++i) {
      req.still_image_bundle.members.push_back(CaptureStillImageMember{
          static_cast<uint32_t>(i),
          CaptureStillImageMemberRole::ADDITIONAL_BRACKET,
          member_evs[i]});
    }
    if (!is_valid_capture_still_image_bundle(req.still_image_bundle, provider.supports_multi_image_still_sequence())) {
      return false;
    }
    if (!provider.trigger_capture(req).ok()) {
      return false;
    }
    bool completed = false;
    for (int iter = 0; iter < kMaxIters; ++iter) {
      size_t matched_frames = 0;
      completed = false;
      const auto events_snapshot = cb.snapshot_events();
      for (const auto& ev : events_snapshot) {
        if (ev.tag == "capture_completed" && ev.id == capture_id) {
          completed = true;
        }
        if (ev.tag == "frame" &&
            ev.capture_id == capture_id &&
            ev.capture_image_member_index < member_evs.size() &&
            ev.capture_image_has_realized_exposure_compensation_milli_ev &&
            ev.capture_image_realized_exposure_compensation_milli_ev == ev.capture_image_applied_exposure_compensation_milli_ev &&
            ev.capture_image_applied_exposure_compensation_milli_ev == member_evs[ev.capture_image_member_index]) {
          matched_frames++;
        }
      }
      if (completed && matched_frames >= member_evs.size()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kSleepMs));
    }
    if (!completed) {
      return false;
    }
    out_frames.clear();
    const auto events_snapshot = cb.snapshot_events();
    for (const auto& ev : events_snapshot) {
      if (ev.tag == "frame" &&
          ev.capture_id == capture_id &&
          ev.capture_image_member_index < member_evs.size() &&
          ev.capture_image_has_realized_exposure_compensation_milli_ev &&
          ev.capture_image_realized_exposure_compensation_milli_ev == ev.capture_image_applied_exposure_compensation_milli_ev &&
          ev.capture_image_applied_exposure_compensation_milli_ev == member_evs[ev.capture_image_member_index]) {
        out_frames.push_back(ev);
      }
    }
    return out_frames.size() >= member_evs.size();
  };

  {
    const std::vector<int32_t> evs{0};
    std::vector<EventRec> frames;
    if (!run_capture_and_collect(8101, evs, frames)) {
      return fail_with_cleanup("FAIL synthetic dynamic one-member capture failed");
    }
  }
  {
    const std::vector<int32_t> evs{0, -1000, 2000};
    std::vector<EventRec> frames;
    if (!run_capture_and_collect(8102, evs, frames)) {
      return fail_with_cleanup("FAIL synthetic dynamic asymmetric capture failed");
    }
    if (frames.size() < evs.size()) {
      return fail_with_cleanup("FAIL synthetic dynamic asymmetric expected member count");
    }
    for (size_t i = 0; i < evs.size(); ++i) {
      const auto& frame = frames[frames.size() - evs.size() + i];
      const CaptureImageRouting expected_routing = (i == 0)
          ? CaptureImageRouting::DEFAULT_METERED
          : CaptureImageRouting::ADDITIONAL_BRACKET;
      if (frame.capture_image_routing != expected_routing ||
          frame.capture_image_member_index != i ||
          !frame.capture_image_has_realized_exposure_compensation_milli_ev ||
          frame.capture_image_realized_exposure_compensation_milli_ev != frame.capture_image_applied_exposure_compensation_milli_ev ||
          frame.capture_image_applied_exposure_compensation_milli_ev != evs[i]) {
        return fail_with_cleanup("FAIL synthetic dynamic asymmetric member metadata mismatch");
      }
    }
    const auto& f0 = frames[frames.size() - 3];
    const auto& f1 = frames[frames.size() - 2];
    const auto& f2 = frames[frames.size() - 1];
    const double y0 = f0.sampled_luma;
    const double y1 = f1.sampled_luma;
    const double y2 = f2.sampled_luma;
    if (!(y1 < y0 && y2 > y0)) {
      std::cerr << "FAIL synthetic dynamic asymmetric expected deterministic EV brightness ordering"
                << " [m0 idx=" << f0.capture_image_member_index << " ev=" << f0.capture_image_applied_exposure_compensation_milli_ev
                << " fmt=" << f0.format_fourcc << " luma=" << y0
                << " rgb=(" << static_cast<int>(f0.sample_r) << "," << static_cast<int>(f0.sample_g) << "," << static_cast<int>(f0.sample_b) << ")]"
                << " [m1 idx=" << f1.capture_image_member_index << " ev=" << f1.capture_image_applied_exposure_compensation_milli_ev
                << " fmt=" << f1.format_fourcc << " luma=" << y1
                << " rgb=(" << static_cast<int>(f1.sample_r) << "," << static_cast<int>(f1.sample_g) << "," << static_cast<int>(f1.sample_b) << ")]"
                << " [m2 idx=" << f2.capture_image_member_index << " ev=" << f2.capture_image_applied_exposure_compensation_milli_ev
                << " fmt=" << f2.format_fourcc << " luma=" << y2
                << " rgb=(" << static_cast<int>(f2.sample_r) << "," << static_cast<int>(f2.sample_g) << "," << static_cast<int>(f2.sample_b) << ")]\n";
      return fail_with_cleanup("FAIL synthetic dynamic asymmetric expected deterministic EV brightness ordering");
    }
  }
  {
    const std::vector<int32_t> evs{0, -2000, -1000, 1000, 2000};
    std::vector<EventRec> frames;
    if (!run_capture_and_collect(8103, evs, frames)) {
      return fail_with_cleanup("FAIL synthetic dynamic large bundle capture failed");
    }
    if (frames.size() < evs.size()) {
      return fail_with_cleanup("FAIL synthetic dynamic large bundle expected member count");
    }
    for (size_t i = 0; i < evs.size(); ++i) {
      const auto& frame = frames[frames.size() - evs.size() + i];
      if (frame.capture_image_member_index != i ||
          !frame.capture_image_has_realized_exposure_compensation_milli_ev ||
          frame.capture_image_realized_exposure_compensation_milli_ev != frame.capture_image_applied_exposure_compensation_milli_ev ||
          frame.capture_image_applied_exposure_compensation_milli_ev != evs[i]) {
        return fail_with_cleanup("FAIL synthetic dynamic large bundle member order/metadata mismatch");
      }
    }
  }
  if (!provider.close_device(144).ok() || !provider.shutdown().ok()) {
    return fail_with_cleanup("FAIL synthetic dynamic bundle teardown failed");
  }
  const auto cb_events = cb.snapshot_events();
  return assert_native_balance(cb_events, "synthetic_dynamic_still_bundle_shape");
}

bool run_core_synthetic_three_member_capture_result_check() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "FAIL core synthetic three-member runtime start failed\n";
    return false;
  }

  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);
  bool provider_initialized = false;
  bool device_opened = false;
  const auto fail_with_cleanup = [&](const char* msg) -> bool {
    std::cerr << msg << "\n";
    if (device_opened) {
      (void)provider.close_device(64);
      device_opened = false;
    }
    if (provider_initialized) {
      (void)provider.shutdown();
      provider_initialized = false;
    }
    rt.stop();
    return false;
  };
  rt.attach_provider(&provider);
  if (!provider.initialize(rt.provider_callbacks()).ok()) {
    return fail_with_cleanup("FAIL core synthetic three-member provider init failed");
  }
  provider_initialized = true;
  std::vector<CameraEndpoint> eps;
  if (!provider.enumerate_endpoints(eps).ok() || eps.empty()) {
    return fail_with_cleanup("FAIL core synthetic three-member enumerate failed");
  }

  const uint64_t device_id = 64;
  if (!provider.open_device(eps[0].hardware_id, device_id, 6401).ok()) {
    return fail_with_cleanup("FAIL core synthetic three-member open_device failed");
  }
  device_opened = true;

  CaptureRequest req{};
  for (int i = 0; i < 50; ++i) {
    if (rt.materialize_capture_request(device_id, req)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (req.device_instance_id != device_id) {
    return fail_with_cleanup("FAIL core synthetic three-member materialize request failed");
  }
  CaptureRequest default_req = req;
  default_req.capture_id = 9600;
  if (!provider.trigger_capture(default_req).ok()) {
    return fail_with_cleanup("FAIL core synthetic default trigger_capture failed");
  }
  SharedCaptureResultData default_result;
  for (int i = 0; i < 50; ++i) {
    default_result = rt.get_capture_result(default_req.capture_id, device_id);
    if (default_result) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!default_result || default_result->image_member_count() != 1 || default_result->has_additional_images()) {
    return fail_with_cleanup("FAIL core synthetic default retained result member-count/additional mismatch");
  }

  req.capture_id = 9601;
  req.still_image_bundle = make_default_metered_still_image_bundle();
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, -1000});
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{2u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, +1000});
  if (!provider.trigger_capture(req).ok()) {
    return fail_with_cleanup("FAIL core synthetic three-member trigger_capture failed");
  }

  SharedCaptureResultData result;
  for (int i = 0; i < 50; ++i) {
    result = rt.get_capture_result(req.capture_id, device_id);
    if (result) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!result) {
    return fail_with_cleanup("FAIL core synthetic three-member result missing");
  }
  if (result->default_image.image_member_index != 0 ||
      result->default_image.role != CoreCaptureResultData::ImageMemberRole::DEFAULT_METERED ||
      result->default_image.applied_exposure_compensation_milli_ev != 0 ||
      !result->default_image.has_realized_exposure_compensation_milli_ev ||
      result->default_image.realized_exposure_compensation_milli_ev !=
          result->default_image.applied_exposure_compensation_milli_ev) {
    return fail_with_cleanup("FAIL core synthetic three-member default member metadata mismatch");
  }
  if (result->additional_images.size() != 2 ||
      result->additional_images[0].image_member_index != 1 ||
      result->additional_images[0].role != CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET ||
      result->additional_images[0].applied_exposure_compensation_milli_ev != -1000 ||
      !result->additional_images[0].has_realized_exposure_compensation_milli_ev ||
      result->additional_images[0].realized_exposure_compensation_milli_ev !=
          result->additional_images[0].applied_exposure_compensation_milli_ev ||
      result->additional_images[1].image_member_index != 2 ||
      result->additional_images[1].role != CoreCaptureResultData::ImageMemberRole::ADDITIONAL_BRACKET ||
      result->additional_images[1].applied_exposure_compensation_milli_ev != 1000 ||
      !result->additional_images[1].has_realized_exposure_compensation_milli_ev ||
      result->additional_images[1].realized_exposure_compensation_milli_ev !=
          result->additional_images[1].applied_exposure_compensation_milli_ev) {
    return fail_with_cleanup("FAIL core synthetic three-member additional member metadata mismatch");
  }
  if (result->image_member_count() != 3 || !result->has_additional_images()) {
    return fail_with_cleanup("FAIL core synthetic three-member member-count/additional-image contract mismatch");
  }
  const auto* m0 = result->image_member_at(0);
  const auto* m1 = result->image_member_at(1);
  const auto* m2 = result->image_member_at(2);
  const auto* m3 = result->image_member_at(3);
  if (!m0 || !m1 || !m2 || m3 != nullptr) {
    return fail_with_cleanup("FAIL core synthetic three-member image_member_at access contract mismatch");
  }
  if (m0->image_member_index != 0 || m1->image_member_index != 1 || m2->image_member_index != 2 ||
      m0->applied_exposure_compensation_milli_ev != 0 ||
      m1->applied_exposure_compensation_milli_ev != -1000 ||
      m2->applied_exposure_compensation_milli_ev != 1000 ||
      !m0->has_realized_exposure_compensation_milli_ev ||
      !m1->has_realized_exposure_compensation_milli_ev ||
      !m2->has_realized_exposure_compensation_milli_ev ||
      m0->realized_exposure_compensation_milli_ev != m0->applied_exposure_compensation_milli_ev ||
      m1->realized_exposure_compensation_milli_ev != m1->applied_exposure_compensation_milli_ev ||
      m2->realized_exposure_compensation_milli_ev != m2->applied_exposure_compensation_milli_ev) {
    return fail_with_cleanup("FAIL core synthetic three-member retained member index/ev mismatch");
  }

  const auto& d = result->default_image.payload.bytes;
  const auto& b1 = result->additional_images[0].payload.bytes;
  const auto& b2 = result->additional_images[1].payload.bytes;
  if (d.empty() || b1.empty() || b2.empty()) {
    return fail_with_cleanup("FAIL core synthetic three-member expected non-empty payloads");
  }
  if (!(d.size() == b1.size() && b1.size() == b2.size())) {
    return fail_with_cleanup("FAIL core synthetic three-member expected equal-sized payloads");
  }
  const uint64_t h0 = fnv1a64_hash_bytes(d.data(), d.size());
  const uint64_t h1 = fnv1a64_hash_bytes(b1.data(), b1.size());
  const uint64_t h2 = fnv1a64_hash_bytes(b2.data(), b2.size());
  if (h0 == h1 || h0 == h2 || h1 == h2) {
    return fail_with_cleanup("FAIL core synthetic three-member expected all payload hashes to differ");
  }
  // NOTE: CamBANGCaptureResult wrapper/member-access behavior is intentionally
  // verified in Godot/GDE-specific validation surfaces; this provider smoke
  // remains core/provider-only and must not depend on godot-cpp headers.

  if (!provider.close_device(device_id).ok()) {
    return fail_with_cleanup("FAIL core synthetic three-member close_device failed");
  }
  device_opened = false;
  if (!provider.shutdown().ok()) {
    return fail_with_cleanup("FAIL core synthetic three-member provider shutdown failed");
  }
  provider_initialized = false;
  rt.stop();
  return true;
}

bool run_core_synthetic_three_member_capture_result_realized_ev_mismatch_check() {
  // First, verify callback metadata divergence on direct provider callbacks.
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  cfg.verification_realized_exposure_compensation_override_by_member_index[2u] = 750;
  SyntheticProvider provider(cfg);
  if (!provider.initialize(&cb).ok() ||
      !provider.open_device("synthetic:0", 6500, 65001).ok()) {
    std::cerr << "FAIL core synthetic mismatch callback provider init/open failed\n";
    return false;
  }
  CaptureRequest cb_req{};
  cb_req.capture_id = 9700;
  cb_req.device_instance_id = 6500;
  cb_req.width = 64;
  cb_req.height = 64;
  cb_req.format_fourcc = FOURCC_RGBA;
  cb_req.still_image_bundle = make_default_metered_still_image_bundle();
  cb_req.still_image_bundle.members.push_back(
      CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, -1000});
  cb_req.still_image_bundle.members.push_back(
      CaptureStillImageMember{2u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, +1000});
  if (!provider.trigger_capture(cb_req).ok() ||
      !provider.close_device(6500).ok() ||
      !provider.shutdown().ok()) {
    std::cerr << "FAIL core synthetic mismatch callback trigger/teardown failed\n";
    return false;
  }
  const auto callback_events = cb.snapshot_events();
  std::vector<EventRec> cap_frames;
  for (const auto& e : callback_events) {
    if (e.tag == "frame" && e.capture_id == cb_req.capture_id) {
      cap_frames.push_back(e);
    }
  }
  if (cap_frames.size() != 3) {
    std::cerr << "FAIL core synthetic mismatch callback expected three frame members\n";
    return false;
  }
  std::sort(cap_frames.begin(), cap_frames.end(), [](const EventRec& a, const EventRec& b) {
    return a.capture_image_member_index < b.capture_image_member_index;
  });
  for (size_t i = 0; i < cap_frames.size(); ++i) {
    const auto& frame = cap_frames[i];
    if (frame.capture_image_member_index != i ||
        !frame.capture_image_has_realized_exposure_compensation_milli_ev) {
      std::cerr << "FAIL core synthetic mismatch callback member index/realized-presence mismatch\n";
      return false;
    }
  }
  if (cap_frames[0].capture_image_applied_exposure_compensation_milli_ev != 0 ||
      cap_frames[0].capture_image_realized_exposure_compensation_milli_ev != 0 ||
      cap_frames[1].capture_image_applied_exposure_compensation_milli_ev != -1000 ||
      cap_frames[1].capture_image_realized_exposure_compensation_milli_ev != -1000 ||
      cap_frames[2].capture_image_applied_exposure_compensation_milli_ev != 1000 ||
      cap_frames[2].capture_image_realized_exposure_compensation_milli_ev != 750 ||
      cap_frames[2].capture_image_realized_exposure_compensation_milli_ev ==
          cap_frames[2].capture_image_applied_exposure_compensation_milli_ev) {
    std::cerr << "FAIL core synthetic mismatch callback member EV truth mismatch\n";
    return false;
  }

  // Then verify retained/Core result metadata divergence with normal shape/order.
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "FAIL core synthetic mismatch runtime start failed\n";
    return false;
  }
  SyntheticProvider core_provider(cfg);
  bool provider_initialized = false;
  bool device_opened = false;
  const auto fail_with_cleanup = [&](const char* msg) -> bool {
    std::cerr << msg << "\n";
    if (device_opened) {
      (void)core_provider.close_device(65);
      device_opened = false;
    }
    if (provider_initialized) {
      (void)core_provider.shutdown();
      provider_initialized = false;
    }
    rt.stop();
    return false;
  };
  rt.attach_provider(&core_provider);
  if (!core_provider.initialize(rt.provider_callbacks()).ok()) {
    return fail_with_cleanup("FAIL core synthetic mismatch provider init failed");
  }
  provider_initialized = true;
  std::vector<CameraEndpoint> eps;
  if (!core_provider.enumerate_endpoints(eps).ok() || eps.empty()) {
    return fail_with_cleanup("FAIL core synthetic mismatch enumerate failed");
  }

  const uint64_t device_id = 65;
  if (!core_provider.open_device(eps[0].hardware_id, device_id, 6501).ok()) {
    return fail_with_cleanup("FAIL core synthetic mismatch open_device failed");
  }
  device_opened = true;

  CaptureRequest req{};
  for (int i = 0; i < 50; ++i) {
    if (rt.materialize_capture_request(device_id, req)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (req.device_instance_id != device_id) {
    return fail_with_cleanup("FAIL core synthetic mismatch materialize request failed");
  }

  req.capture_id = 9701;
  req.still_image_bundle = make_default_metered_still_image_bundle();
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, -1000});
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{2u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, +1000});
  if (!core_provider.trigger_capture(req).ok()) {
    return fail_with_cleanup("FAIL core synthetic mismatch trigger_capture failed");
  }

  SharedCaptureResultData result;
  for (int i = 0; i < 50; ++i) {
    result = rt.get_capture_result(req.capture_id, device_id);
    if (result) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!result) {
    return fail_with_cleanup("FAIL core synthetic mismatch result missing");
  }
  if (result->image_member_count() != 3 || !result->has_additional_images()) {
    return fail_with_cleanup("FAIL core synthetic mismatch member-count/additional-image contract mismatch");
  }
  const auto* m0 = result->image_member_at(0);
  const auto* m1 = result->image_member_at(1);
  const auto* m2 = result->image_member_at(2);
  if (!m0 || !m1 || !m2) {
    return fail_with_cleanup("FAIL core synthetic mismatch retained members missing");
  }
  if (m0->applied_exposure_compensation_milli_ev != 0 ||
      !m0->has_realized_exposure_compensation_milli_ev ||
      m0->realized_exposure_compensation_milli_ev != m0->applied_exposure_compensation_milli_ev ||
      m1->applied_exposure_compensation_milli_ev != -1000 ||
      !m1->has_realized_exposure_compensation_milli_ev ||
      m1->realized_exposure_compensation_milli_ev != m1->applied_exposure_compensation_milli_ev ||
      m2->applied_exposure_compensation_milli_ev != 1000 ||
      !m2->has_realized_exposure_compensation_milli_ev ||
      m2->realized_exposure_compensation_milli_ev != 750 ||
      m2->realized_exposure_compensation_milli_ev == m2->applied_exposure_compensation_milli_ev) {
    return fail_with_cleanup("FAIL core synthetic mismatch retained member EV truth mismatch");
  }
  if (result->default_image.payload.bytes.empty() ||
      result->additional_images[0].payload.bytes.empty() ||
      result->additional_images[1].payload.bytes.empty()) {
    return fail_with_cleanup("FAIL core synthetic mismatch expected non-empty payloads");
  }

  if (!core_provider.close_device(device_id).ok()) {
    return fail_with_cleanup("FAIL core synthetic mismatch close_device failed");
  }
  device_opened = false;
  if (!core_provider.shutdown().ok()) {
    return fail_with_cleanup("FAIL core synthetic mismatch provider shutdown failed");
  }
  provider_initialized = false;
  rt.stop();
  return true;
}

bool run_synthetic_stream_plus_still_single_session_truth_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);

  StreamRequest req{};
  req.stream_id = 72;
  req.device_instance_id = 42;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 64;
  req.profile.height = 64;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  CaptureRequest cap{};
  cap.capture_id = 8002;
  cap.device_instance_id = req.device_instance_id;
  cap.width = 64;
  cap.height = 64;
  cap.format_fourcc = FOURCC_RGBA;

  if (!provider.initialize(&cb).ok() ||
      !provider.open_device("synthetic:0", req.device_instance_id, 4201).ok() ||
      !provider.create_stream(req).ok() ||
      !provider.start_stream(req.stream_id, req.profile, req.picture).ok() ||
      !provider.trigger_capture(cap).ok() ||
      !provider.stop_stream(req.stream_id).ok() ||
      !provider.destroy_stream(req.stream_id).ok() ||
      !provider.close_device(req.device_instance_id).ok() ||
      !provider.shutdown().ok()) {
    std::cerr << "FAIL synthetic stream+still setup/teardown failed\n";
    return false;
  }

  const auto cb_events = cb.snapshot_events();
  const int capture_started_ix = find_event_index(cb_events, "capture_started", cap.capture_id);
  const int capture_completed_ix = find_event_index(cb_events, "capture_completed", cap.capture_id);
  const int stream_destroy_ix = find_event_index(cb_events, "stream_destroyed", req.stream_id);
  const int acq_native_id =
      find_native_create_id_by_type(cb_events, static_cast<uint32_t>(NativeObjectType::AcquisitionSession));
  const int acq_create_ix = find_event_index(cb_events, "native_created", static_cast<uint64_t>(acq_native_id));
  const int acq_destroy_ix = find_event_index(cb_events, "native_destroyed", static_cast<uint64_t>(acq_native_id));
  const int acq_create_count = count_events_by_tag_and_type(
      cb_events, "native_created", static_cast<uint32_t>(NativeObjectType::AcquisitionSession));

  if (capture_started_ix < 0 || capture_completed_ix < 0 || stream_destroy_ix < 0 ||
      acq_native_id < 0 || acq_create_ix < 0 || acq_destroy_ix < 0) {
    std::cerr << "FAIL synthetic stream+still missing evidence\n";
    return false;
  }
  if (acq_create_count != 1) {
    std::cerr << "FAIL synthetic stream+still realized multiple acquisition sessions for one device\n";
    return false;
  }
  if (!(acq_create_ix < capture_started_ix &&
      capture_started_ix < capture_completed_ix &&
      capture_completed_ix < acq_destroy_ix)) {
    std::cerr << "FAIL synthetic stream+still ordering invalid\n";
    return false;
  }
  return assert_native_balance(cb_events, "synthetic_stream_plus_still");
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_opts(argc, argv, opt)) {
    return 2;
  }

  // 1) Materialization / loader compliance.
  if (!run_named_check("run_synthetic_scenario_materialization_check", [] { return run_synthetic_scenario_materialization_check(); })) return 1;
  if (!run_named_check("run_synthetic_builtin_scenario_library_build_check", [] { return run_synthetic_builtin_scenario_library_build_check(); })) return 1;
  if (!run_named_check("run_synthetic_external_scenario_loader_check", [] { return run_synthetic_external_scenario_loader_check(); })) return 1;
  if (!run_named_check("run_synthetic_external_scenario_loader_negative_check", [] { return run_synthetic_external_scenario_loader_negative_check(); })) return 1;

  // 2) Primitive lifecycle foundation.
  if (!run_named_check("run_synthetic_primitive_lifecycle_foundation_check", [] { return run_synthetic_primitive_lifecycle_foundation_check(); })) return 1;

  // 3) Clustered strict/gated destructive sequencing interpretation.
  if (!run_named_check("run_clustered_strict_branch_check", [] { return run_clustered_strict_branch_check(); })) return 1;
  if (!run_named_check("run_clustered_completion_gated_branch_check", [] { return run_clustered_completion_gated_branch_check(); })) return 1;

  // 4) Broker / host surface compliance.
  if (!run_named_check("run_broker_timeline_host_surface_check", [] { return run_broker_timeline_host_surface_check(); })) return 1;

  // 5) Synthetic backing capability advertisement seam checks.
  if (!run_named_check("run_synthetic_backing_capability_advertisement_check", [] { return run_synthetic_backing_capability_advertisement_check(); })) return 1;

  // 6) Synthetic frame/picture integration checks.
  if (!run_named_check("run_synthetic_timeline_picture_appearance_check", [] { return run_synthetic_timeline_picture_appearance_check(); })) return 1;

  // Additional provider direct sanity coverage retained.
  if (!run_named_check("run_stub_provider_sanity_check", [] { return run_stub_provider_sanity_check(); })) return 1;
  if (!run_named_check("run_synthetic_provider_direct_sanity_check", [] { return run_synthetic_provider_direct_sanity_check(); })) return 1;
  if (!run_named_check("run_synthetic_still_only_acquisition_session_truth_check", [] { return run_synthetic_still_only_acquisition_session_truth_check(); })) return 1;
  if (!run_named_check("run_synthetic_multi_member_still_sequence_check", [] { return run_synthetic_multi_member_still_sequence_check(); })) return 1;
  if (!run_named_check("run_synthetic_dynamic_still_bundle_shape_check", [] { return run_synthetic_dynamic_still_bundle_shape_check(); })) return 1;
  if (!run_named_check("run_core_synthetic_three_member_capture_result_check", [] { return run_core_synthetic_three_member_capture_result_check(); })) return 1;
  if (!run_named_check("run_core_synthetic_three_member_capture_result_realized_ev_mismatch_check", [] { return run_core_synthetic_three_member_capture_result_realized_ev_mismatch_check(); })) return 1;
  if (!run_named_check("run_synthetic_stream_plus_still_single_session_truth_check", [] { return run_synthetic_stream_plus_still_single_session_truth_check(); })) return 1;

  // 7) External scenario file path (first-class, optional input).
  if (!opt.external_scenario_file.empty()) {
    if (!run_named_check("run_external_scenario_file_execution_check", [&] { return run_external_scenario_file_execution_check(opt.external_scenario_file); })) return 1;
  }

  std::cout << "PASS provider_compliance_verify\n";
  return 0;
}
