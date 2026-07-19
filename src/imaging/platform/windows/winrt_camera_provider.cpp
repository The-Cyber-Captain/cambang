// CamBANG windows_winrt platform provider implementation.
// See winrt_camera_provider.h for the architectural overview and
// docs/provider_implementation_brief.md for the contract this adapts to.

#include "imaging/platform/windows/winrt_camera_provider.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace cambang {
namespace winrt_detail {

using Microsoft::WRL::ComPtr;

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

ProviderError provider_error_from_hresult(HRESULT hr) noexcept {
  switch (hr) {
    case E_ACCESSDENIED:
      return ProviderError::ERR_PLATFORM_CONSTRAINT;
    case MF_E_INVALIDMEDIATYPE:
    case MF_E_UNSUPPORTED_RATE:
    case MF_E_TOPO_CODEC_NOT_FOUND:
      return ProviderError::ERR_PLATFORM_CONSTRAINT;
    case MF_E_HW_MFT_FAILED_START_STREAMING:
    case MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED:
      return ProviderError::ERR_TRANSIENT_FAILURE;
    default:
      return ProviderError::ERR_PROVIDER_FAILED;
  }
}

std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::string wide_to_utf8(const wchar_t* w, size_t len) {
  if (!w || len == 0) return std::string();
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(len), s.data(), n, nullptr, nullptr);
  return s;
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
  const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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
  if (SUCCEEDED(co_hr)) {
    CoUninitialize();
  }
}

// ---------------------------------------------------------------------------
// Frame conversion
// ---------------------------------------------------------------------------

namespace {

// Converts a locked BGRX row image (MFVideoFormat_RGB32) into the destination
// packed format. dst must hold width*height*4 bytes.
void convert_bgrx_rows(const uint8_t* scanline0,
                       LONG pitch,
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
    } else { // BGRA: same channel order, force opaque alpha over the X byte.
      std::memcpy(out, src, row_bytes);
      for (uint32_t x = 0; x < width; ++x) {
        out[4 * x + 3] = 0xFF;
      }
    }
  }
}

bool convert_sample(IMFSample* sample,
                    uint32_t width,
                    uint32_t height,
                    uint32_t dst_fourcc,
                    uint8_t* dst) {
  if (!sample) return false;
  ComPtr<IMFMediaBuffer> buffer;
  if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) {
    return false;
  }

  ComPtr<IMF2DBuffer> buffer2d;
  if (SUCCEEDED(buffer.As(&buffer2d)) && buffer2d) {
    BYTE* scanline0 = nullptr;
    LONG pitch = 0;
    if (SUCCEEDED(buffer2d->Lock2D(&scanline0, &pitch)) && scanline0) {
      convert_bgrx_rows(scanline0, pitch, width, height, dst_fourcc, dst);
      buffer2d->Unlock2D();
      return true;
    }
  }

  BYTE* data = nullptr;
  DWORD max_len = 0;
  DWORD cur_len = 0;
  if (FAILED(buffer->Lock(&data, &max_len, &cur_len)) || !data) {
    return false;
  }
  const size_t needed = static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  bool ok = false;
  if (cur_len >= needed) {
    convert_bgrx_rows(data, static_cast<LONG>(width) * 4, width, height, dst_fourcc, dst);
    ok = true;
  }
  buffer->Unlock();
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
};

class DeviceReaderCallback;

struct DeviceBackend {
  // Serializes reader realization/geometry configuration across the core
  // thread and capture workers without either holding provider state_mutex_.
  // Ordering: configure_mutex before m; state_mutex_ is never taken inside.
  std::mutex configure_mutex;

  std::mutex m;
  ComPtr<IMFMediaSource> source;
  ComPtr<IMFSourceReader> reader;
  ComPtr<DeviceReaderCallback> callback;

  uint64_t device_instance_id = 0;
  uint64_t root_id = 0;
  uint64_t acquisition_session_id = 0; // core-issued native id once realized
  CBProviderStrand* strand = nullptr;  // provider outlives all backends

  bool closed = false;        // set before MF objects are released
  bool pump_active = false;   // one outstanding async ReadSample
  bool reader_failed = false;
  uint32_t configured_w = 0;
  uint32_t configured_h = 0;

  std::shared_ptr<StreamProduction> stream;
  std::deque<std::shared_ptr<CaptureWaiter>> waiters;

