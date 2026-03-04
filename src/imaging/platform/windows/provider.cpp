// src/provider/windows_mediafoundation/windows_mf_provider.cpp

#ifdef _WIN32

#include "imaging/platform/windows/provider.h"
#include "imaging/platform/windows/mf/types.h"

#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <cinttypes>

#include <objbase.h> // StringFromGUID2

#include <mfapi.h>
#include <mferror.h>

#include <godot_cpp/variant/utility_functions.hpp>

namespace cambang {

namespace {

// RAII for COM initialization on a thread.
struct ComInit {
  HRESULT hr = E_FAIL;
  bool ok = false;
  explicit ComInit(DWORD coinit) {
    hr = ::CoInitializeEx(nullptr, coinit);
    ok = SUCCEEDED(hr) || (hr == RPC_E_CHANGED_MODE);
  }
  ~ComInit() {
    if (hr == S_OK || hr == S_FALSE) {
      ::CoUninitialize();
    }
  }
};


static void mf_logf(const char* fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  // Route through Godot so it shows in the editor Output panel.
  godot::UtilityFunctions::print(buf);
}

static std::string guid_to_string(const GUID& g) {
  wchar_t wbuf[64] = {0};
  int n = ::StringFromGUID2(g, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
  if (n <= 0) return std::string("{guid?}");
  char buf[128] = {0};
  std::snprintf(buf, sizeof(buf), "%ls", wbuf);
  return std::string(buf);
}

// MinGW-friendly signed attribute fetch (handles VT_I4 / VT_UI4 etc).
static bool try_get_attr_i32(IMFAttributes* a, REFGUID key, int32_t* out) {
  if (!a || !out) return false;
  PROPVARIANT v;
  ::PropVariantInit(&v);
  HRESULT hr = a->GetItem(key, &v);
  if (FAILED(hr)) {
    ::PropVariantClear(&v);
    return false;
  }

  bool ok = false;
  int32_t val = 0;

  switch (v.vt) {
    case VT_I4:  val = v.lVal; ok = true; break;
    case VT_UI4: val = (int32_t)v.ulVal; ok = true; break;
    case VT_I8:  val = (int32_t)v.hVal.QuadPart; ok = true; break;
    case VT_UI8: val = (int32_t)v.uhVal.QuadPart; ok = true; break;
    default: break;
  }

  ::PropVariantClear(&v);
  if (!ok) return false;
  *out = val;
  return true;
}

static void log_media_type_once(const char* tag, IMFMediaType* mt) {
  if (!mt) {
    mf_logf("[MF] %s: <null media type>", tag);
    return;
  }

  GUID subtype = GUID_NULL;
  mt->GetGUID(MF_MT_SUBTYPE, &subtype);

  UINT32 w = 0, h = 0;
  MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);

  int32_t stride_i32 = 0;
  bool have_stride = try_get_attr_i32(mt, MF_MT_DEFAULT_STRIDE, &stride_i32);
  if (!have_stride) {
    UINT32 s = MFGetAttributeUINT32(mt, MF_MT_DEFAULT_STRIDE, 0);
    stride_i32 = (s != 0) ? (int32_t)s : 0;
  }

  mf_logf("[MF] %s: subtype=%s size=%ux%u stride=%" PRId32,
          tag, guid_to_string(subtype).c_str(), w, h, stride_i32);
}

static bool is_rgb32_family(const GUID& st) {
  return (st == MFVideoFormat_RGB32) || (st == MFVideoFormat_ARGB32);
}

static ComPtr<IMFMediaType> choose_native_type(IMFSourceReader* reader,
                                               const StreamRequest& req) {
  ComPtr<IMFMediaType> best;
  int best_score = -1;

  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> mt;
    HRESULT hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, mt.put());
    if (hr == MF_E_NO_MORE_TYPES) break;
    if (FAILED(hr) || !mt) continue;

    GUID st = GUID_NULL;
    mt->GetGUID(MF_MT_SUBTYPE, &st);

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(mt.get(), MF_MT_FRAME_SIZE, &w, &h);

    if (!is_rgb32_family(st)) continue;

    int score = 0;
    if (req.profile.width != 0 && req.profile.height != 0) {
      if (w == req.profile.width && h == req.profile.height) score += 100;
    }

