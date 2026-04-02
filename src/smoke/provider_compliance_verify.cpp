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
#include "imaging/stub/provider.h"
#include "imaging/synthetic/provider.h"

using namespace cambang;

namespace {

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
  const uint64_t before = harness.runtime().published_seq();
  synthetic.advance(dt_ns);
  harness.runtime().request_publish();
  if (!harness.wait_for_core_publish_count(before + 1, error, 500, 5)) {
    return false;
  }
  return harness.wait_for_core_snapshot(predicate, error, 500, 5, timeout_message);
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
  const uint64_t invalid_stream_id = 40;
  const uint64_t stream_id = 32;

  SyntheticScheduledEvent ev{};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::CreateStream;
  ev.device_instance_id = device_id;
  ev.stream_id = invalid_stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 0;
  ev.type = SyntheticEventType::StartStream;
  ev.stream_id = invalid_stream_id;
  scenario.events.push_back(ev);

  ev = {};
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
  ev.at_ns = 1;
  ev.type = SyntheticEventType::DestroyStream; // invalid while started
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 1;
  ev.type = SyntheticEventType::CloseDevice; // invalid while child stream exists
  ev.device_instance_id = device_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 2;
  ev.type = SyntheticEventType::StopStream;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 3;
  ev.type = SyntheticEventType::DestroyStream;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = 4;
  ev.type = SyntheticEventType::CloseDevice;
  ev.device_instance_id = device_id;
  scenario.events.push_back(ev);

  if (!synthetic->set_timeline_scenario_for_host(scenario).ok()) return false;
  if (!synthetic->start_timeline_scenario_for_host().ok()) return false;

