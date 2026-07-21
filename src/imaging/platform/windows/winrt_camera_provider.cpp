// CamBANG windows_winrt platform provider implementation (C++/WinRT).
// See winrt_camera_provider.h for the architectural overview and
// docs/provider_implementation_brief.md for the contract this adapts to.
//
// Backend surface: winrt::Windows::Media::Capture::MediaCapture with
// Windows.Media.Capture.Frames MediaFrameReader delivering Bgra8 software
// bitmaps. Requires a C++/WinRT-capable toolchain (MSVC + Windows SDK).

#include "imaging/platform/windows/winrt_camera_provider.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <MemoryBuffer.h> // ::Windows::Foundation::IMemoryBufferByteAccess

#include <winrt/base.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Devices.Core.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.Media.MediaProperties.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace cambang {
namespace winrt_detail {

namespace wf = winrt::Windows::Foundation;
namespace wde = winrt::Windows::Devices::Enumeration;
namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wmc = winrt::Windows::Media::Capture;
namespace wmcf = winrt::Windows::Media::Capture::Frames;
namespace wmd = winrt::Windows::Media::Devices;
namespace wmdc = winrt::Windows::Media::Devices::Core;
namespace wmm = winrt::Windows::Media::MediaProperties;

namespace {

void log_line(const char* fmt, ...) {
  char buffer[512];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  std::fprintf(stderr, "[CamBANG][winrt_provider] %s\n", buffer);
  std::fflush(stderr);
}

ProviderError provider_error_from_hresult(winrt::hresult hr) noexcept {
  switch (static_cast<int32_t>(hr)) {
    case static_cast<int32_t>(0x80070005): // E_ACCESSDENIED (camera consent)
      return ProviderError::ERR_PLATFORM_CONSTRAINT;
    case static_cast<int32_t>(0x800710DF): // ERROR_DEVICE_NOT_AVAILABLE
    case static_cast<int32_t>(0x80070490): // ERROR_NOT_FOUND
      return ProviderError::ERR_PLATFORM_CONSTRAINT;
    case static_cast<int32_t>(0x800705AA): // ERROR_NO_SYSTEM_RESOURCES
      return ProviderError::ERR_TRANSIENT_FAILURE;
    default:
      return ProviderError::ERR_PROVIDER_FAILED;
  }
}

std::string hstring_to_utf8(const winrt::hstring& h) {
  if (h.empty()) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, h.c_str(), static_cast<int>(h.size()),
                                    nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, h.c_str(), static_cast<int>(h.size()), s.data(), n,
                      nullptr, nullptr);
  return s;
}

winrt::hstring utf8_to_hstring(const std::string& s) {
  if (s.empty()) return winrt::hstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                    nullptr, 0);
  if (n <= 0) return winrt::hstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return winrt::hstring(w);
}

// Bounded completion wait for a WinRT async op on the control thread. On
// timeout the operation is cancelled so the control thread does not stay
// wedged behind it. Returns true only for successful completion.
template <typename Async>
bool wait_async_bounded(const Async& op, uint32_t timeout_ms) {
  if (op.wait_for(std::chrono::milliseconds(timeout_ms)) ==
      wf::AsyncStatus::Completed) {
    return true;
  }
  try {
    op.Cancel();
  } catch (...) {
  }
  return false;
}

} // namespace

// ---------------------------------------------------------------------------
// BoundedControlExecutor
// ---------------------------------------------------------------------------

BoundedControlExecutor::~BoundedControlExecutor() { stop(); }

bool BoundedControlExecutor::start() noexcept {
  if (running_.load(std::memory_order_acquire)) {
    return false;
  }
  try {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stop_requested_ = false;
      q_.clear();
    }
    worker_ = std::thread([this] { thread_main_(); });
  } catch (...) {
    return false;
  }
  running_.store(true, std::memory_order_release);
  return true;
}

void BoundedControlExecutor::stop() noexcept {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_requested_ = true;
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    // The control thread only runs bounded backend jobs; a wedged driver can
    // stall it indefinitely, in which case abandonment (detach) mirrors
    // Core's own wedged-thread posture rather than wedging teardown.
    try {
      worker_.join();
    } catch (...) {
      worker_.detach();
    }
  }
  running_.store(false, std::memory_order_release);
}

bool BoundedControlExecutor::run_bounded(
    std::function<void(const AbandonToken&)> job,
    std::shared_ptr<AbandonToken> token,
    uint32_t timeout_ms) noexcept {
  if (!running_.load(std::memory_order_acquire) || !job || !token) {
    return false;
  }
  auto done = std::make_shared<std::promise<void>>();
  std::future<void> fut;
  try {
    fut = done->get_future();
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stop_requested_) {
        return false;
      }
      q_.push_back(Entry{std::move(job), token, done});
    }
  } catch (...) {
    return false;
  }
  cv_.notify_one();
  if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
    token->abandoned.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

void BoundedControlExecutor::thread_main_() noexcept {
  bool apartment_initialized = false;
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    apartment_initialized = true;
  } catch (...) {
    // Proceed uninitialised only if the thread already had an apartment.
  }
  for (;;) {
    Entry entry;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [this] { return stop_requested_ || !q_.empty(); });
      if (q_.empty()) {
        break; // stop requested and drained
      }
      entry = std::move(q_.front());
      q_.pop_front();
    }
    try {
      entry.job(*entry.token);
    } catch (...) {
      // Backend jobs contain their own failures; nothing may escape here.
    }
    entry.done->set_value();
  }
  if (apartment_initialized) {
    winrt::uninit_apartment();
  }
}

// ---------------------------------------------------------------------------
// Frame conversion (Bgra8 rows -> packed RGBA/BGRA)
// ---------------------------------------------------------------------------

namespace {

void convert_bgra_rows(const uint8_t* scanline0,
                       int32_t pitch,
                       uint32_t width,
                       uint32_t height,
                       uint32_t dst_fourcc,
                       uint8_t* dst) {
  const size_t row_bytes = static_cast<size_t>(width) * 4u;
  const bool to_rgba = (dst_fourcc == FOURCC_RGBA);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = scanline0 + static_cast<ptrdiff_t>(pitch) * static_cast<ptrdiff_t>(y);
    uint8_t* out = dst + row_bytes * y;
    if (to_rgba) {
      for (uint32_t x = 0; x < width; ++x) {
        out[4 * x + 0] = src[4 * x + 2];
        out[4 * x + 1] = src[4 * x + 1];
        out[4 * x + 2] = src[4 * x + 0];
        out[4 * x + 3] = 0xFF;
      }
    } else { // BGRA: same channel order; force opaque alpha.
      std::memcpy(out, src, row_bytes);
      for (uint32_t x = 0; x < width; ++x) {
        out[4 * x + 3] = 0xFF;
      }
    }
  }
}

// Copies a Bgra8 SoftwareBitmap into dst (width*height*4, requested fourcc).
bool convert_software_bitmap(const wgi::SoftwareBitmap& bitmap,
                             uint32_t width,
                             uint32_t height,
                             uint32_t dst_fourcc,
                             uint8_t* dst) {
  if (!bitmap || bitmap.BitmapPixelFormat() != wgi::BitmapPixelFormat::Bgra8) {
    return false;
  }
  if (static_cast<uint32_t>(bitmap.PixelWidth()) != width ||
      static_cast<uint32_t>(bitmap.PixelHeight()) != height) {
    return false;
  }
  wgi::BitmapBuffer buffer = bitmap.LockBuffer(wgi::BitmapBufferAccessMode::Read);
  bool ok = false;
  {
    wf::IMemoryBufferReference reference = buffer.CreateReference();
    auto byte_access =
        reference.as<::Windows::Foundation::IMemoryBufferByteAccess>();
    uint8_t* data = nullptr;
    uint32_t capacity = 0;
    if (SUCCEEDED(byte_access->GetBuffer(&data, &capacity)) && data) {
      const wgi::BitmapPlaneDescription plane = buffer.GetPlaneDescription(0);
      const uint8_t* scanline0 = data + plane.StartIndex;
      const size_t needed = static_cast<size_t>(plane.Stride) * height;
      if (capacity >= plane.StartIndex + needed) {
        convert_bgra_rows(scanline0, plane.Stride, width, height, dst_fourcc, dst);
        ok = true;
      }
    }
    reference.Close();
  }
  buffer.Close();
  return ok;
}

} // namespace

// ---------------------------------------------------------------------------
// Per-device backend
// ---------------------------------------------------------------------------

struct StreamProduction {
  uint64_t stream_id = 0;
  uint64_t device_instance_id = 0;
  uint64_t acquisition_session_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fourcc = 0;
  CoreRetainedProductionPlan plan{};
  uint64_t frames_posted = 0;
  uint64_t convert_failures = 0;

  struct BufferSlot {
    std::vector<uint8_t> bytes;
    std::atomic<bool> in_use{false};
  };
  std::vector<std::shared_ptr<BufferSlot>> pool;
  size_t cursor = 0;

  // Guarded by DeviceBackend::m.
  bool producing = false;
  uint64_t pool_exhausted_drops = 0;
};

struct CaptureWaiter {
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  bool ok = false;
  ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
  uint32_t width = 0;   // requested output geometry/format (set by the worker)
  uint32_t height = 0;
  uint32_t fourcc = 0;
  std::shared_ptr<std::vector<uint8_t>> bytes;
  bool has_sample_time = false;
  int64_t sample_time_100ns = 0;

  // Optional camera intrinsics/distortion read from the delivered frame,
  // when the device/driver genuinely supplies them. Never fabricated.
  bool has_intrinsics = false;
  float focal_length_x = 0.0f;
  float focal_length_y = 0.0f;
  float principal_point_x = 0.0f;
  float principal_point_y = 0.0f;
  float radial_k1 = 0.0f, radial_k2 = 0.0f, radial_k3 = 0.0f;
  float tangential_p1 = 0.0f, tangential_p2 = 0.0f;
  uint32_t intrinsics_reference_width = 0;
  uint32_t intrinsics_reference_height = 0;
};

struct DeviceBackend : std::enable_shared_from_this<DeviceBackend> {
  // Serializes reader realization/geometry/start across the core thread and
  // capture workers without either holding provider state_mutex_.
  // Ordering: configure_mutex before m; state_mutex_ is never taken inside.
  std::mutex configure_mutex;

  std::mutex m;
  wmc::MediaCapture capture{nullptr};
  wmcf::MediaFrameSource frame_source{nullptr};
  wmcf::MediaFrameReader reader{nullptr};
  // Still-capture pipeline, prepared lazily on first capture. Stills come
  // from here, not from the frame reader above, which serves streams only.
  wmc::LowLagPhotoCapture low_lag_photo{nullptr};
  winrt::event_token frame_token{};
  winrt::event_token failed_token{};
  bool reader_started = false;

  uint64_t device_instance_id = 0;
  uint64_t root_id = 0;
  uint64_t acquisition_session_id = 0; // core-issued native id once realized
  CBProviderStrand* strand = nullptr;  // provider outlives all backends

  bool closed = false;   // set before WinRT objects are released
  bool failed = false;
  uint32_t configured_w = 0;
  uint32_t configured_h = 0;
  std::atomic<uint64_t> frame_arrived_count{0};
  std::atomic<uint64_t> frame_handler_errors{0};