    if (score > best_score) {
      best_score = score;
      best = std::move(mt);
      if (score >= 100) break;
    }
  }

  return best;
}


// Provider-owned frame release hook payload.
struct SampleReleasePayload {
  ComPtr<IMFSample> sample;
  ComPtr<IMFMediaBuffer> buffer;
  bool locked = false;
};

static void frame_release_hook(void* user, const cambang::FrameView* /*frame*/) {
    auto* p = static_cast<SampleReleasePayload*>(user);
    if (!p) return;

    if (p->locked && p->buffer) {
      p->buffer->Unlock();
    }
    delete p; // releases COM refs deterministically
  }

// Convert MF 100ns ticks to ns.
inline uint64_t ticks100ns_to_ns(LONGLONG t) {
  if (t <= 0) return 0;
  return static_cast<uint64_t>(t) * 100ull;
}

} // namespace

// -----------------------------------------------------------------------------
// WindowsProvider::SourceReaderCallback (nested)
// Only enqueues samples; worker thread drains queue and calls callbacks_->on_frame.
// -----------------------------------------------------------------------------
class WindowsProvider::SourceReaderCallback final : public IMFSourceReaderCallback {
public:
  explicit SourceReaderCallback(WindowsProvider::StreamState* s) : stream_(s) {}

  // IUnknown
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
      *ppv = static_cast<IMFSourceReaderCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  STDMETHODIMP_(ULONG) AddRef() override {
    return (ULONG)ref_.fetch_add(1, std::memory_order_relaxed) + 1;
  }

  STDMETHODIMP_(ULONG) Release() override {
    ULONG v = (ULONG)ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (v == 0) {
      delete this;
    }
    return v;
  }

  // IMFSourceReaderCallback
  STDMETHODIMP OnReadSample(HRESULT hrStatus,
                            DWORD dwStreamIndex,
                            DWORD dwStreamFlags,
                            LONGLONG llTimestamp,
                            IMFSample* pSample) override {
    (void)dwStreamIndex;

    if (!stream_) return S_OK;

    // Queue entry (even for failure/flags) so the worker can decide.
    WindowsProvider::SampleItem item{};
    item.flags = dwStreamFlags;
    item.timestamp_100ns = llTimestamp;

    if (SUCCEEDED(hrStatus) && pSample) {
      item.sample.reset(pSample);
      pSample->AddRef();
    }

    {
      std::lock_guard<std::mutex> lk(stream_->q_m);
      stream_->q.push_back(std::move(item));
    }
    stream_->q_cv.notify_one();

    // Continue requesting samples unless stop requested.
    if (!stream_->stop_requested.load(std::memory_order_relaxed) && stream_->reader) {
      stream_->reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                  0,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr);
    }

    return S_OK;
  }

  STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

  STDMETHODIMP OnFlush(DWORD) override {
    if (!stream_) return S_OK;
    stream_->flushed.store(true, std::memory_order_release);
    stream_->q_cv.notify_one();
    return S_OK;
  }

private:
  std::atomic<uint32_t> ref_{1};
  WindowsProvider::StreamState* stream_ = nullptr;
};

// -----------------------------------------------------------------------------
// WindowsProvider
// -----------------------------------------------------------------------------

ProviderResult WindowsProvider::ensure_com_initialized_() {
  if (com_initialized_on_control_thread_) {
    return ProviderResult::success();
  }
  HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    return ProviderResult::failure(ProviderError::ERR_PLATFORM_CONSTRAINT);
  }
  com_initialized_on_control_thread_ = true;
  return ProviderResult::success();
}

ProviderResult WindowsProvider::ensure_mf_started_() {
  if (mf_started_) {
    return ProviderResult::success();
  }
  HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    return ProviderResult::failure(provider_error_from_hr(hr));
  }
  mf_started_ = true;
  return ProviderResult::success();
}

