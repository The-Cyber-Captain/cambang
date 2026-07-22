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
#include <cstring>
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
// Kernel Streaming property identifiers for the GetDeviceProperty* probe
// below. Header-only constant use: no KS/MF runtime is linked or called, and
// ks.h must precede ksmedia.h.
#include <ks.h>
#include <ksmedia.h>
#include <winrt/Windows.Media.Devices.Core.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Perception.Spatial.h>

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

  bool static_facts_seen = false;
  ProviderCameraFacts static_facts{};
  int capture_image_facts_calls = 0;
  bool capture_image_facts_has_intrinsics = false;
  bool capture_image_facts_has_distortion = false;
  bool capture_image_facts_has_focus_state = false;

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

  void on_camera_static_facts(uint64_t, ProviderCameraFacts facts) override {
    std::lock_guard<std::mutex> lock(m);
    static_facts_seen = true;
    static_facts = std::move(facts);
  }
  void on_capture_image_facts(uint64_t, uint64_t, uint32_t,
                              ProviderCaptureImageFacts facts) override {
    std::lock_guard<std::mutex> lock(m);
    ++capture_image_facts_calls;
    if (facts.intrinsics) capture_image_facts_has_intrinsics = true;
    if (facts.distortion) capture_image_facts_has_distortion = true;
    if (facts.focus_state) capture_image_facts_has_focus_state = true;
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

  // Static camera facts (facing, sensor mounting orientation) are best-effort
  // and device-dependent (not every camera reports an EnclosureLocation), so
  // this is informational rather than pass/fail. open_device() already
  // waited out the bounded lookup synchronously; this only waits for the
  // strand to deliver whatever it decided to post.
  (void)harness.wait_for([&] { return harness.static_facts_seen; }, 2000);
  {
    std::lock_guard<std::mutex> lock(harness.m);
    if (harness.static_facts_seen) {
      const auto& sf = harness.static_facts.static_facts;
      note("static_facts_facing",
           sf.facing ? std::to_string(static_cast<int>(sf.facing->value)) : "absent");
      note("static_facts_camera_nature",
           sf.nature ? std::to_string(static_cast<int>(sf.nature->value)) : "absent");
      note("static_facts_sensor_orientation_degrees",
           sf.sensor_orientation
               ? std::to_string(static_cast<int>(sf.sensor_orientation->value))
               : "absent");
    } else {
      note("static_facts", "device reported no EnclosureLocation (or lookup did not complete)");
    }
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
    // Per-capture-image intrinsics/distortion are device-dependent (most UVC
    // webcams report none via VideoMediaFrame.CameraIntrinsics(); some
    // depth/professional cameras do), so this is informational, not
    // pass/fail: it reports whether the attempt was made and what it found,
    // never asserts a specific outcome.
    note("capture_image_facts_calls", std::to_string(harness.capture_image_facts_calls));
    note("capture_image_facts_intrinsics_present",
         harness.capture_image_facts_has_intrinsics ? "true" : "false");
    note("capture_image_facts_distortion_present",
         harness.capture_image_facts_has_distortion ? "true" : "false");
    note("capture_image_facts_focus_state_present",
         harness.capture_image_facts_has_focus_state ? "true" : "false");
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

  // Diagnostic-only, never affects pass/fail: a capability census of what each
  // enumerated camera will tell us about itself, so decisions about which
  // facts a platform provider can supply rest on measurement.
  //
  // Probed in three pipeline states, because some drivers cannot report which
  // controls they support until the camera is running, and the documented
  // requirement names *preview* specifically -- which is not the same
  // mechanism as the frame reader this provider drives:
  //   cold                 -- device initialized, nothing running
  //   frame_reader_running -- a color MediaFrameReader realized and started
  //   preview_running      -- StartPreviewAsync, a distinct mechanism
  //
  // Each state reports: which VideoDeviceController controls claim support;
  // what VariablePhotoSequenceController advertises (including the
  // documented-millimetre frame focus Min/Max/Step); the raw FocusControl
  // Min/Max/Step/Value read; static MediaCaptureSettings characteristics; and
  // Kernel Streaming VideoProcAmp/CameraControl property reachability.
  //
  // READ THIS BEFORE TRUSTING ANY KS VALUE. The KS reads below report the
  // control's *set-point*, not realized sensor state. Measured on this
  // hardware: exposure sat at -6 (2^-6 s = 1/64 s, a thoroughly plausible
  // number) while frame brightness swung sixty-fold, and focus sat at 500
  // while the maintainer confirmed the autofocus motor hunting. A plausible
  // constant is the dangerous case -- it passes every sanity check that does
  // not test whether the value tracks reality. KS *capability* answers are
  // trustworthy (it correctly reports no focus control on a fixed-focus lens
  // and focus control on an autofocusing one); KS values are not.
  //
  // The real provider's admission-time check
  // (query_exposure_compensation_range_) only ever probes cold, by design, so
  // admission stays prompt/bounded. If a later state differs from cold here,
  // that admission check is probing too early and its refusals cannot be
  // trusted.
  //
  // Runs on its own short-lived, apartment-initialized thread (the provider's
  // own device is fully released by now, so this cannot conflict with it),
  // after every prior provider-under-test device has been closed.
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
          // SharedReadOnly, deliberately, not ExclusiveControl. Taking
          // exclusive control hands this session ownership of the device's
          // controls, which was observed to freeze the camera's own autofocus
          // for as long as the probe held the device -- autofocus resumed the
          // moment it exited. That confounds every "the value never changed"
          // reading taken under exclusive control: a pinned value may mean we
          // pinned it, not that the driver fails to report realized state.
          // Shared mode cannot set controls, but this probe only reads.
          diag_settings.SharingMode(
              winrt::Windows::Media::Capture::MediaCaptureSharingMode::SharedReadOnly);
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

          // Variable photo sequence is the candidate primary still-capture
          // path: it is the advertised native EV-bracket route AND the only
          // one whose delivered frames carry CapturedFrameControlValues, so
          // realized exposure/ISO/focus arrive frame-synchronized rather than
          // polled out-of-band. FrameFocusCapabilities is also the one place
          // Microsoft documents a unit for focus ("specified in millimeters").
          const auto describe_vps = [&]() -> std::string {
            try {
              const auto vps = vdc.VariablePhotoSequenceController();
              if (!vps) return "controller_null";
              if (!vps.Supported()) return "supported=false";
              std::string out = "supported=true";
              const auto caps = vps.FrameCapabilities();
              if (!caps) return out + " frame_capabilities=null";
              out += std::string(" exposure=") +
                     (caps.Exposure().Supported() ? "true" : "false");
              out += std::string(" exposure_compensation=") +
                     (caps.ExposureCompensation().Supported() ? "true" : "false");
              out += std::string(" iso=") +
                     (caps.IsoSpeed().Supported() ? "true" : "false");
              const auto focus = caps.Focus();
              const bool focus_supported = focus && focus.Supported();
              out += std::string(" focus=") + (focus_supported ? "true" : "false");
              if (focus_supported) {
                // The documented-millimetre four-number read. Magnitude
                // discriminates the competing readings: ~50-10000 is a focus
                // (subject) distance in mm; ~2-6 would be a focal length; a
                // 0-255 or 0-100 span is a raw driver scale wearing the
                // documented unit's clothes.
                out += " focus_mm_min=" + std::to_string(focus.Min()) +
                       " focus_mm_max=" + std::to_string(focus.Max()) +
                       " focus_mm_step=" + std::to_string(focus.Step());
              }
              return out;
            } catch (const winrt::hresult_error& e) {
              return "probe_failed hr=0x" + std::to_string(static_cast<uint32_t>(e.code()));
            } catch (...) {
              return "probe_failed unknown_exception";
            }
          };

          // The same four numbers from the non-sequence control, whose docs
          // use identical wording ("focus length") but state no unit. Value
          // is the decisive one: if it always falls inside [Min, Max] the
          // value shares that domain; if it falls outside, the reading is an
          // index into a Min + Value * Step scale instead, and any
          // millimetre interpretation of it is wrong.
          const auto describe_focus_numbers = [&]() -> std::string {
            try {
              const auto focus = vdc.FocusControl();
              if (!focus) return "control_null";
              if (!focus.Supported()) return "supported=false";
              std::string out = "supported=true";
              out += " min=" + std::to_string(focus.Min()) +
                     " max=" + std::to_string(focus.Max()) +
                     " step=" + std::to_string(focus.Step());
              try {
                const uint32_t value = focus.Value();
                out += " value=" + std::to_string(value);
                out += std::string(" value_within_min_max=") +
                       ((value >= focus.Min() && value <= focus.Max()) ? "true" : "false");
              } catch (...) {
                out += " value=unreadable";
              }
              return out;
            } catch (const winrt::hresult_error& e) {
              return "probe_failed hr=0x" + std::to_string(static_cast<uint32_t>(e.code()));
            } catch (...) {
              return "probe_failed unknown_exception";
            }
          };

          // Static device characteristics on MediaCaptureSettings, which need
          // no control support at all -- so these may be populated on hardware
          // where every control reports unsupported. Each is IReference<double>
          // (null when the device does not report it).
          //
          // Note carefully: a 35mm-equivalent focal length is NOT the physical
          // focal length our focal_length_mm fact carries -- it is that value
          // scaled by the sensor crop factor, and EXIF rightly treats the two
          // as separate tags. Measuring it here decides whether it is worth
          // having at all; it must not be mapped onto focal_length_mm.
          // PitchOffsetDegrees is likewise pose-adjacent, not a CameraPose.
          const auto describe_capture_settings = [&]() -> std::string {
            try {
              const auto settings = diag_capture.MediaCaptureSettings();
              if (!settings) return "settings_null";
              const auto opt = [](const auto& ref) -> std::string {
                return ref ? std::to_string(ref.Value()) : std::string("absent");
              };
              return "h35mm_equiv_focal_length=" +
                     opt(settings.Horizontal35mmEquivalentFocalLength()) +
                     " v35mm_equiv_focal_length=" +
                     opt(settings.Vertical35mmEquivalentFocalLength()) +
                     " pitch_offset_degrees=" + opt(settings.PitchOffsetDegrees());
            } catch (const winrt::hresult_error& e) {
              return "probe_failed hr=0x" + std::to_string(static_cast<uint32_t>(e.code()));
            } catch (...) {
              return "probe_failed unknown_exception";
            }
          };

          // Kernel Streaming property probe, reached through the pure-WinRT
          // GetDeviceProperty* escape hatch on VideoDeviceController -- no MF
          // runtime and no DirectShow COM interop, only KS identifiers from
          // ksmedia.h passed through a WinRT call.
          //
          // This exists because the semantic WinRT control objects report
          // nothing on this hardware while the Windows Settings camera page
          // and the vendor utility both visibly adjust these same devices, so
          // the controls demonstrably exist below the WinRT control surface.
          //
          // EXPERIMENTAL DESIGN -- read the VideoProcAmp results first. The
          // maintainer has confirmed Settings adjusts Brightness, Contrast,
          // Sharpness and Saturation on both cameras, so those four are a
          // POSITIVE CONTROL for the mechanism itself:
          //   * if VideoProcAmp reads succeed, the route works, and a
          //     not_supported on CameraControl focus/exposure/iris is then a
          //     genuine hardware negative;
          //   * if even VideoProcAmp fails, this API cannot reach KS property
          //     sets this way and NOTHING may be concluded about the camera's
          //     actual capabilities from the CameraControl results.
          // Without that control a negative here would be uninterpretable.
          //
          // Two identifier encodings are tried per property because the
          // expected form is not documented anywhere we have been able to
          // confirm: the DEVPROPKEY-style "{set-guid} pid" string, and the
          // raw extended-id blob (GUID bytes followed by the property id).
          // Reporting per-encoding status is the point; a size_required or
          // size_too_small status is itself a positive signal, since it means
          // the property was found and only the buffer was wrong.
          const auto ks_status_name =
              [](winrt::Windows::Media::Devices::VideoDeviceControllerGetDevicePropertyStatus s) {
                using S =
                    winrt::Windows::Media::Devices::VideoDeviceControllerGetDevicePropertyStatus;
                switch (s) {
                  case S::Success: return "success";
                  case S::UnknownFailure: return "unknown_failure";
                  case S::BufferTooSmall: return "buffer_too_small";
                  case S::NotSupported: return "not_supported";
                  case S::DeviceNotAvailable: return "device_unavailable";
                  case S::MaxPropertyValueSizeTooSmall: return "size_too_small";
                  case S::MaxPropertyValueSizeRequired: return "size_required";
                }
                return "unrecognized";
              };

          // KS property payloads are LONG-based structs, so render the raw
          // byte length plus the int32 sequence and let the actual data reveal
          // the layout, rather than assuming one. Length disambiguates: 12
          // bytes is a bare Value/Flags/Capabilities triple, 36 bytes is that
          // triple behind a 24-byte KSPROPERTY header, and the focal-length
          // payload carries three LONGs (ocular, objective min, objective max)
          // in the same fashion.
          const auto describe_property_value =
              [](const winrt::Windows::Foundation::IInspectable& value) -> std::string {
            if (!value) return "value_null";
            try {
              const auto property_value = value.as<winrt::Windows::Foundation::IPropertyValue>();
              const auto type = property_value.Type();
              if (type != winrt::Windows::Foundation::PropertyType::UInt8Array) {
                return "value_type=" + std::to_string(static_cast<int32_t>(type));
              }
              winrt::com_array<uint8_t> bytes;
              property_value.GetUInt8Array(bytes);
              // The call returns a buffer of exactly the requested maximum
              // size with the real payload at the front, so decode a fixed
              // leading window rather than the whole (mostly padding) array.
              std::string out = "len=" + std::to_string(bytes.size()) + " i32=[";
              const uint32_t max_words = 14u;
              for (uint32_t word = 0; word < max_words; ++word) {
                const uint32_t offset = word * 4u;
                if (offset + 4u > bytes.size()) break;
                int32_t decoded = 0;
                std::memcpy(&decoded, bytes.data() + offset, sizeof(decoded));
                if (word != 0u) out += ",";
                out += std::to_string(decoded);
              }
              out += "]";
              return out;
            } catch (...) {
              return "value_decode_threw";
            }
          };

          // The byte-blob encoding (GetDevicePropertyByExtendedId) threw on
          // every property in the previous run, so only the DEVPROPKEY-style
          // string form is used here. 4096 rather than the earlier 256 because
          // every VideoProcAmp read came back MaxPropertyValueSizeTooSmall --
          // which proved the properties exist, but yielded no data.
          const auto probe_ks_property = [&](const wchar_t* set_guid_text,
                                             uint32_t property_id) -> std::string {
            try {
              const auto max_size =
                  winrt::box_value(4096u)
                      .as<winrt::Windows::Foundation::IReference<uint32_t>>();
              const std::wstring id_text =
                  std::wstring(L"{") + set_guid_text + L"} " + std::to_wstring(property_id);
              const auto result = vdc.GetDevicePropertyById(winrt::hstring(id_text), max_size);
              std::string out = ks_status_name(result.Status());
              if (result.Status() ==
                  winrt::Windows::Media::Devices::VideoDeviceControllerGetDevicePropertyStatus::
                      Success) {
                out += " " + describe_property_value(result.Value());
              }
              return out;
            } catch (const winrt::hresult_error& e) {
              return "threw hr=0x" + std::to_string(static_cast<uint32_t>(e.code()));
            } catch (...) {
              return "threw";
            }
          };

          const auto describe_ks_videoprocamp = [&]() -> std::string {
            static constexpr wchar_t kSet[] = L"C6E13360-30AC-11D0-A18C-00A0C9118956";
            return "brightness[" +
                   probe_ks_property(kSet, KSPROPERTY_VIDEOPROCAMP_BRIGHTNESS) +
                   "] saturation[" +
                   probe_ks_property(kSet, KSPROPERTY_VIDEOPROCAMP_SATURATION) +
                   "] sharpness[" +
                   probe_ks_property(kSet, KSPROPERTY_VIDEOPROCAMP_SHARPNESS) +
                   "]";
          };

          const auto describe_ks_cameracontrol = [&]() -> std::string {
            static constexpr wchar_t kSet[] = L"C6E13370-30AC-11D0-A18C-00A0C9118956";
            return "exposure[" +
                   probe_ks_property(kSet, KSPROPERTY_CAMERACONTROL_EXPOSURE) +
                   "] iris[" +
                   probe_ks_property(kSet, KSPROPERTY_CAMERACONTROL_IRIS) +
                   "] focus[" +
                   probe_ks_property(kSet, KSPROPERTY_CAMERACONTROL_FOCUS) +
                   "] focal_length[" +
                   probe_ks_property(kSet, KSPROPERTY_CAMERACONTROL_FOCAL_LENGTH) +
                   "]";
          };

          // Single-value KS read at the offsets the previous run established:
          // 24 bytes of KSPROPERTY header, then Value, then Flags.
          const auto note_capability_state = [&](const std::string& stage) {
            note(ep_tag + "_controls_" + stage, describe_controls());
            note(ep_tag + "_vps_" + stage, describe_vps());
            note(ep_tag + "_focus_numbers_" + stage, describe_focus_numbers());
            note(ep_tag + "_capture_settings_" + stage, describe_capture_settings());
            note(ep_tag + "_ks_videoprocamp_" + stage, describe_ks_videoprocamp());
            note(ep_tag + "_ks_cameracontrol_" + stage, describe_ks_cameracontrol());
          };

          note(ep_tag + "_name", endpoints[ep_idx].name);
          note_capability_state("cold");

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
            // Checks the real WinRT path to relative camera pose
            // (MediaFrameSourceInfo.CoordinateSystem -> SpatialCoordinateSystem;
            // a real pose value would come from TryGetTransformTo against a
            // second coordinate system -- a rig camera, or a motion-sensor
            // locator). Informational only: whether this is even non-null
            // tells us if this hardware is spatially-locatable at all.
            try {
              const auto coord_system = color_source.Info().CoordinateSystem();
              note(ep_tag + "_coordinate_system_present", coord_system ? "true" : "false");
            } catch (const winrt::hresult_error& e) {
              note(ep_tag + "_coordinate_system_probe_failed",
                   "hr=0x" + std::to_string(static_cast<uint32_t>(e.code())));
            } catch (...) {
              note(ep_tag + "_coordinate_system_probe_failed", "unknown_exception");
            }

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
              note_capability_state("frame_reader_running");
              (void)reader.StopAsync().get();
            }
            reader.Close();
          }

          // Third state, and the one prior probing never covered. The
          // VideoDeviceController docs say some drivers cannot report which
          // controls they support until the *preview* is running -- and a
          // MediaFrameReader is not a preview (MediaStreamType::VideoPreview
          // / StartPreviewAsync is a distinct mechanism). If controls appear
          // here but not above, this provider's whole capability model has to
          // account for preview state. The frame reader is stopped and closed
          // first so preview is the only variable that changed.
          //
          // This also tests the open risk that StartPreviewAsync needs a
          // preview sink: this validator is a console app and the real
          // consumer is a GDExtension inside Godot, so both surfaces are
          // effectively headless. A failure here is itself the finding, not a
          // validator bug -- record the HRESULT rather than swallowing it.
          try {
            diag_capture.StartPreviewAsync().get();
            note(ep_tag + "_preview_started", "true");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            note_capability_state("preview_running");
            try {
              diag_capture.StopPreviewAsync().get();
            } catch (...) {
              note(ep_tag + "_preview_stop_failed", "true");
            }
          } catch (const winrt::hresult_error& e) {
            note(ep_tag + "_preview_started",
                 "false hr=0x" + std::to_string(static_cast<uint32_t>(e.code())));
          } catch (...) {
            note(ep_tag + "_preview_started", "false unknown_exception");
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