  std::shared_ptr<StreamProduction> stream;
  std::deque<std::shared_ptr<CaptureWaiter>> waiters;

  // Caller holds m.
  void fail_all_waiters_locked(ProviderError error) {
    for (auto& w : waiters) {
      std::lock_guard<std::mutex> wl(w->m);
      if (!w->done) {
        w->done = true;
        w->ok = false;
        w->error = error;
        w->cv.notify_all();
      }
    }
    waiters.clear();
  }

  // Caller holds m. Latches backend failure and posts the truthful facts.
  void latch_failure_locked(ProviderError error) {
    if (failed) {
      return;
    }
    failed = true;
    fail_all_waiters_locked(error);
    if (strand) {
      strand->post_device_error(device_instance_id, error);
      if (stream && stream->producing) {
        stream->producing = false;
        strand->post_stream_error(stream->stream_id, error);
        strand->post_stream_stopped(stream->stream_id, error);
      }
    }
  }
};

// Frame release leases: FrameView.release must stay valid on any thread and
// with any provider-side storage teardown ordering, so each posted frame owns
// its backing through a heap lease (matches SyntheticProvider's pattern).
struct StreamFrameLease {
  std::shared_ptr<StreamProduction::BufferSlot> slot;
};

struct CaptureFrameLease {
  std::shared_ptr<std::vector<uint8_t>> bytes;
};

namespace {

void release_stream_frame(void* user, const FrameView* /*frame*/) {
  auto* lease = static_cast<StreamFrameLease*>(user);
  if (!lease) return;
  if (lease->slot) {
    lease->slot->in_use.store(false, std::memory_order_release);
  }
  delete lease;
}

void release_capture_frame(void* user, const FrameView* /*frame*/) {
  delete static_cast<CaptureFrameLease*>(user);
}

// Monotonic 100ns mark for the still-capture path, which has no frame
// timestamp of its own. QPC is the same clock family WinRT SystemRelativeTime
// derives from, but that equality is assumed rather than verified here, so
// marks built on it make no cross-frame claim (see
// make_provider_observed_timing below).
int64_t provider_monotonic_100ns() noexcept {
  LARGE_INTEGER frequency{};
  LARGE_INTEGER now{};
  if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
      !QueryPerformanceCounter(&now) || now.QuadPart < 0) {
    return -1;
  }
  // Split to avoid overflowing the multiply on long-uptime machines.
  const int64_t whole_seconds = now.QuadPart / frequency.QuadPart;
  const int64_t remainder = now.QuadPart % frequency.QuadPart;
  return whole_seconds * 10'000'000LL + (remainder * 10'000'000LL) / frequency.QuadPart;
}

// Timing for a still captured through the photo pipeline. The mark is taken
// when the capture call returns, so it trails the exposure by the pipeline's
// processing latency and cannot be compared against stream-frame marks, whose
// domain equality is unverified. SAME_IMAGE_ONLY states exactly that.
std::optional<SourcedFact<ImageAcquisitionTiming>> make_provider_observed_timing(
    int64_t mark_100ns) {
  if (mark_100ns < 0) {
    return std::nullopt;
  }
  const auto tick_period = TickPeriod::create(100, 1);
  if (!tick_period) {
    return std::nullopt;
  }
  const auto timing = ImageAcquisitionTiming::create(
      mark_100ns,
      *tick_period,
      ImageAcquisitionClockDomain::PROVIDER_MONOTONIC,
      ImageAcquisitionReferenceEvent::PROVIDER_OBSERVED,
      ImageAcquisitionComparability::SAME_IMAGE_ONLY);
  if (!timing) {
    return std::nullopt;
  }
  return SourcedFact<ImageAcquisitionTiming>{*timing, FactOrigin::NATIVE_REPORTED};
}

std::optional<SourcedFact<ImageAcquisitionTiming>> make_acquisition_timing(
    int64_t sample_time_100ns) {
  if (sample_time_100ns < 0) {
    return std::nullopt;
  }
  // WinRT SystemRelativeTime marks tick at 100ns in the backend's own
  // system-relative monotonic domain.
  const auto tick_period = TickPeriod::create(100, 1);
  if (!tick_period) {
    return std::nullopt;
  }
  const auto timing = ImageAcquisitionTiming::create(
      sample_time_100ns,
      *tick_period,
      ImageAcquisitionClockDomain::PROVIDER_MONOTONIC,
      ImageAcquisitionReferenceEvent::PROVIDER_OBSERVED,
      ImageAcquisitionComparability::SAME_DEVICE);
  if (!timing) {
    return std::nullopt;
  }
  return SourcedFact<ImageAcquisitionTiming>{*timing, FactOrigin::NATIVE_REPORTED};
}

// Routes one arrived frame to the pending capture waiter (non-lossy capture
// truth first) and then to the repeating stream pool. Caller holds backend.m.
void deliver_frame_locked(DeviceBackend& backend,
                          const wgi::SoftwareBitmap& bitmap,
                          int64_t sample_time_100ns,
                          bool has_sample_time,
                          const wmdc::CameraIntrinsics& intrinsics) {
  if (!backend.waiters.empty()) {
    std::shared_ptr<CaptureWaiter> waiter = backend.waiters.front();
    backend.waiters.pop_front();
    auto bytes = std::make_shared<std::vector<uint8_t>>();
    bytes->resize(static_cast<size_t>(waiter->width) *
                  static_cast<size_t>(waiter->height) * 4u);
    const bool ok = convert_software_bitmap(bitmap, waiter->width, waiter->height,
                                            waiter->fourcc, bytes->data());
    if (!ok) {
      log_line("capture waiter convert failed: delivered %dx%d fmt=%d, expected %ux%u",
               bitmap ? bitmap.PixelWidth() : -1,
               bitmap ? bitmap.PixelHeight() : -1,
               bitmap ? static_cast<int>(bitmap.BitmapPixelFormat()) : -1,
               waiter->width, waiter->height);
    }
    std::lock_guard<std::mutex> wl(waiter->m);
    if (!waiter->done) {
      waiter->done = true;
      waiter->ok = ok;
      waiter->error = ok ? ProviderError::OK : ProviderError::ERR_PROVIDER_FAILED;
      if (ok) {
        waiter->bytes = std::move(bytes);
        waiter->has_sample_time = has_sample_time;
        waiter->sample_time_100ns = sample_time_100ns;
        // Truthful only: most UVC webcams report no CameraIntrinsics at all;
        // has_intrinsics stays false rather than fabricating a value.
        if (intrinsics) {
          try {
            const auto focal_length = intrinsics.FocalLength();
            const auto principal_point = intrinsics.PrincipalPoint();
            const auto radial = intrinsics.RadialDistortion();
            const auto tangential = intrinsics.TangentialDistortion();
            waiter->focal_length_x = focal_length.x;
            waiter->focal_length_y = focal_length.y;
            waiter->principal_point_x = principal_point.x;
            waiter->principal_point_y = principal_point.y;
            waiter->radial_k1 = radial.x;
            waiter->radial_k2 = radial.y;
            waiter->radial_k3 = radial.z;
            waiter->tangential_p1 = tangential.x;
            waiter->tangential_p2 = tangential.y;
            waiter->intrinsics_reference_width = intrinsics.ImageWidth();
            waiter->intrinsics_reference_height = intrinsics.ImageHeight();
            waiter->has_intrinsics = true;
          } catch (...) {
            // Leave has_intrinsics false; never post a partially-read fact.
          }
        }
      }
      waiter->cv.notify_all();
    }
  }

  StreamProduction* s = backend.stream.get();
  if (!s || !s->producing || !backend.strand) {
    return;
  }

  std::shared_ptr<StreamProduction::BufferSlot> slot;
  const size_t n = s->pool.size();
  for (size_t probe = 0; probe < n; ++probe) {
    const size_t idx = (s->cursor + probe) % n;
    auto& cand = s->pool[idx];
    bool expected = false;
    if (cand && cand->in_use.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
      slot = cand;
      s->cursor = (idx + 1) % n;
      break;
    }
  }
  if (!slot) {
    ++s->pool_exhausted_drops;
    if ((s->pool_exhausted_drops & (s->pool_exhausted_drops - 1)) == 0) {
      log_line("stream=%llu frame pool exhausted (drops=%llu)",
               static_cast<unsigned long long>(s->stream_id),
               static_cast<unsigned long long>(s->pool_exhausted_drops));
    }
    return; // repeating frames are lossy
  }

  if (!convert_software_bitmap(bitmap, s->width, s->height, s->fourcc,
                               slot->bytes.data())) {
    slot->in_use.store(false, std::memory_order_release);
    ++s->convert_failures;
    if ((s->convert_failures & (s->convert_failures - 1)) == 0) {
      log_line("stream=%llu frame convert failed: delivered %dx%d fmt=%d, expected %ux%u (failures=%llu)",
               static_cast<unsigned long long>(s->stream_id),
               bitmap ? bitmap.PixelWidth() : -1,
               bitmap ? bitmap.PixelHeight() : -1,
               bitmap ? static_cast<int>(bitmap.BitmapPixelFormat()) : -1,
               s->width, s->height,
               static_cast<unsigned long long>(s->convert_failures));
    }
    return;
  }
  ++s->frames_posted;

  FrameView fv{};
  fv.device_instance_id = s->device_instance_id;
  fv.stream_id = s->stream_id;
  fv.acquisition_session_id = s->acquisition_session_id;
  fv.capture_id = 0;
  fv.width = s->width;
  fv.height = s->height;
  fv.format_fourcc = s->fourcc;
  if (has_sample_time) {
    fv.acquisition_timing = make_acquisition_timing(sample_time_100ns);
  }
  fv.data = slot->bytes.data();
  fv.size_bytes = slot->bytes.size();
  fv.stride_bytes = s->width * 4u;
  fv.requested_retained_plan = s->plan;
  fv.release = &release_stream_frame;
  fv.release_user = new StreamFrameLease{slot};
  backend.strand->post_frame(fv);
}

} // namespace

} // namespace winrt_detail

// ---------------------------------------------------------------------------
// WinrtCameraProvider
// ---------------------------------------------------------------------------

using winrt_detail::BoundedControlExecutor;
using winrt_detail::CaptureWaiter;
using winrt_detail::DeviceBackend;
using winrt_detail::StreamProduction;
namespace wf = winrt_detail::wf;
namespace wde = winrt_detail::wde;
namespace wgi = winrt_detail::wgi;
namespace wmc = winrt_detail::wmc;
namespace wmcf = winrt_detail::wmcf;
namespace wmd = winrt_detail::wmd;
namespace wmdc = winrt_detail::wmdc;
namespace wmm = winrt_detail::wmm;

namespace {

// Windows camera consent-store preflight (no UI, no hardware). Only an
// explicit "Deny" blocks; unreadable/missing keys stay Ready so real failures
// surface truthfully at open time instead of being masked by a check failure.
bool consent_store_value_is_deny(HKEY root, const char* subkey) noexcept {
  char value[16] = {};
  DWORD size = sizeof(value);
  const LSTATUS st = RegGetValueA(root, subkey, "Value", RRF_RT_REG_SZ,
                                  nullptr, value, &size);
  return st == ERROR_SUCCESS && std::strncmp(value, "Deny", 4) == 0;
}

constexpr const char* kConsentKey =
    "Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam";
constexpr const char* kConsentKeyNonPackaged =
    "Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam\\NonPackaged";

} // namespace

