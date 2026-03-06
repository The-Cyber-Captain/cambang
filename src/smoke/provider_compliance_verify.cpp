#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "Build with -DCAMBANG_INTERNAL_SMOKE=1"
#endif

#include "imaging/api/provider_strand.h"
#include "imaging/stub/provider.h"

using namespace cambang;

namespace {

struct RecorderCallbacks final : IProviderCallbacks {
  uint64_t next_native_id = 1;
  std::vector<std::string> events;

  uint64_t allocate_native_id(NativeObjectType) override { return next_native_id++; }
  uint64_t core_monotonic_now_ns() override { return 0; }

  void on_device_opened(uint64_t id) override { events.push_back("device_opened:" + std::to_string(id)); }
  void on_device_closed(uint64_t id) override { events.push_back("device_closed:" + std::to_string(id)); }
  void on_stream_created(uint64_t id) override { events.push_back("stream_created:" + std::to_string(id)); }
  void on_stream_destroyed(uint64_t id) override { events.push_back("stream_destroyed:" + std::to_string(id)); }
  void on_stream_started(uint64_t id) override { events.push_back("stream_started:" + std::to_string(id)); }
  void on_stream_stopped(uint64_t id, ProviderError) override { events.push_back("stream_stopped:" + std::to_string(id)); }
  void on_capture_started(uint64_t id) override { events.push_back("capture_started:" + std::to_string(id)); }
  void on_capture_completed(uint64_t id) override { events.push_back("capture_completed:" + std::to_string(id)); }
  void on_capture_failed(uint64_t id, ProviderError) override { events.push_back("capture_failed:" + std::to_string(id)); }
  void on_frame(const FrameView&) override { events.push_back("frame"); }
  void on_device_error(uint64_t id, ProviderError) override { events.push_back("device_error:" + std::to_string(id)); }
  void on_stream_error(uint64_t id, ProviderError) override { events.push_back("stream_error:" + std::to_string(id)); }
  void on_native_object_created(const NativeObjectCreateInfo& info) override {
    events.push_back("native_created:" + std::to_string(info.native_id));
  }
  void on_native_object_destroyed(const NativeObjectDestroyInfo& info) override {
    events.push_back("native_destroyed:" + std::to_string(info.native_id));
  }
};

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

  const auto find = [&](const std::string& tag) {
    return std::find(cb.events.begin(), cb.events.end(), tag) != cb.events.end();
  };
  if (!find("stream_created:11") || !find("stream_started:11") || !find("stream_error:11") ||
      !find("stream_stopped:11") || !find("stream_destroyed:11")) {
    std::cerr << "FAIL strand non-frame event drop under pressure\n";
    return false;
  }

  auto idx = [&](const std::string& tag) {
    return static_cast<int>(std::find(cb.events.begin(), cb.events.end(), tag) - cb.events.begin());
  };
  if (!(idx("stream_created:11") < idx("stream_started:11") && idx("stream_started:11") < idx("stream_error:11") &&
        idx("stream_error:11") < idx("stream_stopped:11") && idx("stream_stopped:11") < idx("stream_destroyed:11"))) {
    std::cerr << "FAIL strand non-frame order\n";
    return false;
  }
  return true;
}

bool run_stub_shutdown_truth_check() {
  RecorderCallbacks cb;
  StubProvider provider;

  if (!provider.initialize(&cb).ok()) return false;
  if (!provider.open_device("stub0", 1, 101).ok()) return false;

  StreamRequest req{};
  req.stream_id = 7;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 64;
  req.profile.height = 64;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;
  if (!provider.create_stream(req).ok()) return false;
  if (!provider.start_stream(req.stream_id, req.profile, req.picture).ok()) return false;

  if (!provider.shutdown().ok()) return false;

  const auto idx = [&](const std::string& tag) {
    return static_cast<int>(std::find(cb.events.begin(), cb.events.end(), tag) - cb.events.begin());
  };

  // Lifecycle ordering guarantees around teardown.
  if (!(idx("stream_stopped:7") < idx("stream_destroyed:7") && idx("stream_destroyed:7") < idx("device_closed:1"))) {
    std::cerr << "FAIL teardown lifecycle ordering\n";
    return false;
  }

  // Native object destroy must be observed for provider/device/stream/frame producer.
  int created = 0;
  int destroyed = 0;
  for (const auto& e : cb.events) {
    if (e.rfind("native_created:", 0) == 0) ++created;
    if (e.rfind("native_destroyed:", 0) == 0) ++destroyed;
  }
  if (created != destroyed) {
    std::cerr << "FAIL native object create/destroy mismatch\n";
    return false;
  }

  return true;
}

} // namespace

int main() {
  if (!run_strand_policy_check()) return 1;
  if (!run_stub_shutdown_truth_check()) return 1;
  std::cout << "PASS provider_compliance_verify\n";
  return 0;
}
