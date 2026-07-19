// windows_winrt platform runtime validator (platform_runtime_validate=yes).
//
// Real-OS/API visibility check for the windows_winrt provider against local
// camera hardware (docs/dev/build_and_scaffolding.md §11). This SUPPLEMENTS
// the deterministic contract verifiers (provider_compliance_verify et al.);
// it never replaces them.
//
// Exercises, against the first enumerated physical endpoint:
//   enumeration -> open -> stream create/start -> live frame arrival ->
//   still capture while streaming -> geometry-conflict rejection ->
//   stop/destroy/close -> shutdown, and asserts provider fact ordering
//   (started-before-frames, exactly-one-terminal, no frames after stopped).
//
// Exit codes: 0 = PASS, 1 = FAIL, 2 = SKIP (no camera endpoint present).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "imaging/api/icamera_provider.h"
#include "imaging/platform/windows/winrt_camera_provider.h"

namespace {

using namespace cambang;

constexpr uint64_t kDeviceInstanceId = 1;
constexpr uint64_t kRootId = 100;
constexpr uint64_t kStreamId = 10;
constexpr uint64_t kCaptureId = 500;
constexpr uint32_t kWidth = 640;
constexpr uint32_t kHeight = 480;

struct HarnessCallbacks final : IProviderCallbacks {
  std::atomic<uint64_t> next_native_id{1};

  std::mutex m;
  std::condition_variable cv;

  uint64_t stream_frames = 0;
  uint64_t stream_frames_after_stopped = 0;
  bool stream_started = false;
  bool stream_stopped = false;
  ProviderError stream_stop_error = ProviderError::OK;

  bool capture_started = false;
  bool capture_frame_seen = false;
  bool capture_frame_before_started = false;
  bool capture_frame_has_payload_owner = false;
  bool capture_frame_has_timing = false;
  uint32_t capture_frame_member_index = 0xFFFFFFFFu;
  int capture_terminals = 0;
  bool capture_completed = false;
  ProviderError capture_failed_error = ProviderError::OK;

  uint64_t native_created = 0;
  uint64_t native_destroyed = 0;
  uint64_t device_errors = 0;
  uint64_t stream_errors = 0;

  uint64_t allocate_native_id(NativeObjectType) override {
    return next_native_id.fetch_add(1, std::memory_order_relaxed);
  }
  uint64_t core_monotonic_now_ns() override {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
  bool is_stream_display_demand_active(uint64_t) override { return false; }

  void on_device_opened(uint64_t) override {}
  void on_device_closed(uint64_t) override {}
  void on_stream_created(uint64_t) override {}
  void on_stream_destroyed(uint64_t) override {}
  void on_stream_started(uint64_t) override {
    std::lock_guard<std::mutex> lock(m);
    stream_started = true;
    cv.notify_all();
  }
  void on_stream_stopped(uint64_t, ProviderError error_or_ok) override {
    std::lock_guard<std::mutex> lock(m);
    stream_stopped = true;
    stream_stop_error = error_or_ok;
    cv.notify_all();
  }

  void on_capture_started(uint64_t, uint64_t) override {
    std::lock_guard<std::mutex> lock(m);
    capture_started = true;
    cv.notify_all();
  }
  void on_capture_completed(uint64_t, uint64_t) override {
    std::lock_guard<std::mutex> lock(m);
    ++capture_terminals;
    capture_completed = true;
    cv.notify_all();
  }
  void on_capture_failed(uint64_t, uint64_t, ProviderError error) override {
    std::lock_guard<std::mutex> lock(m);
    ++capture_terminals;
    capture_failed_error = error;
    cv.notify_all();
  }

  void on_frame(const FrameView& frame) override {
    {
      std::lock_guard<std::mutex> lock(m);
      if (frame.capture_id != 0) {
        if (!capture_started) {
          capture_frame_before_started = true;
        }
        capture_frame_seen = true;
        capture_frame_member_index = frame.capture_image.image_member_index;
        capture_frame_has_payload_owner = static_cast<bool>(frame.cpu_payload_owner);
        capture_frame_has_timing = frame.acquisition_timing.has_value();
      } else {
        if (stream_stopped) {
          ++stream_frames_after_stopped;
        }
        ++stream_frames;
      }
      cv.notify_all();
    }
    frame.release_now();
  }

  void on_device_error(uint64_t, ProviderError) override {
    std::lock_guard<std::mutex> lock(m);
    ++device_errors;
  }
  void on_stream_error(uint64_t, ProviderError) override {
    std::lock_guard<std::mutex> lock(m);
    ++stream_errors;
  }

  void on_native_object_created(const NativeObjectCreateInfo&) override {
    std::lock_guard<std::mutex> lock(m);
    ++native_created;
  }
  void on_native_object_destroyed(const NativeObjectDestroyInfo&) override {
    std::lock_guard<std::mutex> lock(m);
    ++native_destroyed;
  }

  template <typename Pred>
  bool wait_for(Pred pred, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(m);
    return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred);
  }
};

int g_failures = 0;

void check(bool ok, const char* label) {
  std::printf("[winrt_runtime_validate] %s %s\n", ok ? "PASS" : "FAIL", label);
  if (!ok) {
    ++g_failures;
  }
}

void note(const char* label, const std::string& detail) {
  std::printf("[winrt_runtime_validate] INFO %s %s\n", label, detail.c_str());
}

} // namespace

