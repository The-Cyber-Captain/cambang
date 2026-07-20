// windows_winrt platform runtime validator (platform_runtime_validate=yes).
//
// Real-OS/API visibility check for the windows_winrt provider against local
// camera hardware (docs/dev/build_and_scaffolding.md §11). This SUPPLEMENTS
// the deterministic contract verifiers (provider_compliance_verify et al.);
// it never replaces them.
//
// Exercises, against the first enumerated physical endpoint:
//   enumeration -> open -> stream create/start -> live frame arrival ->
//   still capture while streaming -> bracketed (multi-image) still capture ->
//   geometry-conflict rejection -> stop/destroy/close -> shutdown, and
//   asserts provider fact ordering (started-before-frames, exactly-one-
//   terminal, no frames after stopped) plus, for the bracket, per-member
//   FIFO ordering/routing and distinct truthful realized exposure values.
//   If the opened device has no exposure-compensation control, the bracket
//   admission is expected to be deterministically refused
//   (ERR_NOT_SUPPORTED) rather than treated as a failure.
//
// Exit codes: 0 = PASS, 1 = FAIL, 2 = SKIP (no camera endpoint present).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imaging/api/icamera_provider.h"
#include "imaging/platform/windows/winrt_camera_provider.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.Media.MediaProperties.h>

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

  struct MemberFrameObservation {
    uint32_t member_index = 0;
    bool is_bracket = false;
    int32_t applied_milli_ev = 0;
    bool has_realized = false;
    int32_t realized_milli_ev = 0;
  };
  std::vector<MemberFrameObservation> capture_member_frames;

  // Per-capture observation fields only (never capture_terminals, which is a
  // running total callers snapshot before/after to detect fact emission).
  void begin_new_capture_expectation() {
    std::lock_guard<std::mutex> lock(m);
    capture_started = false;
    capture_completed = false;
    capture_frame_seen = false;
    capture_frame_before_started = false;
    capture_frame_has_payload_owner = false;
    capture_frame_has_timing = false;
    capture_frame_member_index = 0xFFFFFFFFu;
    capture_failed_error = ProviderError::OK;
    capture_member_frames.clear();
  }

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
        MemberFrameObservation obs;
        obs.member_index = frame.capture_image.image_member_index;
        obs.is_bracket = frame.capture_image.routing == CaptureImageRouting::ADDITIONAL_BRACKET;
        obs.applied_milli_ev = frame.capture_image.applied_exposure_compensation_milli_ev;
        obs.has_realized = frame.capture_image.has_realized_exposure_compensation_milli_ev;
        obs.realized_milli_ev = frame.capture_image.realized_exposure_compensation_milli_ev;
        capture_member_frames.push_back(obs);
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