ProviderResult WindowsProvider::initialize(IProviderCallbacks* callbacks) {
  if (!callbacks) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  std::lock_guard<std::mutex> lock(m_);
  if (initialized_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  callbacks_ = callbacks;

  // Explicit, auditable initialization discipline:
  // - CoInitializeEx is per-thread; initialize on the control thread here.
  // - MFStartup/MFShutdown are performed once per provider lifetime.
  ProviderResult pr = ensure_com_initialized_();
  if (!pr.ok()) return pr;

  pr = ensure_mf_started_();
  if (!pr.ok()) return pr;

  initialized_ = true;
  return ProviderResult::success();
}

ProviderResult WindowsProvider::enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) {
  std::lock_guard<std::mutex> lock(m_);
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  out_endpoints.clear();

  ComPtr<IMFAttributes> attrs;
  HRESULT hr = ::MFCreateAttributes(attrs.put(), 1);
  if (FAILED(hr)) return ProviderResult::failure(provider_error_from_hr(hr));

  hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) return ProviderResult::failure(provider_error_from_hr(hr));

  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  hr = ::MFEnumDeviceSources(attrs.get(), &devices, &count);
  if (FAILED(hr)) return ProviderResult::failure(provider_error_from_hr(hr));

  struct ReleaseEnum {
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    ~ReleaseEnum() {
      if (!devices) return;
      for (UINT32 i = 0; i < count; ++i) {
        if (devices[i]) devices[i]->Release();
      }
      ::CoTaskMemFree(devices);
    }
  } release_enum{devices, count};

  for (UINT32 i = 0; i < count; ++i) {
    WCHAR* symbolic = nullptr;
    UINT32 sym_len = 0;
    WCHAR* name = nullptr;
    UINT32 name_len = 0;

    hr = devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                        &symbolic,
                                        &sym_len);
    if (FAILED(hr) || !symbolic) continue;

    hr = devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &name_len);

    CameraEndpoint ep{};
    {
      std::wstring ws(symbolic);
      ep.hardware_id.assign(ws.begin(), ws.end());
    }
    if (name) {
      std::wstring wn(name);
      ep.name.assign(wn.begin(), wn.end());
    } else {
      ep.name = "Windows Camera";
    }

    ::CoTaskMemFree(symbolic);
    if (name) ::CoTaskMemFree(name);

    out_endpoints.push_back(std::move(ep));
  }

  return ProviderResult::success();
}

ProviderResult WindowsProvider::build_activation_for_hardware_id_(const std::string& hardware_id,
                                                                         ComPtr<IMFActivate>& out_activate) {
  ComPtr<IMFAttributes> attrs;
  HRESULT hr = ::MFCreateAttributes(attrs.put(), 1);
  if (FAILED(hr)) return ProviderResult::failure(provider_error_from_hr(hr));

  hr = attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  if (FAILED(hr)) return ProviderResult::failure(provider_error_from_hr(hr));

  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  hr = ::MFEnumDeviceSources(attrs.get(), &devices, &count);
  if (FAILED(hr)) return ProviderResult::failure(provider_error_from_hr(hr));

  struct ReleaseEnum {
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    ~ReleaseEnum() {
      if (!devices) return;
      for (UINT32 i = 0; i < count; ++i) {
        if (devices[i]) devices[i]->Release();
      }
      ::CoTaskMemFree(devices);
    }
  } release_enum{devices, count};

  for (UINT32 i = 0; i < count; ++i) {
    WCHAR* symbolic = nullptr;
    UINT32 sym_len = 0;
    hr = devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                                        &symbolic,
                                        &sym_len);
    if (FAILED(hr) || !symbolic) continue;

    std::wstring ws(symbolic);
    std::string id(ws.begin(), ws.end());
    ::CoTaskMemFree(symbolic);

    if (id == hardware_id) {
      out_activate.reset(devices[i]);
      devices[i]->AddRef();
      return ProviderResult::success();
    }
  }

  return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
}

ProviderResult WindowsProvider::open_device(const std::string& hardware_id,
                                                   uint64_t device_instance_id,
                                                   uint64_t root_id) {
  std::lock_guard<std::mutex> lock(m_);
  if (!initialized_ || shutting_down_) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (device_.open) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }

  ComPtr<IMFActivate> act;
  ProviderResult pr = build_activation_for_hardware_id_(hardware_id, act);
  if (!pr.ok()) return pr;

  ComPtr<IMFMediaSource> source;
  HRESULT hr = act->ActivateObject(__uuidof(IMFMediaSource), (void**)source.put());
  if (FAILED(hr)) return ProviderResult::failure(provider_error_from_hr(hr));

  device_.hardware_id = hardware_id;
  device_.device_instance_id = device_instance_id;
  device_.root_id = root_id;
  device_.activation = std::move(act);
  device_.source = std::move(source);
  device_.open = true;

  callbacks_->on_device_opened(device_instance_id);
  return ProviderResult::success();
}