  // Caller holds m. Issues the next async ReadSample when demand exists.
  void ensure_pump_locked() {
    if (closed || reader_failed || pump_active || !reader) {
      return;
    }
    const bool need = (stream && stream->producing) || !waiters.empty();
    if (!need) {
      return;
    }
    const HRESULT hr = reader->ReadSample(
        static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),
        0, nullptr, nullptr, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
      pump_active = true;
    } else {
      const ProviderError err = provider_error_from_hresult(hr);
      reader_failed = true;
      fail_all_waiters_locked(err);
      if (strand) {
        strand->post_device_error(device_instance_id, err);
        if (stream && stream->producing) {
          stream->producing = false;
          strand->post_stream_error(stream->stream_id, err);
          strand->post_stream_stopped(stream->stream_id, err);
        }
      }
    }
  }

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

std::optional<SourcedFact<ImageAcquisitionTiming>> make_acquisition_timing(
    int64_t sample_time_100ns) {
  if (sample_time_100ns < 0) {
    return std::nullopt;
  }
  const auto tick_period = TickPeriod::create(100, 1); // MF marks tick at 100ns
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

} // namespace

// ---------------------------------------------------------------------------
// IMFSourceReaderCallback
// ---------------------------------------------------------------------------

class DeviceReaderCallback final : public IMFSourceReaderCallback {
public:
  explicit DeviceReaderCallback(std::weak_ptr<DeviceBackend> backend)
      : backend_(std::move(backend)) {}

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFSourceReaderCallback)) {
      *ppv = static_cast<IMFSourceReaderCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(refs_.fetch_add(1, std::memory_order_acq_rel) + 1);
  }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG left = static_cast<ULONG>(refs_.fetch_sub(1, std::memory_order_acq_rel) - 1);
    if (left == 0) {
      delete this;
    }
    return left;
  }

  // IMFSourceReaderCallback
  STDMETHODIMP OnReadSample(HRESULT hrStatus,
                            DWORD /*dwStreamIndex*/,
                            DWORD dwStreamFlags,
                            LONGLONG llTimestamp,
                            IMFSample* pSample) override {
    std::shared_ptr<DeviceBackend> backend = backend_.lock();
    if (!backend) {
      return S_OK;
    }
    std::lock_guard<std::mutex> lock(backend->m);
    backend->pump_active = false;
    if (backend->closed) {
      return S_OK;
    }

    if (FAILED(hrStatus) || (dwStreamFlags & MF_SOURCE_READERF_ERROR) != 0) {
      const ProviderError err = FAILED(hrStatus)
                                    ? provider_error_from_hresult(hrStatus)
                                    : ProviderError::ERR_PROVIDER_FAILED;
      backend->reader_failed = true;
      backend->fail_all_waiters_locked(err);
      if (backend->strand) {
        backend->strand->post_device_error(backend->device_instance_id, err);
        if (backend->stream && backend->stream->producing) {
          backend->stream->producing = false;
          backend->strand->post_stream_error(backend->stream->stream_id, err);
          backend->strand->post_stream_stopped(backend->stream->stream_id, err);
        }
      }
      return S_OK;
    }

    if (pSample) {
      deliver_sample_locked_(*backend, pSample, llTimestamp);
    }
    backend->ensure_pump_locked();
    return S_OK;
  }

  STDMETHODIMP OnFlush(DWORD /*dwStreamIndex*/) override { return S_OK; }
  STDMETHODIMP OnEvent(DWORD /*dwStreamIndex*/, IMFMediaEvent* /*pEvent*/) override {
    return S_OK;
  }

