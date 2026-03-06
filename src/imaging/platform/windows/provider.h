// src/provider/windows_mediafoundation/windows_mf_provider.h
#pragma once

#ifdef _WIN32

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include <windows.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfobjects.h>

#include "imaging/api/icamera_provider.h"
#include "imaging/api/provider_strand.h"
#include "imaging/platform/windows/mf/com_ptr.h"

namespace cambang {

// Windows Media Foundation provider (DEV ACCELERATOR ONLY).
//
// Purpose:
// - Validate lifecycle + deterministic shutdown choreography on Windows.
// - Deliver frames into CamBANG pipeline with strict release-on-drop semantics.
// - Visibility is "best effort": we do NOT enable CPU format conversion. If the
//   camera outputs YUV/RAW, frames will be delivered and later dropped+released
//   by the dev sink.
//
// Non-goals (explicit):
// - Production-quality device selection, profile negotiation, or colorspace conversion.
// - Multi-stream, still capture, rigs, and spec patch support.
//
// Threading/COM rules (auditable):
// - MFStartup/MFShutdown: once per provider lifetime (initialize/shutdown), not per stream.
// - Any thread that touches COM/MF objects must call CoInitializeEx.
// - This provider uses MTA (COINIT_MULTITHREADED) for control + worker threads.
// - Provider->core callback context remains serialized via a single worker thread that
//   drains a queue populated by the MF async callback.
//
// NOTE:
// To keep StreamState private, the MF callback type is a *nested* class.
// In the .cpp, define it as `class WindowsProvider::SourceReaderCallback ...`
// (not in an anonymous namespace).
class WindowsProvider final : public ICameraProvider {
public:
  WindowsProvider() = default;
  ~WindowsProvider() override;

  const char* provider_name() const override { return "windows_mediafoundation(dev)"; }

  StreamTemplate stream_template() const override;
  bool supports_stream_picture_updates() const noexcept override { return false; }

  ProviderResult initialize(IProviderCallbacks* callbacks) override;
  ProviderResult enumerate_endpoints(std::vector<CameraEndpoint>& out_endpoints) override;

  ProviderResult open_device(
      const std::string& hardware_id,
      uint64_t device_instance_id,
      uint64_t root_id) override;

  ProviderResult close_device(uint64_t device_instance_id) override;

  ProviderResult create_stream(const StreamRequest& req) override;
  ProviderResult destroy_stream(uint64_t stream_id) override;

  ProviderResult start_stream(
      uint64_t stream_id,
      const CaptureProfile& profile,
      const PictureConfig& picture) override;
  ProviderResult stop_stream(uint64_t stream_id) override;

  ProviderResult set_stream_picture_config(uint64_t stream_id, const PictureConfig& picture) override;

  ProviderResult trigger_capture(const CaptureRequest& req) override;
  ProviderResult abort_capture(uint64_t capture_id) override;

  ProviderResult apply_camera_spec_patch(
      const std::string& hardware_id,
      uint64_t new_camera_spec_version,
      SpecPatchView patch) override;

  ProviderResult apply_imaging_spec_patch(
      uint64_t new_imaging_spec_version,
      SpecPatchView patch) override;

  ProviderResult shutdown() override;

private:
  struct DeviceState {
    std::string hardware_id;
    uint64_t device_instance_id = 0;
    uint64_t root_id = 0;
    bool open = false;
    uint64_t native_id = 0;

    ComPtr<IMFMediaSource> source;
    ComPtr<IMFActivate> activation;
  };

  // Queue item produced by MF async callback and consumed by the provider worker thread.
  struct SampleItem {
    DWORD flags = 0;
    LONGLONG timestamp_100ns = 0; // MF timestamps are in 100ns units.
    ComPtr<IMFSample> sample;     // Strong ref; released when popped/handled.
  };

  struct StreamState {
    StreamRequest req{};
    CaptureProfile active_profile{};
    PictureConfig active_picture{};
    bool created = false;
    bool started = false;
    bool producing = false;
    uint64_t native_id = 0;
    uint64_t frame_producer_native_id = 0;

    std::atomic<bool> stop_requested{false};
    std::atomic<bool> flushed{false};

    std::mutex q_m;
    std::condition_variable q_cv;
    std::deque<SampleItem> q;

    std::thread worker;
    bool worker_exited = true;
    std::condition_variable worker_cv;

    // Worker-thread-owned MF objects. Created/used/destroyed on worker thread.

    // For "log once / on change" type visibility.
    GUID last_logged_subtype = GUID_NULL;
    uint32_t last_logged_w = 0;
    uint32_t last_logged_h = 0;
    int32_t last_logged_stride = 0;
    bool logged_native_types = false;
    bool dumped_first_rgba4 = false;
    bool dumped_first_buflen = false;

  bool     seen_first_sample = false;
    ComPtr<IMFSourceReader> reader;
  };

  // Nested MF callback to allow access to private StreamState without making it public.
  class SourceReaderCallback;

  ProviderResult ensure_com_initialized_();
  ProviderResult ensure_mf_started_();

  ProviderResult build_activation_for_hardware_id_(
      const std::string& hardware_id,
      ComPtr<IMFActivate>& out_activate);

  void worker_thread_(uint64_t stream_id);
  uint64_t alloc_native_id_(NativeObjectType type) const;
  void emit_native_created_(uint64_t native_id, NativeObjectType type, uint64_t root_id, uint64_t owner_device_id, uint64_t owner_stream_id);
  void emit_native_destroyed_(uint64_t native_id);
  ProviderResult stop_stream_with_timeout_(uint64_t stream_id, std::chrono::milliseconds timeout);
  ProviderResult destroy_stream_forced_(uint64_t stream_id);

  CBProviderStrand strand_;

  IProviderCallbacks* callbacks_ = nullptr;
  bool initialized_ = false;
  bool mf_started_ = false;
  bool com_initialized_on_control_thread_ = false;

  std::mutex m_;
  bool shutting_down_ = false;

  DeviceState device_;
  StreamState stream_;
  uint64_t provider_native_id_ = 0;
};

} // namespace cambang

#endif // _WIN32