  if (!advance_and_expect_snapshot(harness, *synthetic, 0, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return VerifyCaseHarness::has_device(s, device_id) &&
               stream && stream->mode == CBStreamMode::FLOWING &&
               !VerifyCaseHarness::has_stream(s, invalid_stream_id);
      }, error, "timed out waiting for invalid-order t=0")) {
    std::cerr << "FAIL synthetic timeline invalid-order open/create/start boundary: " << error << "\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return VerifyCaseHarness::has_device(s, device_id) &&
               stream && stream->mode == CBStreamMode::FLOWING;
      }, error, "timed out waiting for invalid-order rejected destroy/close")) {
    std::cerr << "FAIL synthetic timeline invalid-order invalid destroy/close self-healed\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return stream && stream->mode == CBStreamMode::STOPPED;
      }, error, "timed out waiting for invalid-order stop")) {
    std::cerr << "FAIL synthetic timeline invalid-order stop missing\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        return !VerifyCaseHarness::has_stream(s, stream_id) &&
               VerifyCaseHarness::has_device(s, device_id);
      }, error, "timed out waiting for invalid-order destroy")) {
    std::cerr << "FAIL synthetic timeline invalid-order destroy missing\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        return !VerifyCaseHarness::has_stream(s, stream_id) &&
               !VerifyCaseHarness::has_device(s, device_id);
      }, error, "timed out waiting for invalid-order close")) {
    std::cerr << "FAIL synthetic timeline invalid-order close missing\n";
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

bool run_synthetic_timeline_picture_appearance_check() {
  VerifyCaseHarness harness(VerifyCaseProviderKind::Synthetic);
  std::string error;
  if (!harness.start_runtime(error)) {
    std::cerr << "FAIL synthetic timeline picture harness start: " << error << "\n";
    return false;
  }
  auto* synthetic = dynamic_cast<SyntheticProvider*>(harness.runtime().attached_provider());
  if (!synthetic) {
    std::cerr << "FAIL synthetic timeline picture provider cast failed\n";
    harness.stop_runtime();
    return false;
  }

  const uint64_t device_id = 51;
  const uint64_t root_id = 5201;
  const uint64_t stream_id = 52;
  const uint64_t period_ns = 1'000'000'000ull / 30ull;

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
  ev.at_ns = period_ns;
  ev.type = SyntheticEventType::UpdateStreamPicture;
  ev.stream_id = stream_id;
  ev.has_picture = true;
  ev.picture.preset = PatternPreset::Solid;
  ev.picture.overlay_frame_index_offsets = false;
  ev.picture.overlay_moving_bar = false;
  ev.picture.solid_r = 25;
  ev.picture.solid_g = 200;
  ev.picture.solid_b = 75;
  ev.picture.solid_a = 255;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns * 3;
  ev.type = SyntheticEventType::StopStream;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns * 3 + 1;
  ev.type = SyntheticEventType::DestroyStream;
  ev.stream_id = stream_id;
  scenario.events.push_back(ev);

  ev = {};
  ev.at_ns = period_ns * 3 + 2;
  ev.type = SyntheticEventType::CloseDevice;
  ev.device_instance_id = device_id;
  scenario.events.push_back(ev);

  if (!synthetic->set_timeline_scenario_for_host(scenario).ok()) return false;
  if (!synthetic->start_timeline_scenario_for_host().ok()) return false;

  if (!advance_and_expect_snapshot(harness, *synthetic, 0, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return stream && stream->mode == CBStreamMode::FLOWING && stream->frames_received >= 1;
      }, error, "timed out waiting for first frame")) {
    std::cerr << "FAIL synthetic timeline picture first frame missing\n";
    return false;
  }
  auto snap0 = harness.snapshot_buffer().snapshot_copy();
  if (!snap0) return false;
  const auto* stream0 = VerifyCaseHarness::find_stream(*snap0, stream_id);
  if (!stream0) return false;
  const uint64_t frames0 = stream0->frames_received;

  if (!advance_and_expect_snapshot(harness, *synthetic, period_ns, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return stream && stream->frames_received > frames0;
      }, error, "timed out waiting for post-picture-update frame")) {
    std::cerr << "FAIL synthetic timeline picture update halted frame flow\n";
    return false;
  }
  auto snap1 = harness.snapshot_buffer().snapshot_copy();
  if (!snap1) return false;
  const auto* stream1 = VerifyCaseHarness::find_stream(*snap1, stream_id);
  if (!stream1) return false;
  const uint64_t frames1 = stream1->frames_received;

  if (!advance_and_expect_snapshot(harness, *synthetic, period_ns, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return stream && stream->frames_received > frames1;
      }, error, "timed out waiting for picture persistence frame")) {
    std::cerr << "FAIL synthetic timeline picture state did not persist frame production\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, period_ns, [&](const CamBANGStateSnapshot& s) {
        const auto* stream = VerifyCaseHarness::find_stream(s, stream_id);
        return stream && stream->mode == CBStreamMode::STOPPED;
      }, error, "timed out waiting for picture scenario stop")) {
    std::cerr << "FAIL synthetic timeline picture scenario stop missing\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        return !VerifyCaseHarness::has_stream(s, stream_id) &&
               VerifyCaseHarness::has_device(s, device_id);
      }, error, "timed out waiting for picture scenario destroy")) {
    std::cerr << "FAIL synthetic timeline picture scenario destroy missing\n";
    return false;
  }

  if (!advance_and_expect_snapshot(harness, *synthetic, 1, [&](const CamBANGStateSnapshot& s) {
        return !VerifyCaseHarness::has_stream(s, stream_id) &&
               !VerifyCaseHarness::has_device(s, device_id);
      }, error, "timed out waiting for picture scenario close")) {
    std::cerr << "FAIL synthetic timeline picture scenario close missing\n";
    return false;
  }

  harness.stop_runtime();
  return true;
}


} // namespace

int main() {
  if (!run_stub_check()) return 1;
  if (!run_synthetic_check()) return 1;
  if (!run_synthetic_timeline_scenario_check()) return 1;
  if (!run_synthetic_timeline_invalid_order_check()) return 1;
  if (!run_synthetic_timeline_host_controls_check()) return 1;
  if (!run_synthetic_timeline_picture_appearance_check()) return 1;
  std::cout << "PASS provider_compliance_verify\n";
  return 0;
}