ProviderResult WindowsProvider::close_device(uint64_t device_instance_id) {
  std::lock_guard<std::mutex> lock(m_);
  if (!device_.open || device_.device_instance_id != device_instance_id) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (stream_.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  device_.source.reset();
  if (device_.activation) {
    device_.activation->ShutdownObject();
  }
  device_.activation.reset();
  device_.open = false;

  callbacks_->on_device_closed(device_instance_id);
  return ProviderResult::success();
}

ProviderResult WindowsProvider::create_stream(const StreamRequest& req) {
  std::lock_guard<std::mutex> lock(m_);
  if (!device_.open || req.device_instance_id != device_.device_instance_id) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (stream_.created) {
    return ProviderResult::failure(ProviderError::ERR_BUSY);
  }

  stream_.req = req;
  stream_.created = true;
  stream_.started = false;
  stream_.stop_requested.store(false);
  stream_.flushed.store(false);

  callbacks_->on_stream_created(req.stream_id);
  return ProviderResult::success();
}

ProviderResult WindowsProvider::destroy_stream(uint64_t stream_id) {
  std::lock_guard<std::mutex> lock(m_);
  if (!stream_.created || stream_.req.stream_id != stream_id) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (stream_.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  stream_.created = false;
  callbacks_->on_stream_destroyed(stream_id);
  return ProviderResult::success();
}

ProviderResult WindowsProvider::start_stream(
    uint64_t stream_id,
    const CaptureProfile& /*profile*/,
    const PictureConfig& /*picture*/) {
  std::lock_guard<std::mutex> lock(m_);
  if (!stream_.created || stream_.req.stream_id != stream_id) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }
  if (stream_.started) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }
  if (!device_.source) {
    return ProviderResult::failure(ProviderError::ERR_BAD_STATE);
  }

  stream_.stop_requested.store(false);
  stream_.flushed.store(false);
  {
    std::lock_guard<std::mutex> qlk(stream_.q_m);
    stream_.q.clear();
  }

  stream_.worker = std::thread([this, stream_id]() { worker_thread_(stream_id); });

  stream_.started = true;
  callbacks_->on_stream_started(stream_id);
  return ProviderResult::success();
}

ProviderResult WindowsProvider::stop_stream(uint64_t stream_id) {
  std::unique_lock<std::mutex> lock(m_);
  if (!stream_.started || stream_.req.stream_id != stream_id) {
    return ProviderResult::failure(ProviderError::ERR_INVALID_ARGUMENT);
  }

  // Deterministic unblock strategy:
  // - Request stop
  // - Flush reader to cancel pending sample requests and discard queued samples.
  stream_.stop_requested.store(true, std::memory_order_release);

  if (stream_.reader) {
    stream_.reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  }

  // Wake worker in case it's waiting with an empty queue.
  stream_.q_cv.notify_one();

  lock.unlock();
  if (stream_.worker.joinable()) {
    stream_.worker.join();
  }
  lock.lock();

  stream_.reader.reset();
  stream_.started = false;

  callbacks_->on_stream_stopped(stream_id, ProviderError::OK);
  return ProviderResult::success();
}

ProviderResult WindowsProvider::trigger_capture(const CaptureRequest&) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult WindowsProvider::abort_capture(uint64_t) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult WindowsProvider::apply_camera_spec_patch(const std::string&,
                                                               uint64_t,
                                                               SpecPatchView) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult WindowsProvider::apply_imaging_spec_patch(uint64_t, SpecPatchView) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult WindowsProvider::shutdown() {
  std::unique_lock<std::mutex> lock(m_);
  if (!initialized_) {
    return ProviderResult::success();
  }
  shutting_down_ = true;

  if (stream_.started) {
    uint64_t sid = stream_.req.stream_id;
    lock.unlock();
    stop_stream(sid);
    lock.lock();
  }
  if (device_.open) {
    uint64_t did = device_.device_instance_id;
    lock.unlock();
    close_device(did);
    lock.lock();
  }

  if (mf_started_) {
    ::MFShutdown();
    mf_started_ = false;
  }

  initialized_ = false;
  callbacks_ = nullptr;
  return ProviderResult::success();
}

