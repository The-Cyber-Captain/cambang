// src/provider/windows_mediafoundation/windows_mf_provider.cpp

#ifdef _WIN32

#include "provider/windows_mediafoundation/windows_mf_provider.h"
#include "provider/windows_mediafoundation/windows_mf_types.h"

#include <cstring>

#include <mfapi.h>
#include <mferror.h>

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
// WindowsMfCameraProvider::SourceReaderCallback (nested)
// Only enqueues samples; worker thread drains queue and calls callbacks_->on_frame.
// -----------------------------------------------------------------------------
class WindowsMfCameraProvider::SourceReaderCallback final : public IMFSourceReaderCallback {
public:
  explicit SourceReaderCallback(WindowsMfCameraProvider::StreamState* s) : stream_(s) {}

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
    WindowsMfCameraProvider::SampleItem item{};
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
  WindowsMfCameraProvider::StreamState* stream_ = nullptr;
};

// -----------------------------------------------------------------------------
// WindowsMfCameraProvider
// -----------------------------------------------------------------------------

ProviderResult WindowsMfCameraProvider::ensure_com_initialized_() {
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

ProviderResult WindowsMfCameraProvider::ensure_mf_started_() {
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

ProviderResult WindowsMfCameraProvider::initialize(IProviderCallbacks* callbacks) {
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

ProviderResult WindowsMfCameraProvider::enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) {
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

ProviderResult WindowsMfCameraProvider::build_activation_for_hardware_id_(const std::string& hardware_id,
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

ProviderResult WindowsMfCameraProvider::open_device(const std::string& hardware_id,
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

ProviderResult WindowsMfCameraProvider::close_device(uint64_t device_instance_id) {
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

ProviderResult WindowsMfCameraProvider::create_stream(const StreamRequest& req) {
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

ProviderResult WindowsMfCameraProvider::destroy_stream(uint64_t stream_id) {
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

ProviderResult WindowsMfCameraProvider::start_stream(uint64_t stream_id) {
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

ProviderResult WindowsMfCameraProvider::stop_stream(uint64_t stream_id) {
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

ProviderResult WindowsMfCameraProvider::trigger_capture(const CaptureRequest&) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult WindowsMfCameraProvider::abort_capture(uint64_t) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult WindowsMfCameraProvider::apply_camera_spec_patch(const std::string&,
                                                               uint64_t,
                                                               SpecPatchView) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult WindowsMfCameraProvider::apply_imaging_spec_patch(uint64_t, SpecPatchView) {
  return ProviderResult::failure(ProviderError::ERR_NOT_SUPPORTED);
}

ProviderResult WindowsMfCameraProvider::shutdown() {
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

void WindowsMfCameraProvider::worker_thread_(uint64_t stream_id) {
  ComInit com(COINIT_MULTITHREADED);
  if (!com.ok) {
    callbacks_->on_stream_error(stream_id, ProviderError::ERR_PLATFORM_CONSTRAINT);
    return;
  }

  // Build attributes with async callback.
  ComPtr<IMFAttributes> attrs;
  HRESULT hr = ::MFCreateAttributes(attrs.put(), 2);
  if (FAILED(hr)) {
    callbacks_->on_stream_error(stream_id, provider_error_from_hr(hr));
    return;
  }

  auto* cb = new WindowsMfCameraProvider::SourceReaderCallback(&stream_);
  attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, cb);

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

  // Publish reader so stop_stream can Flush to cancel pending requests.
  stream_.reader = std::move(reader);

  // Kick the first async read.
  stream_.reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);

  // Worker processing loop: serialize callbacks -> core.
  for (;;) {
    WindowsMfCameraProvider::SampleItem item{};
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
        // MinGW MF headers may not expose IMFAttributes::GetINT32.
        // Use MFGetAttributeUINT32 helper and fall back to tightly-packed.
        const UINT32 s = MFGetAttributeUINT32(mt.get(), MF_MT_DEFAULT_STRIDE, 0);
        stride = (s != 0) ? static_cast<LONG>(s) : static_cast<LONG>(w) * 4;

      }
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

    FrameView fv{};
    fv.device_instance_id = stream_.req.device_instance_id;
    fv.stream_id = stream_id;
    fv.capture_id = 0;
    fv.width = w;
    fv.height = h;
    fv.format_fourcc = fourcc;
    fv.stride_bytes = static_cast<uint32_t>(stride);
    fv.timestamp_ns = ticks100ns_to_ns(item.timestamp_100ns);

    fv.data = reinterpret_cast<const uint8_t*>(data);
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

} // namespace cambang

#endif // _WIN32