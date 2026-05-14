#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include "imaging/platform/windows/provider.h"

using namespace cambang;

namespace {

struct RuntimeCallbacks final : IProviderCallbacks {
  uint64_t next_native_id = 1;
  uint64_t stream_errors = 0;
  uint64_t device_errors = 0;

  uint64_t allocate_native_id(NativeObjectType) override { return next_native_id++; }
  uint64_t core_monotonic_now_ns() override { return 0; }
  bool is_stream_display_demand_active(uint64_t) override { return false; }

  void on_device_opened(uint64_t) override {}
  void on_device_closed(uint64_t) override {}
  void on_stream_created(uint64_t) override {}
  void on_stream_destroyed(uint64_t) override {}
  void on_stream_started(uint64_t) override {}
  void on_stream_stopped(uint64_t, ProviderError) override {}
  void on_capture_started(uint64_t, uint64_t) override {}
  void on_capture_completed(uint64_t, uint64_t) override {}
  void on_capture_failed(uint64_t, uint64_t, ProviderError) override {}
  void on_frame(const FrameView&) override {}
  void on_device_error(uint64_t, ProviderError) override { ++device_errors; }
  void on_stream_error(uint64_t, ProviderError) override { ++stream_errors; }
  void on_native_object_created(const NativeObjectCreateInfo&) override {}
  void on_native_object_destroyed(const NativeObjectDestroyInfo&) override {}
};

bool has_flag(int argc, char** argv, const char* flag) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == flag) {
      return true;
    }
  }
  return false;
}

int run_real_hardware_validation() {
  RuntimeCallbacks cb;
  WindowsProvider provider;

  ProviderResult r = provider.initialize(&cb);
  if (!r.ok()) {
    std::cerr << "FAIL windows_mf_runtime_validate: initialize failed\n";
    return 1;
  }

  std::vector<CameraEndpoint> endpoints;
  r = provider.enumerate_endpoints(endpoints);
  if (!r.ok()) {
    std::cerr << "FAIL windows_mf_runtime_validate: enumerate_endpoints failed\n";
    (void)provider.shutdown();
    return 1;
  }
  if (endpoints.empty()) {
    std::cerr << "WARN windows_mf_runtime_validate: no camera endpoints found; skipping runtime validation\n";
    (void)provider.shutdown();
    return 0;
  }

  const auto& ep = endpoints[0];
  if (!provider.open_device(ep.hardware_id, 1, 4001).ok()) {
    std::cerr << "FAIL windows_mf_runtime_validate: open_device failed\n";
    (void)provider.shutdown();
    return 1;
  }

  StreamRequest req{};
  req.stream_id = 21;
  req.device_instance_id = 1;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 640;
  req.profile.height = 480;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 30;
  req.profile.target_fps_max = 30;

  if (!provider.create_stream(req).ok()) {
    std::cerr << "FAIL windows_mf_runtime_validate: create_stream failed\n";
    (void)provider.shutdown();
    return 1;
  }
  if (!provider.start_stream(req.stream_id, req.profile, req.picture).ok()) {
    std::cerr << "FAIL windows_mf_runtime_validate: start_stream failed\n";
    (void)provider.shutdown();
    return 1;
  }

  const ProviderResult shutdown_r = provider.shutdown();
  if (!shutdown_r.ok()) {
    std::cerr << "FAIL windows_mf_runtime_validate: shutdown failed (error="
              << static_cast<unsigned>(shutdown_r.code) << ")\n";
    return 1;
  }

  if (cb.device_errors > 0 || cb.stream_errors > 0) {
    std::cerr << "FAIL windows_mf_runtime_validate: provider reported runtime errors"
              << " (device_errors=" << cb.device_errors
              << ", stream_errors=" << cb.stream_errors << ")\n";
    return 1;
  }

  std::cout << "PASS windows_mf_runtime_validate\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  if (!has_flag(argc, argv, "--real-hardware")) {
    std::cout << "Refusing to access physical cameras without explicit opt-in.\n"
              << "Usage: windows_mf_runtime_validate --real-hardware\n";
    return 0;
  }
  return run_real_hardware_validation();
}

#else
int main(int, char**) {
  std::cout << "windows_mf_runtime_validate is only supported on Windows builds.\n";
  return 0;
}
#endif