WinrtCameraProvider::~WinrtCameraProvider() {
  if (initialized_.load(std::memory_order_acquire)) {
    (void)shutdown();
  }
}

ProviderAccessStatus WinrtCameraProvider::check_access_readiness() noexcept {
  if (consent_store_value_is_deny(HKEY_LOCAL_MACHINE, kConsentKey) ||
      consent_store_value_is_deny(HKEY_CURRENT_USER, kConsentKey) ||
      consent_store_value_is_deny(HKEY_CURRENT_USER, kConsentKeyNonPackaged)) {
    return ProviderAccessStatus::failure(
        ProviderAccessCode::PermissionDenied, "webcam_consent_denied", true);
  }
  return ProviderAccessStatus::ready("windows_winrt_ready");
}

StreamTemplate WinrtCameraProvider::stream_template() const {
  // Deterministic default; no backend I/O (brief §2). 640x480 is the
  // universally supported UVC baseline mode.
  StreamTemplate t{};
  t.profile.width = 640;
  t.profile.height = 480;
  t.profile.format_fourcc = FOURCC_RGBA;
  t.profile.target_fps_min = 0;
  t.profile.target_fps_max = 30;
  t.picture = PictureConfig{};
  return t;
}

CaptureTemplate WinrtCameraProvider::capture_template() const {
  CaptureTemplate t{};
  t.profile = stream_template().profile;
  t.picture = PictureConfig{};
  return t;
}

ProducerBackingCapabilities WinrtCameraProvider::stream_backing_capabilities(
    const CaptureProfile& /*profile*/,
    const PictureConfig& /*picture*/) const noexcept {
  return ProducerBackingCapabilities{true, false, false};
}

ProducerBackingCapabilities WinrtCameraProvider::capture_backing_capabilities(
    const CaptureRequest& /*req*/) const noexcept {
  return ProducerBackingCapabilities{true, false, false};
}

uint64_t WinrtCameraProvider::alloc_native_id_(NativeObjectType type) {
  return callbacks_ ? callbacks_->allocate_native_id(type) : 0;
}

void WinrtCameraProvider::emit_native_created_(
    uint64_t native_id,
    NativeObjectType type,
    uint64_t root_id,
    uint64_t owner_device_id,
    uint64_t owner_acquisition_session_id,
    uint64_t owner_stream_id) {
  if (!callbacks_ || native_id == 0) {
    return;
  }
  NativeObjectCreateInfo info{};
  info.native_id = native_id;
  info.type = static_cast<uint32_t>(type);
  info.root_id = root_id;
  info.owner_device_instance_id = owner_device_id;
  info.owner_acquisition_session_id = owner_acquisition_session_id;
  info.owner_stream_id = owner_stream_id;
  info.owner_provider_native_id = provider_native_id_;
  info.has_created_ns = true;
  info.created_ns = callbacks_->core_monotonic_now_ns();
  strand_.post_native_object_created(info);
}

void WinrtCameraProvider::emit_native_destroyed_(uint64_t native_id) {
  if (!callbacks_ || native_id == 0) {
    return;
  }
  NativeObjectDestroyInfo info{};
  info.native_id = native_id;
  info.has_destroyed_ns = true;
  info.destroyed_ns = callbacks_->core_monotonic_now_ns();
  strand_.post_native_object_destroyed(info);
}

