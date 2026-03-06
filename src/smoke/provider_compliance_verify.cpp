#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

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
  const int fp_create = find_frameproducer_create(events, stream_id);
  const int started = find_index(events, "stream_started", stream_id);
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
  if (!p.start_stream(req.stream_id, req.profile, req.picture).ok()) return false;
  if (!assert_start_boundary(cb.events, req.stream_id, "stub")) return false;
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
  if (!p.start_stream(req.stream_id, req.profile, req.picture).ok()) return false;
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

#ifdef _WIN32
bool run_windows_check() {
  RecorderCallbacks cb;
  WindowsProvider p;
  if (!p.initialize(&cb).ok()) return false;

  std::vector<CameraEndpoint> eps;
  if (!p.enumerate_endpoints(eps).ok() || eps.empty()) {
    (void)p.shutdown();
    std::cerr << "WARN windows check skipped (no endpoints)\n";
    return true;
  }

  if (!p.open_device(eps[0].hardware_id, 1, 3001).ok()) return false;
  StreamRequest req{};
  req.stream_id = 13;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 640;
  req.profile.height = 480;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;
  if (!p.create_stream(req).ok()) return false;
  if (!p.start_stream(req.stream_id, req.profile, req.picture).ok()) return false;
  if (!assert_start_boundary(cb.events, req.stream_id, "windows")) return false;
  if (!p.shutdown().ok()) return false;

  if (find_index(cb.events, "stream_destroyed", req.stream_id) < 0) {
    std::cerr << "FAIL windows shutdown omitted stream destroy\n";
    return false;
  }
  return assert_native_balance(cb.events, "windows");
}
#endif

} // namespace

int main() {
  if (!run_stub_check()) return 1;
  if (!run_synthetic_check()) return 1;
#ifdef _WIN32
  if (!run_windows_check()) return 1;
#endif
  std::cout << "PASS provider_compliance_verify\n";
  return 0;
}