private:
  ~DeviceReaderCallback() = default;

  static void deliver_sample_locked_(DeviceBackend& backend,
                                     IMFSample* sample,
                                     LONGLONG timestamp_100ns) {
    // Still-capture waiters take priority: they are non-lossy capture truth.
    if (!backend.waiters.empty()) {
      std::shared_ptr<CaptureWaiter> waiter = backend.waiters.front();
      backend.waiters.pop_front();
      auto bytes = std::make_shared<std::vector<uint8_t>>();
      bytes->resize(static_cast<size_t>(waiter->width) *
                    static_cast<size_t>(waiter->height) * 4u);
      const bool ok = (waiter->width == backend.configured_w &&
                       waiter->height == backend.configured_h) &&
                      convert_sample(sample, waiter->width, waiter->height,
                                     waiter->fourcc, bytes->data());
      std::lock_guard<std::mutex> wl(waiter->m);
      if (!waiter->done) {
        waiter->done = true;
        waiter->ok = ok;
        waiter->error = ok ? ProviderError::OK : ProviderError::ERR_PROVIDER_FAILED;
        if (ok) {
          waiter->bytes = std::move(bytes);
          waiter->has_sample_time = true;
          waiter->sample_time_100ns = static_cast<int64_t>(timestamp_100ns);
        }
        waiter->cv.notify_all();
      }
    }

    StreamProduction* s = backend.stream.get();
    if (!s || !s->producing || !backend.strand) {
      return;
    }
    if (s->width != backend.configured_w || s->height != backend.configured_h) {
      return; // stale geometry; drop (repeating frames are lossy)
    }

    // Acquire a pool slot without per-frame allocation; exhaustion drops the
    // frame (lossy class).
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
      return;
    }

    if (!convert_sample(sample, s->width, s->height, s->fourcc, slot->bytes.data())) {
      slot->in_use.store(false, std::memory_order_release);
      return;
    }

    FrameView fv{};
    fv.device_instance_id = s->device_instance_id;
    fv.stream_id = s->stream_id;
    fv.acquisition_session_id = s->acquisition_session_id;
    fv.capture_id = 0;
    fv.width = s->width;
    fv.height = s->height;
    fv.format_fourcc = s->fourcc;
    fv.acquisition_timing =
        make_acquisition_timing(static_cast<int64_t>(timestamp_100ns));
    fv.data = slot->bytes.data();
    fv.size_bytes = slot->bytes.size();
    fv.stride_bytes = s->width * 4u;
    fv.requested_retained_plan = s->plan;
    fv.release = &release_stream_frame;
    fv.release_user = new StreamFrameLease{slot};
    backend.strand->post_frame(fv);
  }

  std::atomic<int64_t> refs_{1};
  std::weak_ptr<DeviceBackend> backend_;
};

} // namespace winrt_detail

// ---------------------------------------------------------------------------
// WinrtCameraProvider
// ---------------------------------------------------------------------------