void WindowsProvider::worker_thread_(uint64_t stream_id) {
  ComInit com(COINIT_MULTITHREADED);
  if (!com.ok) {
    callbacks_->on_stream_error(stream_id, ProviderError::ERR_PLATFORM_CONSTRAINT);
    return;
  }

  // Build attributes with async callback.
  ComPtr<IMFAttributes> attrs;
  HRESULT hr = ::MFCreateAttributes(attrs.put(), 4);
  if (FAILED(hr)) {
    callbacks_->on_stream_error(stream_id, provider_error_from_hr(hr));
    return;
  }

  auto* cb = new WindowsProvider::SourceReaderCallback(&stream_);
  attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, cb);

  // Dev-accelerator: allow Source Reader converters so we can request RGB32 from YUV-only devices.
  // (CamBANG still has no CPU YUV->RGB paths; any conversion is inside MF.)
  // Note: we intentionally do NOT set MF_READWRITE_DISABLE_CONVERTERS.
  attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);


  // Create reader on this worker thread (reader will invoke cb on MF threads).
  ComPtr<IMFSourceReader> reader;
  {
    std::lock_guard<std::mutex> lock(m_);
    hr = ::MFCreateSourceReaderFromMediaSource(device_.source.get(), attrs.get(), reader.put());
  }
  if (FAILED(hr)) {
    cb->Release();
    callbacks_->on_stream_error(stream_id, provider_error_from_hr(hr));
    return;
  }

  // Enumerate native types once per stream and choose a candidate.
// (Do not publish reader yet; negotiation should occur before any ReadSample.)
if (!stream_.logged_native_types) {
  mf_logf("[MF] Enumerating native media types (video stream 0)...");
  for (DWORD i = 0;; ++i) {
    ComPtr<IMFMediaType> mt;
    HRESULT ehr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, mt.put());
    if (ehr == MF_E_NO_MORE_TYPES) break;
    if (FAILED(ehr) || !mt) continue;

    GUID st = GUID_NULL;
    mt->GetGUID(MF_MT_SUBTYPE, &st);
    if (is_rgb32_family(st)) {
      log_media_type_once("native (RGB32-family)", mt.get());
    }
  }
  stream_.logged_native_types = true;
}

// Choose best native type based on StreamRequest (use whatever is already there).

// Choose best native type based on StreamRequest (use whatever is already there).
ComPtr<IMFMediaType> chosen = choose_native_type(reader.get(), stream_.req);
HRESULT set_hr = S_OK;

if (chosen) {
  log_media_type_once("chosen native", chosen.get());
  set_hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, chosen.get());
  mf_logf("[MF] SetCurrentMediaType(native) hr=0x%08lx", (unsigned long)set_hr);
} else {
  mf_logf("[MF] No native RGB32/ARGB32 type found (will attempt converter-backed RGB32 in dev visibility mode).");

  // With converters enabled, ask the SourceReader for a converter-backed RGB32/ARGB32 stream.
  auto try_request = [&](const GUID& subtype, const char* label) -> HRESULT {
    ComPtr<IMFMediaType> mt;
    HRESULT hr = ::MFCreateMediaType(mt.put());
    if (FAILED(hr)) return hr;

    hr = mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return hr;

    hr = mt->SetGUID(MF_MT_SUBTYPE, subtype);
    if (FAILED(hr)) return hr;

    // Prefer requested size if provided; otherwise leave unset (let MF pick).
    if (stream_.req.profile.width != 0 && stream_.req.profile.height != 0) {
      (void)::MFSetAttributeSize(mt.get(), MF_MT_FRAME_SIZE, stream_.req.profile.width, stream_.req.profile.height);
    }

    // Progressive, if it sticks.
    (void)mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    // Prefer requested FPS range (use max as target).
    if (stream_.req.profile.target_fps_max != 0) {
      (void)::MFSetAttributeRatio(mt.get(), MF_MT_FRAME_RATE, stream_.req.profile.target_fps_max, 1);
    }

    log_media_type_once(label, mt.get());
    return reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt.get());
  };

  set_hr = try_request(MFVideoFormat_RGB32, "requested RGB32");
  mf_logf("[MF] SetCurrentMediaType(request RGB32) hr=0x%08lx", (unsigned long)set_hr);
  if (FAILED(set_hr)) {
    set_hr = try_request(MFVideoFormat_ARGB32, "requested ARGB32");
    mf_logf("[MF] SetCurrentMediaType(request ARGB32) hr=0x%08lx", (unsigned long)set_hr);
  }
}