int main() {
  const ProviderAccessStatus access = WinrtCameraProvider::check_access_readiness();
  note("access_readiness", access.stable_reason);
  if (!access.ok()) {
    check(false, "access_readiness_ready");
    return 1;
  }

  WinrtCameraProvider provider;
  HarnessCallbacks harness;

  ProviderResult pr = provider.initialize(&harness);
  check(pr.ok(), "initialize");
  if (!pr.ok()) {
    return 1;
  }

  std::vector<CameraEndpoint> endpoints;
  pr = provider.enumerate_endpoints(endpoints);
  check(pr.ok(), "enumerate_endpoints");
  for (const auto& ep : endpoints) {
    note("endpoint", ep.name + " [" + ep.hardware_id + "]");
  }
  if (endpoints.empty()) {
    std::printf("[winrt_runtime_validate] SKIP no_camera_endpoint_present\n");
    (void)provider.shutdown();
    return 2;
  }

  pr = provider.open_device(endpoints[0].hardware_id, kDeviceInstanceId, kRootId);
  check(pr.ok(), "open_device");
  if (!pr.ok()) {
    (void)provider.shutdown();
    return 1;
  }

  StreamRequest sreq{};
  sreq.stream_id = kStreamId;
  sreq.device_instance_id = kDeviceInstanceId;
  sreq.intent = StreamIntent::PREVIEW;
  sreq.profile.width = kWidth;
  sreq.profile.height = kHeight;
  sreq.profile.format_fourcc = FOURCC_RGBA;
  sreq.profile.target_fps_max = 30;
  check(provider.create_stream(sreq).ok(), "create_stream");
  check(provider.start_stream(kStreamId, sreq.profile, sreq.picture).ok(),
        "start_stream");

  check(harness.wait_for([&] { return harness.stream_frames >= 10; }, 10000),
        "stream_delivers_frames");
  {
    std::lock_guard<std::mutex> lock(harness.m);
    note("stream_frames", std::to_string(harness.stream_frames));
  }

  // Still capture while the stream is live, at the stream geometry.
  CaptureRequest creq{};
  creq.capture_id = kCaptureId;
  creq.device_instance_id = kDeviceInstanceId;
  creq.width = kWidth;
  creq.height = kHeight;
  creq.format_fourcc = FOURCC_RGBA;
  creq.still_image_bundle = make_default_metered_still_image_bundle();
  pr = provider.trigger_capture(creq);
  check(pr.ok(), "trigger_capture_admission");
  if (pr.ok()) {
    check(harness.wait_for([&] { return harness.capture_terminals > 0; }, 15000),
          "capture_reaches_terminal");
    std::lock_guard<std::mutex> lock(harness.m);
    check(harness.capture_completed, "capture_completed");
    check(harness.capture_frame_seen, "capture_frame_delivered");
    check(!harness.capture_frame_before_started, "capture_started_before_frame");
    check(harness.capture_terminals == 1, "capture_exactly_one_terminal");
    check(harness.capture_frame_member_index == 0, "capture_member_index_zero");
    check(harness.capture_frame_has_payload_owner, "capture_zero_copy_payload_owner");
    check(harness.capture_frame_has_timing, "capture_acquisition_timing_present");
  }

  // Deterministic geometry-conflict rejection: a capture needing different
  // geometry while a started stream pins the shared reader must be refused at
  // admission with ERR_PLATFORM_CONSTRAINT and emit no facts.
  {
    CaptureRequest conflict = creq;
    conflict.capture_id = kCaptureId + 1;
    conflict.width = 320;
    conflict.height = 240;
    const int terminals_before = harness.capture_terminals;
    pr = provider.trigger_capture(conflict);
    check(pr.code == ProviderError::ERR_PLATFORM_CONSTRAINT,
          "geometry_conflict_rejected_at_admission");
    std::lock_guard<std::mutex> lock(harness.m);
    check(harness.capture_terminals == terminals_before,
          "rejected_capture_emits_no_facts");
  }

  check(provider.stop_stream(kStreamId).ok(), "stop_stream");
  // The stopped fact is delivered through the strand asynchronously; only its
  // order relative to frames is synchronous (no_frames_after_stopped below).
  check(harness.wait_for([&] { return harness.stream_stopped; }, 5000),
        "stream_stopped_fact");
  {
    std::lock_guard<std::mutex> lock(harness.m);
    check(harness.stream_stop_error == ProviderError::OK, "stream_stopped_ok");
  }
  check(provider.destroy_stream(kStreamId).ok(), "destroy_stream");
  check(provider.close_device(kDeviceInstanceId).ok(), "close_device");
  check(provider.shutdown().ok(), "shutdown");

  {
    std::lock_guard<std::mutex> lock(harness.m);
    check(harness.stream_frames_after_stopped == 0, "no_frames_after_stopped");
    check(harness.device_errors == 0, "no_device_errors");
    check(harness.stream_errors == 0, "no_stream_errors");
    check(harness.native_created == harness.native_destroyed,
          "native_objects_balanced");
    note("native_created", std::to_string(harness.native_created));
  }

  std::printf("[winrt_runtime_validate] %s failures=%d\n",
              g_failures == 0 ? "RESULT PASS" : "RESULT FAIL", g_failures);
  return g_failures == 0 ? 0 : 1;
}