using winrt_detail::BoundedControlExecutor;
using winrt_detail::CaptureWaiter;
using winrt_detail::ComPtr;
using winrt_detail::DeviceBackend;
using winrt_detail::DeviceReaderCallback;
using winrt_detail::StreamProduction;

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

  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  auto hr_out = std::make_shared<HRESULT>(E_FAIL);
  const bool completed = control_.run_bounded(
      [hr_out](const BoundedControlExecutor::AbandonToken& t) {
        const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (t.abandoned.load(std::memory_order_acquire)) {
          if (SUCCEEDED(hr)) {
            MFShutdown();
          }
          return;
        }
        *hr_out = hr;
      },
      token, kControlJobTimeoutMs);
  if (!completed || FAILED(*hr_out)) {
    control_.stop();
    return ProviderResult::failure(
        completed ? winrt_detail::provider_error_from_hresult(*hr_out)
                  : ProviderError::ERR_TIMEOUT);
  }
  mf_started_ = true;

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
    HRESULT hr = E_FAIL;
  };
  auto result = std::make_shared<EnumResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool completed = control_.run_bounded(
      [result](const BoundedControlExecutor::AbandonToken& t) {
        EnumResult local;
        ComPtr<IMFAttributes> attrs;
        HRESULT hr = MFCreateAttributes(&attrs, 1);
        if (SUCCEEDED(hr)) {
          hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        }
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        if (SUCCEEDED(hr)) {
          hr = MFEnumDeviceSources(attrs.Get(), &activates, &count);
        }
        if (SUCCEEDED(hr)) {
          for (UINT32 i = 0; i < count; ++i) {
            wchar_t* link = nullptr;
            UINT32 link_len = 0;
            wchar_t* name = nullptr;
            UINT32 name_len = 0;
            if (SUCCEEDED(activates[i]->GetAllocatedString(
                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                    &link, &link_len)) && link) {
              CameraEndpoint ep;
              ep.hardware_id = winrt_detail::wide_to_utf8(link, link_len);
              if (SUCCEEDED(activates[i]->GetAllocatedString(
                      MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &name_len)) &&
                  name) {
                ep.name = winrt_detail::wide_to_utf8(name, name_len);
                CoTaskMemFree(name);
              }
              CoTaskMemFree(link);
              if (!ep.hardware_id.empty()) {
                local.endpoints.push_back(std::move(ep));
              }
            }
            activates[i]->Release();
          }
          CoTaskMemFree(activates);
        }
        local.hr = hr;
        if (!t.abandoned.load(std::memory_order_acquire)) {
          *result = std::move(local);
        }
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (FAILED(result->hr)) {
    return ProviderResult::failure(
        winrt_detail::provider_error_from_hresult(result->hr));
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

  struct OpenResult {
    ComPtr<IMFMediaSource> source;
    HRESULT hr = E_FAIL;
  };
  auto result = std::make_shared<OpenResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const std::wstring wide_link = winrt_detail::utf8_to_wide(hardware_id);
  const bool completed = control_.run_bounded(
      [result, wide_link](const BoundedControlExecutor::AbandonToken& t) {
        ComPtr<IMFMediaSource> source;
        ComPtr<IMFAttributes> attrs;
        HRESULT hr = MFCreateAttributes(&attrs, 2);
        if (SUCCEEDED(hr)) {
          hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        }
        if (SUCCEEDED(hr)) {
          hr = attrs->SetString(
              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
              wide_link.c_str());
        }
        if (SUCCEEDED(hr)) {
          hr = MFCreateDeviceSource(attrs.Get(), &source);
        }
        if (t.abandoned.load(std::memory_order_acquire)) {
          // Caller gave up: the acquisition is orphaned; release it here.
          if (source) {
            source->Shutdown();
          }
          return;
        }
        result->hr = hr;
        result->source = std::move(source);
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (FAILED(result->hr) || !result->source) {
    return ProviderResult::failure(
        winrt_detail::provider_error_from_hresult(result->hr));
  }

  dev.hardware_id = hardware_id;
  dev.device_instance_id = device_instance_id;
  dev.root_id = root_id;
  dev.open = true;
  dev.stream_id = 0;
  dev.native_id = alloc_native_id_(NativeObjectType::Device);
  dev.backend = std::make_shared<DeviceBackend>();
  dev.backend->device_instance_id = device_instance_id;
  dev.backend->root_id = root_id;
  dev.backend->strand = &strand_;
  dev.backend->source = std::move(result->source);

  emit_native_created_(dev.native_id, NativeObjectType::Device, root_id,
                       device_instance_id, 0, 0);
  strand_.post_device_opened(device_instance_id);
  return ProviderResult::success();
}

ProviderResult WinrtCameraProvider::ensure_reader_realized_(
    const std::shared_ptr<DeviceBackend>& backend) {
  if (!backend) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  std::lock_guard<std::mutex> configure_lock(backend->configure_mutex);
  ComPtr<IMFMediaSource> source;
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    if (backend->reader) {
      return backend->reader_failed
                 ? ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED)
                 : ProviderResult::success();
    }
    source = backend->source;
  }
  if (!source) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  // COM callback objects start at refcount 1; adopt without AddRef.
  ComPtr<DeviceReaderCallback> callback;
  callback.Attach(new DeviceReaderCallback(backend));

  struct RealizeResult {
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = E_FAIL;
  };
  auto result = std::make_shared<RealizeResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool completed = control_.run_bounded(
      [result, source, callback](const BoundedControlExecutor::AbandonToken& t) {
        ComPtr<IMFSourceReader> reader;
        ComPtr<IMFAttributes> attrs;
        HRESULT hr = MFCreateAttributes(&attrs, 2);
        if (SUCCEEDED(hr)) {
          hr = attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
        }
        if (SUCCEEDED(hr)) {
          // Advanced video processing: format conversion to RGB32 plus
          // scaling, so effective-config geometry can be honored exactly.
          hr = attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        }
        if (SUCCEEDED(hr)) {
          hr = MFCreateSourceReaderFromMediaSource(source.Get(), attrs.Get(), &reader);
        }
        if (t.abandoned.load(std::memory_order_acquire)) {
          return; // reader ComPtr releases here; source stays owned by backend
        }
        result->hr = hr;
        result->reader = std::move(reader);
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    winrt_detail::log_line("reader realization timed out (device=%llu)",
                           static_cast<unsigned long long>(backend->device_instance_id));
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (FAILED(result->hr) || !result->reader) {
    winrt_detail::log_line("reader realization failed hr=0x%08lX (device=%llu)",
                           static_cast<unsigned long>(result->hr),
                           static_cast<unsigned long long>(backend->device_instance_id));
    return ProviderResult::failure(
        winrt_detail::provider_error_from_hresult(result->hr));
  }

  // AcquisitionSession native truth: the concretely realized source reader.
  const uint64_t session_id = alloc_native_id_(NativeObjectType::AcquisitionSession);
  uint64_t root_id = 0;
  uint64_t device_instance_id = 0;
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed) {
      // Raced with device close: release the freshly created reader.
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    backend->reader = std::move(result->reader);
    backend->callback = callback;
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

  ComPtr<IMFSourceReader> reader;
  {
    std::lock_guard<std::mutex> bl(backend->m);
    if (backend->closed || !backend->reader || backend->reader_failed) {
      return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
    }
    if (backend->configured_w == width && backend->configured_h == height) {
      return ProviderResult::success();
    }
    // A started stream pins the negotiated geometry: reconfiguring the shared
    // reader mid-stream would disrupt live delivery. Deterministic failure.
    if (backend->stream && backend->stream->producing) {
      return ProviderResult::failure(ProviderError::ERR_PLATFORM_CONSTRAINT);
    }
    reader = backend->reader;
  }

  struct ConfigureResult {
    HRESULT hr = E_FAIL;
    uint32_t actual_w = 0;
    uint32_t actual_h = 0;
  };
  auto result = std::make_shared<ConfigureResult>();
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  const bool completed = control_.run_bounded(
      [result, reader, width, height](const BoundedControlExecutor::AbandonToken& t) {
        ConfigureResult local;
        ComPtr<IMFMediaType> mt;
        HRESULT hr = MFCreateMediaType(&mt);
        if (SUCCEEDED(hr)) hr = mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr)) {
          hr = MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, width, height);
        }
        if (SUCCEEDED(hr)) {
          hr = reader->SetStreamSelection(
              static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        }
        if (SUCCEEDED(hr)) {
          hr = reader->SetCurrentMediaType(
              static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
              mt.Get());
        }
        if (SUCCEEDED(hr)) {
          ComPtr<IMFMediaType> actual;
          hr = reader->GetCurrentMediaType(
              static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &actual);
          if (SUCCEEDED(hr)) {
            UINT32 aw = 0;
            UINT32 ah = 0;
            if (SUCCEEDED(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &aw, &ah))) {
              local.actual_w = aw;
              local.actual_h = ah;
            }
          }
        }
        local.hr = hr;
        if (!t.abandoned.load(std::memory_order_acquire)) {
          *result = local;
        }
      },
      token, kControlJobTimeoutMs);
  if (!completed) {
    winrt_detail::log_line("media-type configure timed out (%ux%u)", width, height);
    return ProviderResult::failure(ProviderError::ERR_TIMEOUT);
  }
  if (FAILED(result->hr)) {
    winrt_detail::log_line("media-type configure failed hr=0x%08lX (%ux%u)",
                           static_cast<unsigned long>(result->hr), width, height);
    return ProviderResult::failure(
        winrt_detail::provider_error_from_hresult(result->hr));
  }
  if (result->actual_w != width || result->actual_h != height) {
    // The backend negotiated something other than the effective configuration
    // Core supplied; never deliver silently-substituted geometry (brief §6).
    winrt_detail::log_line("media-type negotiated %ux%u instead of requested %ux%u",
                           result->actual_w, result->actual_h, width, height);
    return ProviderResult::failure(ProviderError::ERR_PLATFORM_CONSTRAINT);
  }

  std::lock_guard<std::mutex> bl(backend->m);
  backend->configured_w = width;
  backend->configured_h = height;
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

  // Real release of MF objects on the control thread. IMFSourceReader's final
  // release blocks until outstanding callbacks complete, so after this job no
  // further OnReadSample can observe this backend.
  auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
  (void)control_.run_bounded(
      [backend](const BoundedControlExecutor::AbandonToken& /*t*/) {
        if (!backend) return;
        ComPtr<IMFSourceReader> reader;
        ComPtr<IMFMediaSource> source;
        {
          std::lock_guard<std::mutex> bl(backend->m);
          reader = std::move(backend->reader);
          source = std::move(backend->source);
          backend->callback.Reset();
        }
        reader.Reset();
        if (source) {
          source->Shutdown();
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
    if (dev.backend->reader_failed || dev.backend->closed) {
      return ProviderResult::failure(ProviderError::ERR_PROVIDER_FAILED);
    }
    production->producing = true;
    dev.backend->stream = production;
    dev.backend->ensure_pump_locked();
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

    // Fetch the backend under a short state lock only; realization and
    // geometry configuration serialize on the backend's own configure mutex
    // so this worker never holds state_mutex_ across bounded backend jobs.
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

    uint64_t session_id = 0;
    {
      std::lock_guard<std::mutex> bl(backend->m);
      session_id = backend->acquisition_session_id;
    }

    auto waiter = std::make_shared<CaptureWaiter>();
    waiter->width = job.request.width;
    waiter->height = job.request.height;
    waiter->fourcc = job.request.format_fourcc;
    {
      std::lock_guard<std::mutex> bl(backend->m);
      if (backend->closed || backend->reader_failed) {
        fail(ProviderError::ERR_PROVIDER_FAILED);
        return;
      }
      backend->waiters.push_back(waiter);
      backend->ensure_pump_locked();
    }

    bool done = false;
    {
      std::unique_lock<std::mutex> wl(waiter->m);
      done = waiter->cv.wait_for(wl, std::chrono::milliseconds(kCaptureSampleWaitMs),
                                 [&] { return waiter->done; });
    }
    if (!done) {
      // The stalled backend is contained here (brief §2): remove the waiter
      // and terminalize instead of blocking any contract call.
      {
        std::lock_guard<std::mutex> bl(backend->m);
        for (auto it = backend->waiters.begin(); it != backend->waiters.end(); ++it) {
          if (it->get() == waiter.get()) {
            backend->waiters.erase(it);
            break;
          }
        }
      }
      // Re-check under the waiter lock: the callback may have fulfilled it
      // between the timeout and the removal above.
      std::lock_guard<std::mutex> wl(waiter->m);
      if (!waiter->done) {
        waiter->done = true;
        waiter->ok = false;
        waiter->error = ProviderError::ERR_TIMEOUT;
      }
    }

    ProviderError waiter_error = ProviderError::ERR_PROVIDER_FAILED;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    bool ok = false;
    bool has_time = false;
    int64_t sample_time = 0;
    {
      std::lock_guard<std::mutex> wl(waiter->m);
      ok = waiter->ok;
      waiter_error = waiter->error;
      bytes = waiter->bytes;
      has_time = waiter->has_sample_time;
      sample_time = waiter->sample_time_100ns;
    }
    if (!ok || !bytes) {
      fail(waiter_error);
      return;
    }

    FrameView fv{};
    fv.device_instance_id = device_id;
    fv.stream_id = 0;
    fv.acquisition_session_id = session_id;
    fv.capture_id = capture_id;
    fv.capture_image.routing = CaptureImageRouting::DEFAULT_METERED;
    fv.capture_image.image_member_index = 0;
    fv.width = job.request.width;
    fv.height = job.request.height;
    fv.format_fourcc = job.request.format_fourcc;
    if (has_time) {
      fv.acquisition_timing = winrt_detail::make_acquisition_timing(sample_time);
    }
    fv.data = bytes->data();
    fv.size_bytes = bytes->size();
    fv.stride_bytes = job.request.width * 4u;
    // Fresh immutable allocation: publish for zero-copy retention (brief §4).
    fv.cpu_payload_owner = bytes;
    fv.requested_retained_plan = job.request.requested_retained_plan;
    fv.release = &winrt_detail::release_capture_frame;
    fv.release_user = new winrt_detail::CaptureFrameLease{bytes};
    strand_.post_frame(fv);

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
    // A started stream pins the shared reader geometry; a capture that needs
    // different geometry cannot execute without disrupting the stream.
    if (dev_it->second.backend) {
      std::lock_guard<std::mutex> bl(dev_it->second.backend->m);
      const auto& stream = dev_it->second.backend->stream;
      if (stream && stream->producing &&
          (stream->width != req.width || stream->height != req.height)) {
        return ProviderResult::failure(ProviderError::ERR_PLATFORM_CONSTRAINT);
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

  // 5. Close devices with real MF release on the control thread.
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
            ComPtr<IMFSourceReader> reader;
            ComPtr<IMFMediaSource> source;
            {
              std::lock_guard<std::mutex> bl(backend->m);
              reader = std::move(backend->reader);
              source = std::move(backend->source);
              backend->callback.Reset();
            }
            reader.Reset();
            if (source) {
              source->Shutdown();
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
  //    strand, then release the MF runtime and the control thread.
  strand_.flush();
  strand_.stop();

  if (mf_started_) {
    auto token = std::make_shared<BoundedControlExecutor::AbandonToken>();
    (void)control_.run_bounded(
        [](const BoundedControlExecutor::AbandonToken& /*t*/) { MFShutdown(); },
        token, kControlJobTimeoutMs);
    mf_started_ = false;
  }
  control_.stop();

  callbacks_ = nullptr;
  initialized_.store(false, std::memory_order_release);
  return ProviderResult::success();
}

} // namespace cambang
