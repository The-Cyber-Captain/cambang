#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "Build with -DCAMBANG_INTERNAL_SMOKE=1"
#endif

#include "imaging/api/provider_strand.h"
#include "imaging/stub/provider.h"
#include "imaging/synthetic/provider.h"

#ifdef _WIN32
#include "imaging/platform/windows/provider.h"
#endif

using namespace cambang;

namespace {

struct EventRec {
  std::string tag;
  uint64_t id = 0;
  uint32_t type = 0;
  uint64_t owner_stream_id = 0;
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
  void on_frame(const FrameView&) override { events.push_back({"frame", 0}); }
  void on_device_error(uint64_t id, ProviderError) override { events.push_back({"device_error", id}); }
  void on_stream_error(uint64_t id, ProviderError) override { events.push_back({"stream_error", id}); }
  void on_native_object_created(const NativeObjectCreateInfo& info) override {
    events.push_back({"native_created", info.native_id, info.type, info.owner_stream_id});
  }
  void on_native_object_destroyed(const NativeObjectDestroyInfo& info) override {
    events.push_back({"native_destroyed", info.native_id});
  }
};

int index_of_tag_id(const std::vector<EventRec>& events, const char* tag, uint64_t id) {
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].tag == tag && events[i].id == id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int index_of_frameproducer_create_for_stream(const std::vector<EventRec>& events, uint64_t stream_id) {
  const uint32_t fp_type = static_cast<uint32_t>(NativeObjectType::FrameProducer);
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].tag == "native_created" && events[i].type == fp_type && events[i].owner_stream_id == stream_id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool check_start_boundary_order(const std::vector<EventRec>& events, uint64_t stream_id, const char* provider_name) {
  const int fp_create = index_of_frameproducer_create_for_stream(events, stream_id);
  const int started = index_of_tag_id(events, "stream_started", stream_id);
  if (fp_create < 0 || started < 0 || fp_create > started) {
    std::cerr << "FAIL " << provider_name << " start boundary ordering\n";
    return false;
  }
  return true;
}

bool run_strand_policy_check() {
  RecorderCallbacks cb;
  CBProviderStrand strand;
  strand.start(&cb, "strand_compliance", 4);

  FrameView f{};
  for (int i = 0; i < 32; ++i) {
    strand.post_frame(f);
  }
  strand.post_stream_created(11);
  strand.post_stream_started(11);
  strand.post_stream_error(11, ProviderError::ERR_PLATFORM_CONSTRAINT);
  strand.post_stream_stopped(11, ProviderError::OK);
  strand.post_stream_destroyed(11);

  strand.flush();
  strand.stop();

  const int c = index_of_tag_id(cb.events, "stream_created", 11);
  const int s = index_of_tag_id(cb.events, "stream_started", 11);
  const int e = index_of_tag_id(cb.events, "stream_error", 11);
  const int t = index_of_tag_id(cb.events, "stream_stopped", 11);
  const int d = index_of_tag_id(cb.events, "stream_destroyed", 11);
  if (!(c >= 0 && s >= 0 && e >= 0 && t >= 0 && d >= 0 && c < s && s < e && e < t && t < d)) {
    std::cerr << "FAIL strand non-frame delivery/order under pressure\n";
    return false;
  }
  return true;
}

bool run_stub_check() {
  RecorderCallbacks cb;
  StubProvider provider;

  StreamRequest req{};
  req.stream_id = 7;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 64;
  req.profile.height = 64;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  if (!provider.initialize(&cb).ok()) return false;
  if (!provider.open_device("stub0", 1, 101).ok()) return false;
  if (!provider.create_stream(req).ok()) return false;
  if (!provider.start_stream(req.stream_id, req.profile, req.picture).ok()) return false;
  if (!check_start_boundary_order(cb.events, req.stream_id, "stub")) return false;
  if (!provider.shutdown().ok()) return false;

  const int stop_idx = index_of_tag_id(cb.events, "stream_stopped", req.stream_id);
  const int destroy_idx = index_of_tag_id(cb.events, "stream_destroyed", req.stream_id);
  const int dev_close_idx = index_of_tag_id(cb.events, "device_closed", req.device_instance_id);
  if (!(stop_idx >= 0 && destroy_idx >= 0 && dev_close_idx >= 0 && stop_idx < destroy_idx && destroy_idx < dev_close_idx)) {
    std::cerr << "FAIL stub teardown lifecycle ordering\n";
    return false;
  }

  int created = 0;
  int destroyed = 0;
  for (const auto& e : cb.events) {
    if (e.tag == "native_created") ++created;
    if (e.tag == "native_destroyed") ++destroyed;
  }
  if (created != destroyed) {
    std::cerr << "FAIL stub native create/destroy mismatch\n";
    return false;
  }
  return true;
}

bool run_synthetic_check() {
  RecorderCallbacks cb;
  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);

  StreamRequest req{};
  req.stream_id = 9;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 64;
  req.profile.height = 64;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  if (!provider.initialize(&cb).ok()) return false;
  if (!provider.open_device("synthetic:0", 1, 201).ok()) return false;
  if (!provider.create_stream(req).ok()) return false;
  if (!provider.start_stream(req.stream_id, req.profile, req.picture).ok()) return false;
  if (!check_start_boundary_order(cb.events, req.stream_id, "synthetic")) return false;
  if (!provider.shutdown().ok()) return false;

  int created = 0;
  int destroyed = 0;
  for (const auto& e : cb.events) {
    if (e.tag == "native_created") ++created;
    if (e.tag == "native_destroyed") ++destroyed;
  }
  if (created != destroyed) {
    std::cerr << "FAIL synthetic native create/destroy mismatch\n";
    return false;
  }
  return true;
}

#ifdef _WIN32
bool run_windows_check() {
  RecorderCallbacks cb;
  WindowsProvider provider;
  if (!provider.initialize(&cb).ok()) {
    std::cerr << "FAIL windows initialize\n";
    return false;
  }

  std::vector<CameraEndpoint> eps;
  if (!provider.enumerate_endpoints(eps).ok() || eps.empty()) {
    // Hardware-dependent path; if unavailable we can't assert stream lifecycle.
    provider.shutdown();
    std::cerr << "WARN windows check skipped (no endpoints)\n";
    return true;
  }

  if (!provider.open_device(eps[0].hardware_id, 1, 301).ok()) return false;

  StreamRequest req{};
  req.stream_id = 13;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 640;
  req.profile.height = 480;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  if (!provider.create_stream(req).ok()) return false;
  if (!provider.start_stream(req.stream_id, req.profile, req.picture).ok()) return false;
  if (!check_start_boundary_order(cb.events, req.stream_id, "windows")) return false;

  if (!provider.shutdown().ok()) {
    std::cerr << "FAIL windows shutdown\n";
    return false;
  }

  if (index_of_tag_id(cb.events, "stream_destroyed", req.stream_id) < 0) {
    std::cerr << "FAIL windows shutdown omitted stream destroy\n";
    return false;
  }

  int created = 0;
  int destroyed = 0;
  for (const auto& e : cb.events) {
    if (e.tag == "native_created") ++created;
    if (e.tag == "native_destroyed") ++destroyed;
  }
  if (created != destroyed) {
    std::cerr << "FAIL windows native create/destroy mismatch\n";
    return false;
  }
  return true;
}
#endif

} // namespace

int main() {
  if (!run_strand_policy_check()) return 1;
  if (!run_stub_check()) return 1;
  if (!run_synthetic_check()) return 1;
#ifdef _WIN32
  if (!run_windows_check()) return 1;
#endif
  std::cout << "PASS provider_compliance_verify\n";
  return 0;
}