ProviderResult WinrtCameraProvider::initialize(IProviderCallbacks* callbacks) {
  if (initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!callbacks) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  if (!control_.start()) {
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  callbacks_ = callbacks;
  if (!strand_.start(callbacks_, "winrt_provider")) {
    callbacks_ = nullptr;
    control_.stop();
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  if (!start_capture_executor_()) {
    strand_.stop();
    callbacks_ = nullptr;
    control_.stop();
    return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
  }

  provider_native_id_ = alloc_native_id_(NativeObjectType::Provider);
  emit_native_created_(provider_native_id_, NativeObjectType::Provider, 0, 0, 0, 0);

  shutting_down_.store(false, std::memory_order_release);
  initialized_.store(true, std::memory_order_release);
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::enumerate_endpoints(
    std::vector<CameraEndpoint>& out_endpoints) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  struct EnumResult {
    std::vector<CameraEndpoint> endpoints;
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
    bool ok = false;
  };
  auto result = std::make_shared<EnumResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool completed = control_.run_bounded(
      [result](const BoundedControlExecutor::AbandonToken& t) {
        EnumResult local;
        try {
          auto op = wde::DeviceInformation::FindAllAsync(
              wde::DeviceClass::VideoCapture);
          if (!winrt_detail::wait_async_bounded(op, kControlJobTimeoutMs - 500)) {
            local.error = ProviderError::ERR_TIMEOUT;
          } else {
            for (const wde::DeviceInformation& info : op.GetResults()) {
              CameraEndpoint ep;
              ep.hardware_id = winrt_detail::hstring_to_utf8(info.Id());
              ep.name = winrt_detail::hstring_to_utf8(info.Name());
              if (!ep.hardware_id.empty()) {
                local.endpoints.push_back(std::move(ep));
              }
            }
            local.ok = true;
          }
        } catch (const winrt::hresult_error& e) {
          local.error = winrt_detail::provider_error_from_hresult(e.code());
        } catch (...) {
        }
        if (!t.abandoned.load(std::memory_order_acquire)) {
          *result = std::move(local);
        }
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (!result->ok) {
    return ProviderResult::failure(result->error);
  }
  out_endpoints = std::move(result->endpoints);
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::open_device(
    const std::string& hardware_id,
    uint64_t device_instance_id,
    uint64_t root_id) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (hardware_id.empty() || device_instance_id == 0 || root_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto& dev = devices_[device_instance_id];
  if (dev.open) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }

  auto backend = std::make_shared<DeviceBackend>();
  backend->device_instance_id = device_instance_id;
  backend->root_id = root_id;
  backend->strand = &strand_;

  struct OpenResult {
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
    bool ok = false;
  };
  auto result = std::make_shared<OpenResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const winrt::hstring device_hid = winrt_detail::utf8_to_hstring(hardware_id);
  const bool completed = control_.run_bounded(
      [result, backend, device_hid](const BoundedControlExecutor::AbandonToken& t) {
        OpenResult local;
        wmc::MediaCapture capture{nullptr};
        try {
          capture = wmc::MediaCapture();
          wmc::MediaCaptureInitializationSettings settings;
          settings.VideoDeviceId(device_hid);
          settings.StreamingCaptureMode(wmc::StreamingCaptureMode::Video);
          // ExclusiveControl is required, not preferred: honouring Core's
          // requested geometry goes through MediaFrameSource::SetFormatAsync,
          // which a SharedReadOnly open cannot perform. Opening read-only was
          // tried and fails start_stream outright, with no frames at all, so
          // do not "fix" this by switching sharing mode -- the contract also
          // forbids the workaround of inheriting whatever geometry the device
          // happens to be in.
          //
          // It was briefly suspected that holding exclusive control freezes
          // the camera's own autofocus. That is not so: a controlled
          // comparison holding one autofocus-capable device 45s under each
          // sharing mode showed autofocus and auto-exposure working normally
          // in both, confirmed by direct observation of the lens motor.
          settings.SharingMode(wmc::MediaCaptureSharingMode::ExclusiveControl);
          settings.MemoryPreference(wmc::MediaCaptureMemoryPreference::Cpu);
          auto op = capture.InitializeAsync(settings);
          if (!winrt_detail::wait_async_bounded(op, kControlJobTimeoutMs - 500)) {
            local.error = ProviderError::ERR_TIMEOUT;
          } else {
            op.GetResults();
            local.ok = true;
          }
        } catch (const winrt::hresult_error& e) {
          local.error = winrt_detail::provider_error_from_hresult(e.code());
          winrt_detail::log_line("MediaCapture initialize failed hr=0x%08X",
                                 static_cast<uint32_t>(e.code()));
        } catch (...) {
        }
        if (t.abandoned.load(std::memory_order_acquire)) {
          // Caller gave up: the acquisition is orphaned; release it here.
          if (local.ok && capture) {
            capture.Close();
          }
          return;
        }
        if (local.ok) {
          std::weak_ptr<DeviceBackend> weak = backend;
          std::lock_guard<std::mutex> bl(backend->m);
          backend->capture = capture;
          backend->failed_token = capture.Failed(
              [weak](const wmc::MediaCapture&,
                     const wmc::MediaCaptureFailedEventArgs& args) {
                std::shared_ptr<DeviceBackend> strong = weak.lock();
                if (!strong) {
                  return;
                }
                std::lock_guard<std::mutex> bl2(strong->m);
                if (strong->closed) {
                  return;
                }
                strong->latch_failure_locked(
                    winrt_detail::provider_error_from_hresult(
                        winrt::hresult(static_cast<int32_t>(args.Code()))));
              });
        }
        *result = local;
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (!result->ok) {
    return ProviderResult::failure(result->error);
  }

  dev.hardware_id = hardware_id;
  dev.device_instance_id = device_instance_id;
  dev.root_id = root_id;
  dev.open = true;
  dev.stream_id = 0;
  dev.native_id = alloc_native_id_(NativeObjectType::Device);
  dev.backend = backend;

  emit_native_created_(dev.native_id, NativeObjectType::Device, root_id,
                       device_instance_id, 0, 0);
  strand_.post_device_opened(device_instance_id);
  post_static_camera_facts_best_effort_(device_instance_id, hardware_id);
  return ProviderResult::success();
}

void WinrtCameraProvider::post_static_camera_facts_best_effort_(
    uint64_t device_instance_id, const std::string& hardware_id) {
  struct LookupResult {
    bool ok = false;
    bool has_panel = false;
    wde::Panel panel = wde::Panel::Unknown;
    uint32_t rotation_degrees_clockwise = 0;
  };
  auto result = std::make_shared<LookupResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const winrt::hstring device_hid = winrt_detail::utf8_to_hstring(hardware_id);
  const bool completed = control_.run_bounded(
      [result, device_hid](const BoundedControlExecutor::AbandonToken& t) {
        try {
          auto op = wde::DeviceInformation::CreateFromIdAsync(device_hid);
          if (!winrt_detail::wait_async_bounded(op, kControlJobTimeoutMs - 500)) {
            return;
          }
          const wde::DeviceInformation info = op.GetResults();
          if (t.abandoned.load(std::memory_order_acquire) || !info) {
            return;
          }
          const wde::EnclosureLocation enclosure = info.EnclosureLocation();
          if (!enclosure) {
            return; // device reports no enclosure location; omit, don't guess
          }
          LookupResult local;
          local.ok = true;
          local.has_panel = true;
          local.panel = enclosure.Panel();
          local.rotation_degrees_clockwise = enclosure.RotationAngleInDegreesClockwise();
          if (!t.abandoned.load(std::memory_order_acquire)) {
            *result = local;
          }
        } catch (...) {
          // Best-effort enrichment only; a failure here must never affect
          // open_device()'s already-committed success.
        }
      },
      token, kControlJobTimeoutMs);
  if (!completed || !result->ok) {
    winrt_detail::log_line(
        "static camera facts unavailable for device=%llu (enclosure location "
        "lookup timed out, failed, or device reports none)",
        static_cast<unsigned long long>(device_instance_id));
    return;
  }

  CameraStaticFacts facts{};
  // A device that reports a physical enclosure panel is physical hardware by
  // construction: a virtual camera has no chassis location to report. Derived
  // rather than native-reported -- the platform stated a panel, not a nature.
  // Devices without an enclosure location reach the early return above, so no
  // nature is claimed for USB or virtual cameras, which WinRT gives us no
  // reliable way to tell apart.
  facts.nature = SourcedFact<CameraNature>{CameraNature::PHYSICAL, FactOrigin::DERIVED};
  if (result->panel == wde::Panel::Front) {
    facts.facing = SourcedFact<CameraFacing>{CameraFacing::FRONT, FactOrigin::NATIVE_REPORTED};
  } else if (result->panel == wde::Panel::Back) {
    facts.facing = SourcedFact<CameraFacing>{CameraFacing::BACK, FactOrigin::NATIVE_REPORTED};
  }
  // Top/Bottom/Left/Right/Unknown don't map onto CamBANG's front/back/
  // external vocabulary without guessing; omitted rather than fabricated.

  const uint32_t rotation_mod = result->rotation_degrees_clockwise % 360u;
  // Real mountings are always axis-aligned in practice; round to the
  // nearest quarter-turn rather than requiring an exact match.
  const uint32_t nearest_quarter = ((rotation_mod + 45u) / 90u) % 4u;
  switch (nearest_quarter) {
    case 0u:
      facts.sensor_orientation = SourcedFact<SensorOrientationDegrees>{
          SensorOrientationDegrees::DEGREES_0, FactOrigin::NATIVE_REPORTED};
      break;
    case 1u:
      facts.sensor_orientation = SourcedFact<SensorOrientationDegrees>{
          SensorOrientationDegrees::DEGREES_90, FactOrigin::NATIVE_REPORTED};
      break;
    case 2u:
      facts.sensor_orientation = SourcedFact<SensorOrientationDegrees>{
          SensorOrientationDegrees::DEGREES_180, FactOrigin::NATIVE_REPORTED};
      break;
    default:
      facts.sensor_orientation = SourcedFact<SensorOrientationDegrees>{
          SensorOrientationDegrees::DEGREES_270, FactOrigin::NATIVE_REPORTED};
      break;
  }

  if (facts.facing || facts.nature || facts.sensor_orientation) {
    strand_.post_camera_static_facts(device_instance_id, ProviderCameraFacts{facts});
  }
}

ProviderResult WinrtCameraProvider::ensure_reader_realized_(
    const std::shared_ptr<DeviceBackend>& backend) {
  if (!backend) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  std::lock_guard<std::mutex> configure_lock(backend->configure_mutex);
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    if (backend->reader) {
      return backend->failed
                 ? ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED)
                 : ProviderResult::success();
    }
    if (!backend->capture) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
  }

  struct RealizeResult {
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
    bool ok = false;
  };
  auto result = std::make_shared<RealizeResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool completed = control_.run_bounded(
      [result, backend](const BoundedControlExecutor::AbandonToken& t) {
        RealizeResult local;
        wmcf::MediaFrameSource source{nullptr};
        wmcf::MediaFrameReader reader{nullptr};
        try {
          wmc::MediaCapture capture{nullptr};
          {
            std::lock_guard<std::mutex> bl(backend->m);
            capture = backend->capture;
          }
          if (!capture) {
            local.error = ProviderError::ERR_BAD_STATE;
          } else {
            // Pick the color video source (prefer record, accept preview).
            for (const auto& kv : capture.FrameSources()) {
              const wmcf::MediaFrameSource candidate = kv.Value();
              const auto info = candidate.Info();
              if (info.SourceKind() != wmcf::MediaFrameSourceKind::Color) {
                continue;
              }
              if (info.MediaStreamType() ==
                  winrt::Windows::Media::Capture::MediaStreamType::VideoRecord) {
                source = candidate;
                break;
              }
              if (!source &&
                  info.MediaStreamType() ==
                      winrt::Windows::Media::Capture::MediaStreamType::VideoPreview) {
                source = candidate;
              }
            }
            if (!source) {
              local.error = ProviderError::ERR_PLATFORM_CONSTRAINT;
              winrt_detail::log_line("no color video frame source on device");
            } else {
              auto op = capture.CreateFrameReaderAsync(
                  source, wmm::MediaEncodingSubtypes::Bgra8());
              if (!winrt_detail::wait_async_bounded(op, kControlJobTimeoutMs - 500)) {
                local.error = ProviderError::ERR_TIMEOUT;
              } else {
                reader = op.GetResults();
                reader.AcquisitionMode(
                    wmcf::MediaFrameReaderAcquisitionMode::Realtime);
                local.ok = true;
              }
            }
          }
        } catch (const winrt::hresult_error& e) {
          local.error = winrt_detail::provider_error_from_hresult(e.code());
          winrt_detail::log_line("frame reader realization failed hr=0x%08X",
                                 static_cast<uint32_t>(e.code()));
        } catch (...) {
        }
        if (t.abandoned.load(std::memory_order_acquire)) {
          if (reader) {
            reader.Close();
          }
          return;
        }
        if (local.ok) {
          std::weak_ptr<DeviceBackend> weak = backend;
          std::lock_guard<std::mutex> bl(backend->m);
          if (backend->closed) {
            local.ok = false;
            local.error = ProviderError::ERR_BAD_STATE;
          } else {
            backend->frame_source = source;
            backend->reader = reader;
            backend->frame_token = reader.FrameArrived(
                [weak](const wmcf::MediaFrameReader& rdr,
                       const wmcf::MediaFrameArrivedEventArgs&) {
                  std::shared_ptr<DeviceBackend> strong = weak.lock();
                  if (!strong) {
                    return;
                  }
                  strong->frame_arrived_count.fetch_add(1, std::memory_order_relaxed);
                  // Nothing may escape into the WinRT event dispatcher: an
                  // uncontained exception here silently kills the
                  // subscription (observed as one-frame-then-silence).
                  try {
                    wmcf::MediaFrameReference frame = rdr.TryAcquireLatestFrame();
                    if (!frame) {
                      return;
                    }
                    const auto video = frame.VideoMediaFrame();
                    if (!video) {
                      return;
                    }
                    const wgi::SoftwareBitmap bitmap = video.SoftwareBitmap();
                    if (!bitmap) {
                      return;
                    }
                    int64_t mark_100ns = 0;
                    bool has_mark = false;
                    if (const auto ts = frame.SystemRelativeTime()) {
                      mark_100ns = ts.Value().count();
                      has_mark = true;
                    }
                    // Null on most UVC webcams; real on some depth/
                    // professional cameras. video.CameraIntrinsics() is a
                    // plain property get, safe to call inline here.
                    const wmdc::CameraIntrinsics intrinsics = video.CameraIntrinsics();
                    {
                      std::lock_guard<std::mutex> bl2(strong->m);
                      if (strong->closed) {
                        return;
                      }
                      winrt_detail::deliver_frame_locked(*strong, bitmap, mark_100ns,
                                                         has_mark, intrinsics);
                    }
                    frame.Close();
                  } catch (const winrt::hresult_error& e) {
                    const uint64_t n = strong->frame_handler_errors.fetch_add(
                                           1, std::memory_order_relaxed) + 1;
                    if ((n & (n - 1)) == 0) {
                      winrt_detail::log_line(
                          "FrameArrived handler error hr=0x%08X (errors=%llu)",
                          static_cast<uint32_t>(e.code()),
                          static_cast<unsigned long long>(n));
                    }
                  } catch (...) {
                    winrt_detail::log_line("FrameArrived handler unknown exception");
                  }
                });
          }
        }
        if (!local.ok && reader) {
          reader.Close();
        }
        *result = local;
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    winrt_detail::log_line("reader realization timed out (device=%llu)",
                           static_cast<unsigned long long>(backend->device_instance_id));
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (!result->ok) {
    return ProviderResult::failure(result->error);
  }

  // AcquisitionSession native truth: the concretely realized frame reader.
  const uint64_t session_id = alloc_native_id_(NativeObjectType::AcquisitionSession);
  uint64_t root_id = 0;
  uint64_t device_instance_id = 0;
  {
    std::lock_guard<std::mutex> bl(backend->m);
    backend->acquisition_session_id = session_id;
    root_id = backend->root_id;
    device_instance_id = backend->device_instance_id;
  }
  emit_native_created_(session_id, NativeObjectType::AcquisitionSession,
                       root_id, device_instance_id, 0, 0);
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::ensure_reader_geometry_(
    const std::shared_ptr<DeviceBackend>& backend,
    uint32_t width,
    uint32_t height,
    uint32_t format_fourcc) {
  if (width == 0 || height == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (format_fourcc != FOURCC_RGBA && format_fourcc != FOURCC_BGRA) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  if (!backend) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  std::lock_guard<std::mutex> configure_lock(backend->configure_mutex);
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed || !backend->reader || backend->failed) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    if (backend->configured_w == width && backend->configured_h == height) {
      return ProviderResult::success();
    }
    // A started stream pins the negotiated geometry: reconfiguring the shared
    // source mid-stream would disrupt live delivery. Deterministic failure.
    if (backend->stream && backend->stream->producing) {
      return ProviderResult::failure(ProviderError::ERR_PLATFORM_CONSTRAINT);
    }
  }

  struct ConfigureResult {
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
    bool ok = false;
  };
  auto result = std::make_shared<ConfigureResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool completed = control_.run_bounded(
      [result, backend, width, height](const BoundedControlExecutor::AbandonToken& t) {
        ConfigureResult local;
        try {
          wmcf::MediaFrameSource source{nullptr};
          {
            std::lock_guard<std::mutex> bl(backend->m);
            source = backend->frame_source;
          }
          if (!source) {
            local.error = ProviderError::ERR_BAD_STATE;
          } else {
            wmcf::MediaFrameFormat chosen{nullptr};
            for (const wmcf::MediaFrameFormat& candidate : source.SupportedFormats()) {
              const auto vf = candidate.VideoFormat();
              if (vf && vf.Width() == width && vf.Height() == height) {
                chosen = candidate;
                break;
              }
            }
            if (!chosen) {
              // The backend cannot natively produce the effective geometry
              // Core supplied; never deliver silently-substituted geometry
              // (brief §6).
              winrt_detail::log_line(
                  "no native format matches requested %ux%u", width, height);
              local.error = ProviderError::ERR_PLATFORM_CONSTRAINT;
            } else {
              auto op = source.SetFormatAsync(chosen);
              if (!winrt_detail::wait_async_bounded(op, kControlJobTimeoutMs - 500)) {
                local.error = ProviderError::ERR_TIMEOUT;
              } else {
                op.GetResults();
                local.ok = true;
              }
            }
          }
        } catch (const winrt::hresult_error& e) {
          local.error = winrt_detail::provider_error_from_hresult(e.code());
          winrt_detail::log_line("SetFormatAsync failed hr=0x%08X (%ux%u)",
                                 static_cast<uint32_t>(e.code()), width, height);
        } catch (...) {
        }
        if (!t.abandoned.load(std::memory_order_acquire)) {
          *result = local;
        }
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    winrt_detail::log_line("media-format configure timed out (%ux%u)", width, height);
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (!result->ok) {
    return ProviderResult::failure(result->error);
  }

  std::lock_guard<std::mutex> bl(backend->m);
  backend->configured_w = width;
  backend->configured_h = height;
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::ensure_reader_started_(
    const std::shared_ptr<DeviceBackend>& backend) {
  if (!backend) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  std::lock_guard<std::mutex> configure_lock(backend->configure_mutex);
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed || !backend->reader || backend->failed) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    if (backend->reader_started) {
      return ProviderResult::success();
    }
  }

  struct StartResult {
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
    bool ok = false;
  };
  auto result = std::make_shared<StartResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool completed = control_.run_bounded(
      [result, backend](const BoundedControlExecutor::AbandonToken& t) {
        StartResult local;
        try {
          wmcf::MediaFrameReader reader{nullptr};
          {
            std::lock_guard<std::mutex> bl(backend->m);
            reader = backend->reader;
          }
          if (!reader) {
            local.error = ProviderError::ERR_BAD_STATE;
          } else {
            auto op = reader.StartAsync();
            if (!winrt_detail::wait_async_bounded(op, kControlJobTimeoutMs - 500)) {
              local.error = ProviderError::ERR_TIMEOUT;
            } else {
              switch (op.GetResults()) {
                case wmcf::MediaFrameReaderStartStatus::Success:
                  local.ok = true;
                  break;
                case wmcf::MediaFrameReaderStartStatus::DeviceNotAvailable:
                  local.error = ProviderError::ERR_TRANSIENT_FAILURE;
                  break;
                case wmcf::MediaFrameReaderStartStatus::ExclusiveControlNotAvailable:
                  local.error = ProviderError::ERR_BUSY;
                  break;
                default:
                  local.error = ProviderError::ERR_PLATFORM_CONSTRAINT;
                  break;
              }
            }
          }
        } catch (const winrt::hresult_error& e) {
          local.error = winrt_detail::provider_error_from_hresult(e.code());
          winrt_detail::log_line("reader StartAsync failed hr=0x%08X",
                                 static_cast<uint32_t>(e.code()));
        } catch (...) {
        }
        if (!t.abandoned.load(std::memory_order_acquire)) {
          *result = local;
        }
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (!result->ok) {
    return ProviderResult::failure(result->error);
  }
  std::lock_guard<std::mutex> bl(backend->m);
  backend->reader_started = true;
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::close_device(uint64_t device_instance_id) {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // In-flight capture jobs pin the device (they registered at admission and
  // erase only at terminal). Entry calls are core-thread serialized, so no
  // new admission can slip in after this check within this call.
  {
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    for (const auto& [key, gen] : in_flight_captures_) {
      (void)gen;
      if (key.device_instance_id == device_instance_id) {
        return ProviderResult::failure(ProviderError::ERR_BUSY);
      }
    }
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto it = devices_.find(device_instance_id);
  if (it == devices_.end() || !it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (it->second.stream_id != 0) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  DeviceState& dev = it->second;
  std::shared_ptr<DeviceBackend> backend = dev.backend;
  uint64_t session_native_id = 0;
  if (backend) {
    std::lock_guard<std::mutex> bl(backend->m);
    backend->closed = true;
    session_native_id = backend->acquisition_session_id;
    backend->fail_all_waiters_locked(ProviderError::ERR_SHUTTING_DOWN);
  }

  // Real release of WinRT objects on the control thread.
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  (void)control_.run_bounded(
      [backend](const BoundedControlExecutor::AbandonToken& /*t*/) {
        if (!backend) return;
        wmcf::MediaFrameReader reader{nullptr};
        wmc::MediaCapture capture{nullptr};
        wmc::LowLagPhotoCapture low_lag_photo{nullptr};
        winrt::event_token frame_token{};
        winrt::event_token failed_token{};
        {
          std::lock_guard<std::mutex> bl(backend->m);
          reader = std::exchange(backend->reader, nullptr);
          capture = std::exchange(backend->capture, nullptr);
          low_lag_photo = std::exchange(backend->low_lag_photo, nullptr);
          backend->frame_source = nullptr;
          frame_token = std::exchange(backend->frame_token, {});
          failed_token = std::exchange(backend->failed_token, {});
          backend->reader_started = false;
        }
        try {
          if (reader) {
            if (frame_token.value != 0) {
              reader.FrameArrived(frame_token);
            }
            auto stop_op = reader.StopAsync();
            (void)winrt_detail::wait_async_bounded(stop_op, kControlJobTimeoutMs - 500);
            reader.Close();
          }
          if (low_lag_photo) {
            auto finish_op = low_lag_photo.FinishAsync();
            (void)winrt_detail::wait_async_bounded(finish_op, kControlJobTimeoutMs - 500);
          }
          if (capture) {
            if (failed_token.value != 0) {
              capture.Failed(failed_token);
            }
            capture.Close();
          }
        } catch (...) {
          // Best-effort backend release; truthful facts are emitted by the
          // caller regardless (the objects are unreachable after this).
        }
      },
      token, kControlJobTimeoutMs);

  dev.open = false;
  dev.backend.reset();
  if (session_native_id != 0) {
    emit_native_destroyed_(session_native_id);
  }
  strand_.post_device_closed(device_instance_id);
  emit_native_destroyed_(dev.native_id);
  dev.native_id = 0;
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::create_stream(const StreamRequest& req) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (req.stream_id == 0 || req.device_instance_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto dev_it = devices_.find(req.device_instance_id);
  if (dev_it == devices_.end() || !dev_it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (dev_it->second.stream_id != 0) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }
  auto& st = streams_[req.stream_id];
  if (st.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  st.req = req;
  st.created = true;
  st.started = false;
  st.native_id = alloc_native_id_(NativeObjectType::Stream);
  dev_it->second.stream_id = req.stream_id;

  uint64_t session_native_id = 0;
  if (dev_it->second.backend) {
    std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
    session_native_id = dev_it->second.backend->acquisition_session_id;
  }
  emit_native_created_(st.native_id, NativeObjectType::Stream,
                       dev_it->second.root_id, req.device_instance_id,
                       session_native_id, req.stream_id);
  strand_.post_stream_created(req.stream_id);
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::destroy_stream(uint64_t stream_id) {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (stream_id == 0) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (st_it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  const uint64_t dev_id = st_it->second.req.device_instance_id;
  auto dev_it = devices_.find(dev_id);
  if (dev_it != devices_.end()) {
    if (dev_it->second.backend) {
      std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
      dev_it->second.backend->stream.reset();
    }
    if (dev_it->second.stream_id == stream_id) {
      dev_it->second.stream_id = 0;
    }
  }

  // Quiescence: posted frames referencing this stream drain before the stream
  // record disappears (slot storage itself is lease-owned and always safe).
  strand_.flush();

  const uint64_t native_id = st_it->second.native_id;
  streams_.erase(st_it);
  strand_.post_stream_destroyed(stream_id);
  emit_native_destroyed_(native_id);
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::start_stream(
    uint64_t stream_id,
    const CaptureProfile& profile,
    const PictureConfig& /*picture*/) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (st_it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  auto dev_it = devices_.find(st_it->second.req.device_instance_id);
  if (dev_it == devices_.end() || !dev_it->second.open) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  DeviceState& dev = dev_it->second;

  ProviderResult pr = ensure_reader_realized_(dev.backend);
  if (!pr.ok()) {
    return pr;
  }
  pr = ensure_reader_geometry_(dev.backend, profile.width, profile.height,
                               profile.format_fourcc);
  if (!pr.ok()) {
    return pr;
  }
  pr = ensure_reader_started_(dev.backend);
  if (!pr.ok()) {
    return pr;
  }

  StreamState& st = st_it->second;
  st.req.profile = profile;

  uint64_t session_native_id = 0;
  {
    std::lock_guard<std::mutex> bl(dev.backend->m);
    session_native_id = dev.backend->acquisition_session_id;
  }

  auto production = std::make_shared<StreamProduction>();
  production->stream_id = stream_id;
  production->device_instance_id = dev.device_instance_id;
  production->acquisition_session_id = session_native_id;
  production->width = profile.width;
  production->height = profile.height;
  production->fourcc = profile.format_fourcc;
  production->plan = st.req.requested_retained_plan;
  const size_t frame_bytes =
      static_cast<size_t>(profile.width) * static_cast<size_t>(profile.height) * 4u;
  production->pool.reserve(kStreamPoolSlots);
  for (size_t i = 0; i < kStreamPoolSlots; ++i) {
    auto slot = std::make_shared<StreamProduction::BufferSlot>();
    slot->bytes.resize(frame_bytes);
    production->pool.push_back(std::move(slot));
  }

  {
    std::lock_guard<std::mutex> bl(dev.backend->m);
    if (dev.backend->failed || dev.backend->closed) {
      return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
    }
    production->producing = true;
    dev.backend->stream = production;
  }

  st.started = true;
  strand_.post_stream_started(stream_id);
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::stop_stream(uint64_t stream_id) {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created || !st_it->second.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  auto dev_it = devices_.find(st_it->second.req.device_instance_id);
  bool already_stopped_by_error = false;
  if (dev_it != devices_.end() && dev_it->second.backend) {
    std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
    auto& stream = dev_it->second.backend->stream;
    if (stream && stream->stream_id == stream_id) {
      already_stopped_by_error = !stream->producing;
      stream->producing = false;
    }
  }

  st_it->second.started = false;
  if (!already_stopped_by_error) {
    // Posting after producing=false under the backend lock guarantees no
    // frame for this stream lands after the stopped fact.
    strand_.post_stream_stopped(stream_id, ProviderError::OK);
  }
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::update_stream_retained_production_plan(
    uint64_t stream_id,
    CoreRetainedProductionPlan requested_retained_plan) {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!requested_retained_plan.valid) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  std::lock_guard<std::mutex> state_lock(state_mutex_);
  auto st_it = streams_.find(stream_id);
  if (st_it == streams_.end() || !st_it->second.created) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  const ProducerBackingCapabilities caps = stream_backing_capabilities(
      st_it->second.req.profile, st_it->second.req.picture);
  if (!caps.viable(requested_retained_plan.posture)) {
    return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
  }
  st_it->second.req.requested_retained_plan = requested_retained_plan;

  auto dev_it = devices_.find(st_it->second.req.device_instance_id);
  if (dev_it != devices_.end() && dev_it->second.backend) {
    std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
    auto& stream = dev_it->second.backend->stream;
    if (stream && stream->stream_id == stream_id) {
      stream->plan = requested_retained_plan;
    }
  }
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::set_stream_picture_config(
    uint64_t /*stream_id*/, const PictureConfig& /*picture*/) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult WinrtCameraProvider::set_capture_picture_config(
    uint64_t /*device_instance_id*/, const PictureConfig& /*picture*/) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

// ---------------------------------------------------------------------------
// Still capture
// ---------------------------------------------------------------------------

bool WinrtCameraProvider::start_capture_executor_() noexcept {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  capture_admission_closed_ = false;
  capture_stop_requested_ = false;
  capture_queue_.clear();
  capture_active_jobs_ = 0;
  in_flight_captures_.clear();
  try {
    capture_workers_.reserve(kCaptureWorkerCount);
    for (size_t i = 0; i < kCaptureWorkerCount; ++i) {
      capture_workers_.emplace_back([this] { capture_worker_main_(); });
    }
  } catch (...) {
    capture_admission_closed_ = true;
    capture_stop_requested_ = true;
    capture_cv_.notify_all();
    for (auto& w : capture_workers_) {
      if (w.joinable()) w.join();
    }
    capture_workers_.clear();
    return false;
  }
  return true;
}

void WinrtCameraProvider::stop_and_join_capture_executor_() noexcept {
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    capture_admission_closed_ = true;
    capture_stop_requested_ = true;
  }
  capture_cv_.notify_all();
  for (auto& w : capture_workers_) {
    if (w.joinable()) {
      try {
        w.join();
      } catch (...) {
        w.detach();
      }
    }
  }
  capture_workers_.clear();
}

void WinrtCameraProvider::capture_worker_main_() noexcept {
  for (;;) {
    DeviceCaptureJob job;
    {
      std::unique_lock<std::mutex> lock(capture_mutex_);
      capture_cv_.wait(lock, [this] {
        return capture_stop_requested_ || !capture_queue_.empty();
      });
      if (capture_queue_.empty()) {
        return; // stop requested and queue fully drained
      }
      job = std::move(capture_queue_.front());
      capture_queue_.pop_front();
      ++capture_active_jobs_;
    }
    run_device_capture_job_(job);
    {
      std::lock_guard<std::mutex> lock(capture_mutex_);
      --capture_active_jobs_;
      in_flight_captures_.erase(
          InFlightKey{job.request.capture_id, job.request.device_instance_id});
    }
    capture_cv_.notify_all();
  }
}

bool WinrtCameraProvider::query_exposure_compensation_range_(
    const std::shared_ptr<DeviceBackend>& backend,
    float& out_min, float& out_max, float& out_step) {
  if (!backend) {
    return false;
  }
  wmc::MediaCapture capture{nullptr};
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed) {
      return false;
    }
    capture = backend->capture;
  }
  if (!capture) {
    return false;
  }
  try {
    wmd::ExposureCompensationControl ctrl =
        capture.VideoDeviceController().ExposureCompensationControl();
    if (!ctrl || !ctrl.Supported()) {
      return false;
    }
    out_min = ctrl.Min();
    out_max = ctrl.Max();
    out_step = ctrl.Step();
    // Defensive: a malformed range cannot be step-rounded safely; treat it
    // as unsupported rather than dividing by (near-)zero below.
    if (!(out_step > 0.0f) || !(out_max >= out_min)) {
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
}

bool WinrtCameraProvider::query_focus_state_at_infinity_(
    const std::shared_ptr<DeviceBackend>& backend) {
  if (!backend) {
    return false;
  }
  wmc::MediaCapture capture{nullptr};
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed) {
      return false;
    }
    capture = backend->capture;
  }
  if (!capture) {
    return false;
  }
  try {
    wmd::FocusControl ctrl = capture.VideoDeviceController().FocusControl();
    return ctrl && ctrl.Supported() && ctrl.Preset() == wmd::FocusPreset::AutoInfinity;
  } catch (...) {
    return false;
  }
}

bool WinrtCameraProvider::apply_exposure_compensation_bounded_(
    const std::shared_ptr<DeviceBackend>& backend,
    int32_t requested_milli_ev,
    int32_t& out_realized_milli_ev,
    ProviderError& out_error) noexcept {
  out_error = ProviderError::ERR_PROVIDER_FAILED;
  float min_ev = 0.0f, max_ev = 0.0f, step_ev = 0.0f;
  if (!query_exposure_compensation_range_(backend, min_ev, max_ev, step_ev)) {
    out_error = ProviderError::ERR_NOT_SUPPORTED;
    return false;
  }

  // Clamp then round to the device's real step grid. The device may still
  // snap this to something else internally; the post-set readback below,
  // not this calculation, is the truthful realized value.
  const float requested_ev = static_cast<float>(requested_milli_ev) / 1000.0f;
  const float clamped_ev = std::clamp(requested_ev, min_ev, max_ev);
  const float steps_from_min = std::round((clamped_ev - min_ev) / step_ev);
  const float target_ev = std::clamp(min_ev + steps_from_min * step_ev, min_ev, max_ev);

  struct SetResult {
    bool ok = false;
    float realized_ev = 0.0f;
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
  };
  auto result = std::make_shared<SetResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool completed = control_.run_bounded(
      [result, backend, target_ev](const BoundedControlExecutor::AbandonToken& t) {
        SetResult local;
        try {
          wmc::MediaCapture capture{nullptr};
          {
            std::lock_guard<std::mutex> bl(backend->m);
            capture = backend->capture;
          }
          if (!capture) {
            local.error = ProviderError::ERR_BAD_STATE;
          } else {
            wmd::ExposureCompensationControl ctrl =
                capture.VideoDeviceController().ExposureCompensationControl();
            auto op = ctrl.SetValueAsync(target_ev);
            if (!winrt_detail::wait_async_bounded(op, kExposureControlJobTimeoutMs - 500)) {
              local.error = ProviderError::ERR_TIMEOUT;
            } else {
              op.GetResults();
              // Truthful realized value: read back what the device actually
              // holds after the set, never the value we requested.
              local.realized_ev = ctrl.Value();
              local.ok = true;
            }
          }
        } catch (const winrt::hresult_error& e) {
          local.error = winrt_detail::provider_error_from_hresult(e.code());
        } catch (...) {
        }
        if (!t.abandoned.load(std::memory_order_acquire)) {
          *result = local;
        }
      },
      token, kExposureControlJobTimeoutMs);
  if (!completed) {
    out_error = ProviderError::ERR_TIMEOUT;
    return false;
  }
  if (!result->ok) {
    out_error = result->error;
    return false;
  }
  out_realized_milli_ev = static_cast<int32_t>(std::lround(result->realized_ev * 1000.0f));
  return true;
}

ProviderResult WinrtCameraProvider::ensure_low_lag_photo_realized_(
    const std::shared_ptr<DeviceBackend>& backend) noexcept {
  if (!backend) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed || backend->failed) {
      return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
    }
    if (backend->low_lag_photo) {
      return ProviderResult::success();
    }
  }

  struct PrepareResult {
    bool ok = false;
    ProviderError error = ProviderError::ERR_PROVIDER_FAILED;
    wmc::LowLagPhotoCapture prepared{nullptr};
  };
  auto result = std::make_shared<PrepareResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool completed = control_.run_bounded(
      [result, backend](const BoundedControlExecutor::AbandonToken& t) {
        PrepareResult local;
        try {
          wmc::MediaCapture capture{nullptr};
          {
            std::lock_guard<std::mutex> bl(backend->m);
            capture = backend->capture;
          }
          if (!capture) {
            local.error = ProviderError::ERR_BAD_STATE;
          } else {
            // Uncompressed Bgra8: the delivered CapturedFrame then carries a
            // SoftwareBitmap in exactly the format convert_software_bitmap
            // already consumes for stream frames, so stills need no second
            // conversion path.
            auto op = capture.PrepareLowLagPhotoCaptureAsync(
                wmm::ImageEncodingProperties::CreateUncompressed(wmm::MediaPixelFormat::Bgra8));
            if (!winrt_detail::wait_async_bounded(op, kControlJobTimeoutMs - 500)) {
              local.error = ProviderError::ERR_TIMEOUT;
            } else {
              local.prepared = op.GetResults();
              local.ok = static_cast<bool>(local.prepared);
              if (!local.ok) {
                local.error = ProviderError::ERR_NOT_SUPPORTED;
              }
            }
          }
        } catch (const winrt::hresult_error& e) {
          local.error = winrt_detail::provider_error_from_hresult(e.code());
          winrt_detail::log_line("PrepareLowLagPhotoCaptureAsync failed hr=0x%08X",
                                 static_cast<uint32_t>(e.code()));
        } catch (...) {
        }
        if (t.abandoned.load(std::memory_order_acquire)) {
          // Caller gave up; the prepared pipeline is orphaned, so finish it
          // here rather than leaking it into a backend nobody will close.
          if (local.ok && local.prepared) {
            try {
              auto finish_op = local.prepared.FinishAsync();
              (void)winrt_detail::wait_async_bounded(finish_op, kControlJobTimeoutMs - 500);
            } catch (...) {
            }
          }
          return;
        }
        *result = local;
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (!result->ok) {
    return ProviderResult::failure(result->error);
  }
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed || backend->failed) {
      return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
    }
    backend->low_lag_photo = result->prepared;
  }
  return ProviderResult::success();
}

WinrtCameraProvider::CapturedMemberFrame WinrtCameraProvider::capture_one_member_frame_(
    const std::shared_ptr<DeviceBackend>& backend,
    uint32_t width, uint32_t height, uint32_t format_fourcc) noexcept {
  CapturedMemberFrame result{};
  if (!backend) {
    return result;
  }
  const ProviderResult prepared = ensure_low_lag_photo_realized_(backend);
  if (!prepared.ok()) {
    result.error = prepared.code;
    return result;
  }

  auto captured = std::make_shared<CapturedMemberFrame>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool completed = control_.run_bounded(
      [captured, backend, width, height, format_fourcc](
          const BoundedControlExecutor::AbandonToken& t) {
        CapturedMemberFrame local{};
        try {
          wmc::LowLagPhotoCapture low_lag{nullptr};
          wmcf::MediaFrameSource source{nullptr};
          {
            std::lock_guard<std::mutex> bl(backend->m);
            if (backend->closed || backend->failed) {
              local.error = ProviderError::ERR_PROVIDER_FAILED;
              if (!t.abandoned.load(std::memory_order_acquire)) *captured = local;
              return;
            }
            low_lag = backend->low_lag_photo;
            source = backend->frame_source;
          }
          if (!low_lag) {
            local.error = ProviderError::ERR_BAD_STATE;
          } else {
            auto op = low_lag.CaptureAsync();
            if (!winrt_detail::wait_async_bounded(op, kCaptureSampleWaitMs)) {
              local.error = ProviderError::ERR_TIMEOUT;
            } else {
              // Provider-observed mark: the photo pipeline exposes no frame
              // timestamp of its own (neither ICapturedFrame nor
              // ICapturedFrame2 carries one), so this is taken as close to the
              // capture as the API allows and includes photo-processing
              // latency. Comparability is therefore reported no wider than the
              // single image -- see make_provider_observed_timing.
              local.sample_time_100ns = winrt_detail::provider_monotonic_100ns();
              local.has_sample_time = true;

              const wmc::CapturedPhoto photo = op.GetResults();
              const wmc::CapturedFrame frame = photo ? photo.Frame() : nullptr;
              const auto with_bitmap =
                  frame ? frame.try_as<wmc::ICapturedFrameWithSoftwareBitmap>() : nullptr;
              const wgi::SoftwareBitmap bitmap =
                  with_bitmap ? with_bitmap.SoftwareBitmap() : nullptr;
              if (!bitmap) {
                local.error = ProviderError::ERR_PROVIDER_FAILED;
              } else if (static_cast<uint32_t>(bitmap.PixelWidth()) != width ||
                         static_cast<uint32_t>(bitmap.PixelHeight()) != height) {
                // The photo follows the frame source's format, which the
                // caller has already set from the capture request. A mismatch
                // is a conversion failure, never a silent resize.
                winrt_detail::log_line(
                    "photo geometry %dx%d does not match requested %ux%u",
                    bitmap.PixelWidth(), bitmap.PixelHeight(), width, height);
                local.error = ProviderError::ERR_PLATFORM_CONSTRAINT;
              } else {
                auto bytes = std::make_shared<std::vector<uint8_t>>(
                    static_cast<size_t>(width) * height * 4u);
                if (winrt_detail::convert_software_bitmap(bitmap, width, height, format_fourcc,
                                                          bytes->data())) {
                  local.bytes = std::move(bytes);
                  local.ok = true;
                } else {
                  local.error = ProviderError::ERR_PROVIDER_FAILED;
                }
              }
              // Intrinsics stay per-image and stay sourced from the frame
              // source, which is where WinRT exposes them; the photo path has
              // none of its own. Legitimate here only because photo and stream
              // geometry are coupled on this backend -- the calibration is
              // format-anchored, and the format is the one just captured at.
              if (local.ok && source) {
                try {
                  const auto format = source.CurrentFormat();
                  const auto intrinsics =
                      format ? source.TryGetCameraIntrinsics(format) : nullptr;
                  if (intrinsics) {
                    local.has_intrinsics = true;
                    local.focal_length_x = intrinsics.FocalLength().x;
                    local.focal_length_y = intrinsics.FocalLength().y;
                    local.principal_point_x = intrinsics.PrincipalPoint().x;
                    local.principal_point_y = intrinsics.PrincipalPoint().y;
                    local.radial_k1 = intrinsics.RadialDistortion().x;
                    local.radial_k2 = intrinsics.RadialDistortion().y;
                    local.radial_k3 = intrinsics.RadialDistortion().z;
                    local.tangential_p1 = intrinsics.TangentialDistortion().x;
                    local.tangential_p2 = intrinsics.TangentialDistortion().y;
                    local.intrinsics_reference_width = intrinsics.ImageWidth();
                    local.intrinsics_reference_height = intrinsics.ImageHeight();
                  }
                } catch (...) {
                  local.has_intrinsics = false;
                }
              }
            }
          }
        } catch (const winrt::hresult_error& e) {
          local.error = winrt_detail::provider_error_from_hresult(e.code());
        } catch (...) {
        }
        if (!t.abandoned.load(std::memory_order_acquire)) {
          *captured = local;
        }
      },
      token, kCaptureSampleWaitMs + kControlJobTimeoutMs);
  if (!completed) {
    result.error = ProviderError::ERR_TIMEOUT;
    return result;
  }
  result = *captured;
  if (result.ok && !result.bytes) {
    result.ok = false;
    result.error = ProviderError::ERR_PROVIDER_FAILED;
  }
  return result;
}

void WinrtCameraProvider::run_device_capture_job_(const DeviceCaptureJob& job) noexcept {
  const uint64_t capture_id = job.request.capture_id;
  const uint64_t device_id = job.request.device_instance_id;
  bool terminal_posted = false;
  auto fail = [&](ProviderError error) {
    if (!terminal_posted) {
      terminal_posted = true;
      strand_.post_capture_failed(capture_id, device_id, error);
    }
  };

  try {
    // Every admitted device job emits capture_started and exactly one
    // terminal fact, shutdown-cancellation included.
    strand_.post_capture_started(capture_id, device_id);

    bool generation_closed = false;
    {
      std::lock_guard<std::mutex> lock(capture_mutex_);
      generation_closed = (job.generation != capture_generation_);
    }
    if (generation_closed) {
      fail(ProviderError::ERR_SHUTTING_DOWN);
      return;
    }

    // Fetch the backend under a short state lock only; realization, geometry
    // configuration, and reader start serialize on the backend's own
    // configure mutex so this worker never holds state_mutex_ across bounded
    // backend jobs.
    std::shared_ptr<DeviceBackend> backend;
    {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      auto dev_it = devices_.find(device_id);
      if (dev_it == devices_.end() || !dev_it->second.open) {
        fail(ProviderError::ERR_BAD_STATE);
        return;
      }
      backend = dev_it->second.backend;
    }
    if (!backend) {
      fail(ProviderError::ERR_BAD_STATE);
      return;
    }

    ProviderResult pr = ensure_reader_realized_(backend);
    if (!pr.ok()) {
      fail(pr.code);
      return;
    }
    pr = ensure_reader_geometry_(backend, job.request.width, job.request.height,
                                 job.request.format_fourcc);
    if (!pr.ok()) {
      fail(pr.code);
      return;
    }
    pr = ensure_reader_started_(backend);
    if (!pr.ok()) {
      fail(pr.code);
      return;
    }

    uint64_t session_id = 0;
    {
      std::lock_guard<std::mutex> bl(backend->m);
      if (backend->closed || backend->failed) {
        fail(ProviderError::ERR_PROVIDER_FAILED);
        return;
      }
      session_id = backend->acquisition_session_id;
    }

    const auto& members = job.request.still_image_bundle.members;
    const bool is_bracket_sequence = members.size() > 1;

    // The exposure-compensation control is device-global (shared with any
    // concurrently running stream, which will visibly show the bias while a
    // bracket runs -- a genuine platform constraint, not a bug: WinRT has no
    // per-capture-scoped exposure program independent of the live device
    // state). Save the pre-bracket value here and restore it once every
    // member has been captured, or as soon as the sequence fails.
    bool have_baseline_ev = false;
    float baseline_ev = 0.0f;
    if (is_bracket_sequence) {
      wmc::MediaCapture capture_for_baseline{nullptr};
      {
        std::lock_guard<std::mutex> bl(backend->m);
        capture_for_baseline = backend->capture;
      }
      if (capture_for_baseline) {
        try {
          wmd::ExposureCompensationControl ctrl =
              capture_for_baseline.VideoDeviceController().ExposureCompensationControl();
          if (ctrl && ctrl.Supported()) {
            baseline_ev = ctrl.Value();
            have_baseline_ev = true;
          }
        } catch (...) {
        }
      }
    }
    const auto restore_baseline_if_needed = [&]() {
      if (!is_bracket_sequence || !have_baseline_ev) {
        return;
      }
      int32_t restore_realized = 0;
      ProviderError restore_error = ProviderError::OK;
      const int32_t baseline_milli_ev = static_cast<int32_t>(std::lround(baseline_ev * 1000.0f));
      if (!apply_exposure_compensation_bounded_(backend, baseline_milli_ev, restore_realized,
                                                restore_error)) {
        // A restore failure does not un-capture already-truthful member
        // frames, so it must not fail the capture -- but it does leave the
        // live device exposure biased, which is worth surfacing.
        winrt_detail::log_line(
            "bracket exposure-compensation restore failed for device=%llu (error=%u)",
            static_cast<unsigned long long>(device_id),
            static_cast<unsigned>(restore_error));
      }
    };

    for (const CaptureStillImageMember& member : members) {
      int32_t realized_milli_ev = 0;
      bool has_realized_ev = false;

      if (member.role == CaptureStillImageMemberRole::ADDITIONAL_BRACKET) {
        ProviderError ev_error = ProviderError::ERR_PROVIDER_FAILED;
        if (!apply_exposure_compensation_bounded_(
                backend, member.intended_exposure_compensation_milli_ev,
                realized_milli_ev, ev_error)) {
          fail(ev_error);
          restore_baseline_if_needed();
          return;
        }
        has_realized_ev = true;
      } else if (have_baseline_ev) {
        // DEFAULT_METERED member: no compensation is requested, but the
        // baseline read above already tells us the (unmodified) control
        // value truthfully, so report it instead of leaving it unset.
        realized_milli_ev = static_cast<int32_t>(std::lround(baseline_ev * 1000.0f));
        has_realized_ev = true;
      }

      const CapturedMemberFrame captured = capture_one_member_frame_(
          backend, job.request.width, job.request.height, job.request.format_fourcc);
      if (!captured.ok || !captured.bytes) {
        fail(captured.error);
        restore_baseline_if_needed();
        return;
      }

      FrameView fv{};
      fv.device_instance_id = device_id;
      fv.stream_id = 0;
      fv.acquisition_session_id = session_id;
      fv.capture_id = capture_id;
      fv.capture_image.routing =
          member.role == CaptureStillImageMemberRole::ADDITIONAL_BRACKET
              ? CaptureImageRouting::ADDITIONAL_BRACKET
              : CaptureImageRouting::DEFAULT_METERED;
      fv.capture_image.image_member_index = member.image_member_index;
      fv.capture_image.applied_exposure_compensation_milli_ev =
          member.intended_exposure_compensation_milli_ev;
      fv.capture_image.has_realized_exposure_compensation_milli_ev = has_realized_ev;
      fv.capture_image.realized_exposure_compensation_milli_ev = realized_milli_ev;
      fv.width = job.request.width;
      fv.height = job.request.height;
      fv.format_fourcc = job.request.format_fourcc;
      if (captured.has_sample_time) {
        fv.acquisition_timing =
            winrt_detail::make_provider_observed_timing(captured.sample_time_100ns);
      }
      fv.data = captured.bytes->data();
      fv.size_bytes = captured.bytes->size();
      fv.stride_bytes = job.request.width * 4u;
      // Fresh immutable allocation: publish for zero-copy retention (brief §4).
      fv.cpu_payload_owner = captured.bytes;
      fv.requested_retained_plan = job.request.requested_retained_plan;
      fv.release = &winrt_detail::release_capture_frame;
      fv.release_user = new winrt_detail::CaptureFrameLease{captured.bytes};
      strand_.post_frame(fv);

      // Real per-image camera facts, only when WinRT genuinely reported them
      // for this exact frame (most UVC webcams report none of this; some
      // depth/professional cameras report intrinsics/distortion, and some
      // report a focus preset). This is a supply-side addition only: Core's
      // existing precedence (external ADC ingestion > provider per-image >
      // provider static) and provenance recording are untouched, so an
      // ingested camera description still overrides this truthfully-
      // reported value exactly as before.
      {
        ProviderCaptureImageFacts image_facts{};
        if (captured.has_intrinsics) {
          const CoordinateDomain domain{CoordinateDomainDeliveredImage{}};
          if (const auto intrinsics_fact = Intrinsics::create(
                  static_cast<double>(captured.focal_length_x),
                  static_cast<double>(captured.focal_length_y),
                  static_cast<double>(captured.principal_point_x),
                  static_cast<double>(captured.principal_point_y),
                  std::nullopt, // WinRT CameraIntrinsics does not report skew
                  captured.intrinsics_reference_width,
                  captured.intrinsics_reference_height,
                  domain)) {
            image_facts.intrinsics =
                SourcedFact<Intrinsics>{*intrinsics_fact, FactOrigin::NATIVE_REPORTED};
          }
          // WinRT delivers the raw sensor frame; these coefficients describe
          // (but have not been used to correct) that raw distortion.
          if (const auto distortion_fact = BrownConrady5Distortion::create(
                  static_cast<double>(captured.radial_k1),
                  static_cast<double>(captured.radial_k2),
                  static_cast<double>(captured.radial_k3),
                  static_cast<double>(captured.tangential_p1),
                  static_cast<double>(captured.tangential_p2),
                  captured.intrinsics_reference_width,
                  captured.intrinsics_reference_height,
                  domain,
                  DistortionImageState::DISTORTED)) {
            image_facts.distortion =
                SourcedFact<Distortion>{Distortion{*distortion_fact}, FactOrigin::NATIVE_REPORTED};
          }
        }
        if (query_focus_state_at_infinity_(backend)) {
          image_facts.focus_state =
              SourcedFact<FocusState>{FocusAtInfinity{}, FactOrigin::NATIVE_REPORTED};
        }
        if (image_facts.intrinsics || image_facts.distortion || image_facts.focus_state) {
          strand_.post_capture_image_facts(capture_id, device_id, member.image_member_index,
                                           image_facts);
        }
      }
    }

    restore_baseline_if_needed();

    terminal_posted = true;
    strand_.post_capture_completed(capture_id, device_id);
  } catch (...) {
    fail(ProviderError::ERR_PROVIDER_FAILED);
  }
}

ProviderResult WinrtCameraProvider::validate_and_admit_submission_locked_(
    const CaptureSubmission& submission,
    std::vector<DeviceCaptureJob>& out_jobs) {
  // Caller holds capture_mutex_ then state_mutex_.
  if (capture_admission_closed_) {
    return ProviderResult::failure(ProviderError::ERR_SHUTTING_DOWN);
  }
  if (submission.capture_id == 0 || submission.device_requests.empty()) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (capture_queue_.size() + capture_active_jobs_ + submission.device_requests.size() >
      kCaptureQueueCapacity) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }

  out_jobs.clear();
  out_jobs.reserve(submission.device_requests.size());
  for (const CaptureRequest& req : submission.device_requests) {
    if (req.capture_id != submission.capture_id || req.device_instance_id == 0) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    if (req.width == 0 || req.height == 0) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    if (req.format_fourcc != FOURCC_RGBA && req.format_fourcc != FOURCC_BGRA) {
      return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
    }
    if (!is_valid_capture_still_image_bundle(req.still_image_bundle,
                                             supports_multi_image_still_sequence())) {
      return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
    }
    if (in_flight_captures_.count(
            InFlightKey{req.capture_id, req.device_instance_id}) != 0) {
      return ProviderResult::failure(ProviderError::ERR_BUSY);
    }
    auto dev_it = devices_.find(req.device_instance_id);
    if (dev_it == devices_.end() || !dev_it->second.open) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    // A started stream pins the shared source geometry; a capture that needs
    // different geometry cannot execute without disrupting the stream.
    if (dev_it->second.backend) {
      std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
      const auto& stream = dev_it->second.backend->stream;
      if (stream && stream->producing &&
          (stream->width != req.width || stream->height != req.height)) {
        return ProviderResult::failure(ProviderError::ERR_PLATFORM_CONSTRAINT);
      }
    }
    // Provider-wide multi-image support is advertised optimistically
    // (exposure-compensation control is a very common UVC capability), but
    // the exact opened device may still lack it, or the caller may request
    // more members than this provider's derived watchdog budget covers
    // (kMaxBracketMembers) -- both fail deterministically here rather than
    // being silently degraded to a single default image.
    if (req.still_image_bundle.members.size() > 1) {
      if (req.still_image_bundle.members.size() > kMaxBracketMembers) {
        return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
      }
      float unused_min = 0.0f, unused_max = 0.0f, unused_step = 0.0f;
      if (!query_exposure_compensation_range_(dev_it->second.backend, unused_min, unused_max,
                                              unused_step)) {
        return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
      }
    }
    DeviceCaptureJob job;
    job.request = req;
    job.generation = capture_generation_;
    out_jobs.push_back(std::move(job));
  }
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::trigger_capture(const CaptureRequest& req) {
  CaptureSubmission submission;
  submission.capture_id = req.capture_id;
  submission.origin = req.rig_id != 0 ? CaptureSubmissionOrigin::RIG_CAPTURE
                                      : CaptureSubmissionOrigin::DEVICE_CAPTURE;
  submission.rig_id = req.rig_id;
  submission.device_requests.push_back(req);
  return trigger_capture_submission(submission);
}

ProviderResult WinrtCameraProvider::trigger_capture_submission(
    const CaptureSubmission& submission) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  std::vector<DeviceCaptureJob> jobs;
  {
    // Atomic grouped admission: every member device job or none, and a
    // rejected submission emits no facts (brief §5).
    std::lock_guard<std::mutex> capture_lock(capture_mutex_);
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    const ProviderResult pr = validate_and_admit_submission_locked_(submission, jobs);
    if (!pr.ok()) {
      return pr;
    }
    for (DeviceCaptureJob& job : jobs) {
      in_flight_captures_[InFlightKey{job.request.capture_id,
                                      job.request.device_instance_id}] =
          job.generation;
      capture_queue_.push_back(std::move(job));
    }
  }
  capture_cv_.notify_all();
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::abort_capture(uint64_t /*capture_id*/) {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  // Deterministic: admitted single-frame reads cannot be recalled mid-flight.
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult WinrtCameraProvider::apply_camera_spec_patch(
    const std::string& hardware_id,
    uint64_t /*new_camera_spec_version*/,
    SpecPatchView /*patch*/) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (hardware_id.empty()) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::apply_imaging_spec_patch(
    uint64_t /*new_imaging_spec_version*/,
    SpecPatchView /*patch*/) {
  if (!initialized_.load(std::memory_order_acquire) ||
      shutting_down_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::shutdown() {
  if (!initialized_.load(std::memory_order_acquire)) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (shutting_down_.exchange(true, std::memory_order_acq_rel)) {
    return ProviderResult::success();
  }

  // 1. Close capture admission and invalidate the generation so queued jobs
  //    terminalize as ERR_SHUTTING_DOWN at their next checkpoint.
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    capture_admission_closed_ = true;
    ++capture_generation_;
  }
  capture_cv_.notify_all();

  // 2. Cancel in-flight sample waits so workers do not ride out full sample
  //    timeouts, then join the workers (each admitted job terminalizes
  //    exactly once inside the worker).
  {
    std::lock_guard<std::mutex> state_lock(state_mutex_);
    for (auto& [dev_id, dev] : devices_) {
      (void)dev_id;
      if (dev.backend) {
        std::lock_guard<std::mutex> bl(dev.backend->m);
        dev.backend->fail_all_waiters_locked(ProviderError::ERR_SHUTTING_DOWN);
      }
    }
  }
  stop_and_join_capture_executor_();

  {
  std::lock_guard<std::mutex> state_lock(state_mutex_);

  // 3. Stop started streams.
  for (auto& [stream_id, st] : streams_) {
    if (!st.started) {
      continue;
    }
    auto dev_it = devices_.find(st.req.device_instance_id);
    if (dev_it != devices_.end() && dev_it->second.backend) {
      std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
      if (dev_it->second.backend->stream) {
        dev_it->second.backend->stream->producing = false;
      }
    }
    st.started = false;
    strand_.post_stream_stopped(stream_id, ProviderError::OK);
  }

  // Quiescence barrier while stream records are still alive.
  strand_.flush();

  // 4. Destroy streams.
  for (auto it = streams_.begin(); it != streams_.end();) {
    const uint64_t stream_id = it->first;
    const uint64_t native_id = it->second.native_id;
    auto dev_it = devices_.find(it->second.req.device_instance_id);
    if (dev_it != devices_.end()) {
      if (dev_it->second.backend) {
        std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
        dev_it->second.backend->stream.reset();
      }
      dev_it->second.stream_id = 0;
    }
    it = streams_.erase(it);
    strand_.post_stream_destroyed(stream_id);
    emit_native_destroyed_(native_id);
  }

  // 5. Close devices with real WinRT release on the control thread.
  for (auto& [dev_id, dev] : devices_) {
    if (!dev.open) {
      continue;
    }
    std::shared_ptr<DeviceBackend> backend = dev.backend;
    uint64_t session_native_id = 0;
    if (backend) {
      {
        std::lock_guard<std::mutex> bl(backend->m);
        backend->closed = true;
        session_native_id = backend->acquisition_session_id;
        backend->fail_all_waiters_locked(ProviderError::ERR_SHUTTING_DOWN);
      }
      auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
      (void)control_.run_bounded(
          [backend](const BoundedControlExecutor::AbandonToken& /*t*/) {
            wmcf::MediaFrameReader reader{nullptr};
            wmc::MediaCapture capture{nullptr};
            wmc::LowLagPhotoCapture low_lag_photo{nullptr};
            winrt::event_token frame_token{};
            winrt::event_token failed_token{};
            {
              std::lock_guard<std::mutex> bl(backend->m);
              reader = std::exchange(backend->reader, nullptr);
              capture = std::exchange(backend->capture, nullptr);
              low_lag_photo = std::exchange(backend->low_lag_photo, nullptr);
              backend->frame_source = nullptr;
              frame_token = std::exchange(backend->frame_token, {});
              failed_token = std::exchange(backend->failed_token, {});
              backend->reader_started = false;
            }
            try {
              if (reader) {
                if (frame_token.value != 0) {
                  reader.FrameArrived(frame_token);
                }
                auto stop_op = reader.StopAsync();
                (void)winrt_detail::wait_async_bounded(stop_op,
                                                       kControlJobTimeoutMs - 500);
                reader.Close();
              }
              if (low_lag_photo) {
                auto finish_op = low_lag_photo.FinishAsync();
                (void)winrt_detail::wait_async_bounded(finish_op,
                                                       kControlJobTimeoutMs - 500);
              }
              if (capture) {
                if (failed_token.value != 0) {
                  capture.Failed(failed_token);
                }
                capture.Close();
              }
            } catch (...) {
            }
          },
          token, kControlJobTimeoutMs);
    }
    dev.open = false;
    dev.backend.reset();
    if (session_native_id != 0) {
      emit_native_destroyed_(session_native_id);
    }
    strand_.post_device_closed(dev_id);
    emit_native_destroyed_(dev.native_id);
    dev.native_id = 0;
  }

  emit_native_destroyed_(provider_native_id_);
  provider_native_id_ = 0;
  } // release state_mutex_ before draining the strand (brief §10)

  // 6. With provider state settled and no locks held: flush and stop the
  //    strand, then stop the control thread.
  strand_.flush();
  strand_.stop();
  control_.stop();

  callbacks_ = nullptr;
  initialized_.store(false, std::memory_order_release);
  return ProviderResult::success();
}

} // namespace cambang