void note(const std::string& label, const std::string& detail) {
  std::printf("[winrt_runtime_validate] INFO %s %s\n", label.c_str(), detail.c_str());
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

  // Real WinRT-native bracketed capture: default-metered + two additional
  // exposure-compensation brackets, matching this repo's canonical 3-member
  // bundle shape (tests/cambang_gde/scenes/01_basic_ux.tscn's
  // capture_profile_bun). If this device's VideoDeviceController has no
  // exposure-compensation control, admission is expected to refuse
  // deterministically (ERR_NOT_SUPPORTED), not to degrade silently.
  {
    check(provider.supports_multi_image_still_sequence(),
          "provider_advertises_multi_image_support");

    harness.begin_new_capture_expectation();
    CaptureRequest bracket_req = creq;
    bracket_req.capture_id = kCaptureId + 20;
    bracket_req.still_image_bundle.members.clear();
    bracket_req.still_image_bundle.members.push_back(
        CaptureStillImageMember{0u, CaptureStillImageMemberRole::DEFAULT_METERED, 0});
    bracket_req.still_image_bundle.members.push_back(CaptureStillImageMember{
        1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, -1000});
    bracket_req.still_image_bundle.members.push_back(
        CaptureStillImageMember{2u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, 1000});

    pr = provider.trigger_capture(bracket_req);
    if (pr.code == ProviderError::ERR_NOT_SUPPORTED) {
      note("bracket_capture",
           "device has no exposure-compensation control; deterministic "
           "admission refusal verified instead of a live bracket");
    } else {
      check(pr.ok(), "bracket_trigger_capture_admission");
      if (pr.ok()) {
        check(harness.wait_for([&] { return harness.capture_terminals > 0; }, 30000),
              "bracket_capture_reaches_terminal");
        std::lock_guard<std::mutex> lock(harness.m);
        check(harness.capture_completed, "bracket_capture_completed");
        check(harness.capture_terminals == 1, "bracket_capture_exactly_one_terminal");
        check(harness.capture_member_frames.size() == 3,
              "bracket_all_three_members_delivered");
        if (harness.capture_member_frames.size() == 3) {
          const auto& m0 = harness.capture_member_frames[0];
          const auto& m1 = harness.capture_member_frames[1];
          const auto& m2 = harness.capture_member_frames[2];
          check(m0.member_index == 0 && m1.member_index == 1 && m2.member_index == 2,
                "bracket_member_index_fifo_order");
          check(!m0.is_bracket && m1.is_bracket && m2.is_bracket,
                "bracket_routing_correct");
          check(m0.has_realized && m1.has_realized && m2.has_realized,
                "bracket_realized_ev_reported_for_all_members");
          check(m0.realized_milli_ev != m1.realized_milli_ev ||
                    m1.realized_milli_ev != m2.realized_milli_ev,
                "bracket_realized_ev_values_distinct");
          note("bracket_realized_milli_ev_member0", std::to_string(m0.realized_milli_ev));
          note("bracket_realized_milli_ev_member1", std::to_string(m1.realized_milli_ev));
          note("bracket_realized_milli_ev_member2", std::to_string(m2.realized_milli_ev));
        }
      }
    }
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

  // Diagnostic-only, never affects pass/fail: log which VideoDeviceController
  // capabilities each enumerated camera reports, for anyone investigating why
  // the bracket check above did or didn't take the live-capture path. Probed
  // twice per device -- immediately after open, and again after a color
  // frame source/reader is realized and actually started -- because some UVC
  // drivers only populate extended control surfaces once a stream pipeline
  // is running, not merely on device open. The real provider's admission-
  // time check (query_exposure_compensation_range_) only ever probes at the
  // "before" point (by design, so admission stays prompt/bounded); if "after"
  // ever differs from "before" here, that is a genuine finding that the
  // admission check is probing too early. Runs on its own short-lived,
  // apartment-initialized thread (the provider's own device is fully
  // released by now, so this cannot conflict with it), after every prior
  // provider-under-test device has been closed.
  if (!endpoints.empty()) {
    std::thread diag_thread([&] {
      try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
      } catch (...) {
        return;
      }
      for (size_t ep_idx = 0; ep_idx < endpoints.size(); ++ep_idx) {
        const std::string ep_tag = "endpoint" + std::to_string(ep_idx);
        try {
          // USB instance paths are ASCII; no UTF-8 decoding needed here.
          const std::string& hw_id = endpoints[ep_idx].hardware_id;
          const std::wstring wide_id(hw_id.begin(), hw_id.end());
          winrt::Windows::Media::Capture::MediaCapture diag_capture;
          winrt::Windows::Media::Capture::MediaCaptureInitializationSettings diag_settings;
          diag_settings.VideoDeviceId(winrt::hstring(wide_id));
          diag_settings.StreamingCaptureMode(
              winrt::Windows::Media::Capture::StreamingCaptureMode::Video);
          diag_settings.SharingMode(
              winrt::Windows::Media::Capture::MediaCaptureSharingMode::ExclusiveControl);
          diag_settings.MemoryPreference(
              winrt::Windows::Media::Capture::MediaCaptureMemoryPreference::Cpu);
          diag_capture.InitializeAsync(diag_settings).get();
          auto vdc = diag_capture.VideoDeviceController();

          const auto describe_controls = [&]() {
            return std::string("exposure_compensation=") +
                   (vdc.ExposureCompensationControl().Supported() ? "true" : "false") +
                   " exposure=" + (vdc.ExposureControl().Supported() ? "true" : "false") +
                   " iso=" + (vdc.IsoSpeedControl().Supported() ? "true" : "false") +
                   " focus=" + (vdc.FocusControl().Supported() ? "true" : "false") +
                   " white_balance=" + (vdc.WhiteBalanceControl().Supported() ? "true" : "false");
          };
          note(ep_tag + "_name", endpoints[ep_idx].name);
          note(ep_tag + "_controls_before_pipeline", describe_controls());

          winrt::Windows::Media::Capture::Frames::MediaFrameSource color_source{nullptr};
          for (const auto& kv : diag_capture.FrameSources()) {
            const auto candidate = kv.Value();
            if (candidate.Info().SourceKind() ==
                winrt::Windows::Media::Capture::Frames::MediaFrameSourceKind::Color) {
              color_source = candidate;
              break;
            }
          }
          if (!color_source) {
            note(ep_tag + "_no_color_frame_source", "true");
          } else {
            auto reader =
                diag_capture
                    .CreateFrameReaderAsync(
                        color_source,
                        winrt::Windows::Media::MediaProperties::MediaEncodingSubtypes::Bgra8())
                    .get();
            reader.AcquisitionMode(
                winrt::Windows::Media::Capture::Frames::MediaFrameReaderAcquisitionMode::Realtime);
            const auto start_status = reader.StartAsync().get();
            const bool started = start_status ==
                winrt::Windows::Media::Capture::Frames::MediaFrameReaderStartStatus::Success;
            note(ep_tag + "_pipeline_started", started ? "true" : "false");
            if (started) {
              // Let the pipeline actually begin streaming before re-querying.
              std::this_thread::sleep_for(std::chrono::milliseconds(500));
              note(ep_tag + "_controls_after_pipeline_started", describe_controls());
              (void)reader.StopAsync().get();
            }
            reader.Close();
          }
          diag_capture.Close();
        } catch (const winrt::hresult_error& e) {
          note(ep_tag + "_probe_failed", "hr=0x" + std::to_string(static_cast<uint32_t>(e.code())));
        } catch (...) {
          note(ep_tag + "_probe_failed", "unknown_exception");
        }
      }
      try {
        winrt::uninit_apartment();
      } catch (...) {
      }
    });
    diag_thread.join();
  }

  std::printf("[winrt_runtime_validate] %s failures=%d\n",
              g_failures == 0 ? "RESULT PASS" : "RESULT FAIL", g_failures);
  return g_failures == 0 ? 0 : 1;
}