// Log what actually stuck.
{
  ComPtr<IMFMediaType> cur;
  if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, cur.put())) && cur) {
    log_media_type_once("current (post-negotiate)", cur.get());
  }
}

// Publish reader so stop_stream can Flush to cancel pending requests.
stream_.reader = std::move(reader);

// Kick the first async read.
    mf_logf("[MF] kicking first ReadSample()");
    HRESULT rhr = stream_.reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(rhr)) {
      mf_logf("[MF] initial ReadSample failed hr=0x%08lx", (unsigned long)rhr);
    }

  // Worker processing loop: serialize callbacks -> core.
  for (;;) {
    WindowsProvider::SampleItem item{};
    {
      std::unique_lock<std::mutex> lk(stream_.q_m);
      stream_.q_cv.wait(lk, [&]() {
        return !stream_.q.empty() ||
               stream_.stop_requested.load(std::memory_order_acquire) ||
               stream_.flushed.load(std::memory_order_acquire);
      });

      if (!stream_.q.empty()) {
        item = std::move(stream_.q.front());
        stream_.q.pop_front();
      } else if (stream_.stop_requested.load(std::memory_order_acquire) &&
                 stream_.flushed.load(std::memory_order_acquire)) {
        break;
      } else if (stream_.stop_requested.load(std::memory_order_acquire)) {
        // Stop requested but no queued work: wait for flush.
        continue;
      } else {
        continue;
      }
    }

    if (item.flags & MF_SOURCE_READERF_ENDOFSTREAM) {
      break;
    }
    if (!item.sample) {
      continue;
    }

    if (!stream_.seen_first_sample) {
      mf_logf("[MF] first sample received (flags=0x%08x ts=%lld)", (unsigned)item.flags, (long long)item.timestamp_100ns);
      stream_.seen_first_sample = true;
    }

    // Determine current subtype.
    GUID subtype = GUID_NULL;
    uint32_t w = 0, h = 0;
    LONG stride = 0;
    {
      ComPtr<IMFMediaType> mt;
      if (stream_.reader &&
          SUCCEEDED(stream_.reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, mt.put())) &&
          mt) {
        mt->GetGUID(MF_MT_SUBTYPE, &subtype);

        UINT32 mw = 0, mh = 0;
        if (SUCCEEDED(MFGetAttributeSize(mt.get(), MF_MT_FRAME_SIZE, &mw, &mh))) {
          w = mw;
          h = mh;
        }
        int32_t stride_i32 = 0;
        bool have_stride = try_get_attr_i32(mt.get(), MF_MT_DEFAULT_STRIDE, &stride_i32);
        if (!have_stride) {
          const UINT32 s = MFGetAttributeUINT32(mt.get(), MF_MT_DEFAULT_STRIDE, 0);
          stride_i32 = (s != 0) ? (int32_t)s : 0;
        }
        if (stride_i32 == 0 && w != 0) {
          stride_i32 = (int32_t)w * 4;
        }
        stride = (LONG)stride_i32;

      }
    }

const int32_t stride_i32 = (int32_t)stride;
if (subtype != stream_.last_logged_subtype || w != stream_.last_logged_w ||
    h != stream_.last_logged_h || stride_i32 != stream_.last_logged_stride) {
  mf_logf("[MF] type change: subtype=%s size=%ux%u stride=%" PRId32,
          guid_to_string(subtype).c_str(), w, h, stride_i32);

  stream_.last_logged_subtype = subtype;
  stream_.last_logged_w = w;
  stream_.last_logged_h = h;
  stream_.last_logged_stride = stride_i32;
}

    const uint32_t fourcc = fourcc_from_mf_subtype(subtype);

    // Get contiguous buffer and lock it. We keep it locked until the core releases.
    ComPtr<IMFMediaBuffer> buf;
    hr = item.sample->ConvertToContiguousBuffer(buf.put());
    if (FAILED(hr) || !buf) {
      continue;
    }

    BYTE* data = nullptr;
    DWORD max_len = 0;
    DWORD cur_len = 0;
    hr = buf->Lock(&data, &max_len, &cur_len);
    if (FAILED(hr) || !data) {
      continue;
    }


if (!stream_.dumped_first_buflen) {
  const uint64_t expected_nv12 = (uint64_t)w * (uint64_t)h * 3u / 2u;
  const uint64_t expected_rgb32 = (uint64_t)w * (uint64_t)h * 4u;
  mf_logf("[MF] first buffer lens: cur_len=%lu max_len=%lu expected_nv12=%llu expected_rgb32=%llu",
          (unsigned long)cur_len, (unsigned long)max_len,
          (unsigned long long)expected_nv12, (unsigned long long)expected_rgb32);
  stream_.dumped_first_buflen = true;
}
    const int32_t signed_stride = (int32_t)stride;
    const int32_t abs_stride = (signed_stride < 0) ? -signed_stride : signed_stride;

    const uint8_t* start = reinterpret_cast<const uint8_t*>(data);
    if (signed_stride < 0 && h != 0) {
      start = start + (static_cast<size_t>(h) - 1) * static_cast<size_t>(abs_stride);
    }

    const size_t min_needed =
        static_cast<size_t>(abs_stride) * static_cast<size_t>(h);
    if (cur_len < min_needed) {
      // Buffer too small for claimed stride/height; drop.
      buf->Unlock();
      continue;
    }

    if (!stream_.dumped_first_rgba4 && fourcc == FOURCC_BGRA && cur_len >= 4) {
      mf_logf("[MF] first4 BGRA bytes: %02x %02x %02x %02x",
              (unsigned)data[0], (unsigned)data[1], (unsigned)data[2], (unsigned)data[3]);
      stream_.dumped_first_rgba4 = true;
    }

    FrameView fv{};
    fv.device_instance_id = stream_.req.device_instance_id;
    fv.stream_id = stream_id;
    fv.capture_id = 0;
    fv.width = w;
    fv.height = h;
    fv.format_fourcc = fourcc;
    fv.stride_bytes = static_cast<uint32_t>(abs_stride);
    // Media Foundation sample time is in 100ns units and is provider-monotonic.
    fv.capture_timestamp.value = static_cast<uint64_t>(item.timestamp_100ns);
    fv.capture_timestamp.tick_ns = 100;
    fv.capture_timestamp.domain = CaptureTimestampDomain::PROVIDER_MONOTONIC;

    fv.data = start;
    fv.size_bytes = static_cast<size_t>(cur_len);

    auto* payload = new SampleReleasePayload();
    payload->sample = std::move(item.sample);
    payload->buffer = std::move(buf);
    payload->locked = true;

    fv.release = &frame_release_hook;
    fv.release_user = payload;

    callbacks_->on_frame(fv);
  }

  // Ensure any pending reads are canceled and OnFlush is delivered.
  if (stream_.reader) {
    stream_.reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  }

  // Release callback object (balances our new).
  cb->Release();
}

StreamTemplate WindowsProvider::stream_template() const {
  StreamTemplate t{};
  t.profile.width = 320;
  t.profile.height = 180;
  t.profile.format_fourcc = FOURCC_BGRA;
  t.profile.target_fps_min = 30;
  t.profile.target_fps_max = 60;
  // PictureConfig defaults are synthetic-only; platform-backed providers ignore pattern.
  t.picture.preset = PatternPreset::XyXor;
  t.picture.seed = 0;
  t.picture.overlay_frame_index_offsets = false;
  t.picture.overlay_moving_bar = false;
  return t;
}

ProviderResult WindowsProvider::set_stream_picture_config(uint64_t /*stream_id*/, const PictureConfig& /*picture*/) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

} // namespace cambang

#endif // _WIN32