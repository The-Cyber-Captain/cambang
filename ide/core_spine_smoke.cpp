#include <chrono>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

#include "core/core_runtime.h"
#include "provider/stub/stub_camera_provider.h"

using namespace cambang;

static CoreDispatchStats get_dispatch_stats(CoreRuntime& rt) {
  std::promise<CoreDispatchStats> p;
  auto fut = p.get_future();
  rt.post([&rt, &p]() mutable {
    p.set_value(rt.dispatcher_stats());
  });
  return fut.get();
}

int main() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start\n";
    return 1;
  }

  StubCameraProvider prov;
  if (!prov.initialize(rt.provider_callbacks()).ok()) {
    std::cerr << "Stub provider initialize failed\n";
    rt.stop();
    return 1;
  }

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    std::cerr << "enumerate_endpoints failed\n";
    rt.stop();
    return 1;
  }

  const uint64_t device_instance_id = 1;
  const uint64_t root_id = 1;
  const uint64_t stream_id = 1;

  if (!prov.open_device(eps[0].hardware_id, device_instance_id, root_id).ok()) {
    std::cerr << "open_device failed\n";
    rt.stop();
    return 1;
  }

  StreamRequest req{};
  req.stream_id = stream_id;
  req.device_instance_id = device_instance_id;
  req.intent = StreamIntent::PREVIEW;
  req.width = 2;
  req.height = 2;
  req.format_fourcc = 0; // irrelevant for this smoke
  req.profile_version = 1;

  if (!prov.create_stream(req).ok()) {
    std::cerr << "create_stream failed\n";
    rt.stop();
    return 1;
  }

  if (!prov.start_stream(stream_id).ok()) {
    std::cerr << "start_stream failed\n";
    rt.stop();
    return 1;
  }

  // Wait (bounded) for the core thread to dispatch and release the frame.
  bool ok = false;
  CoreDispatchStats stats{};
  for (int i = 0; i < 200; ++i) {
    if (prov.frames_released() >= 1) {
      stats = get_dispatch_stats(rt);
      ok = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  if (!ok) {
    std::cerr << "Timeout waiting for frame release\n";
    rt.stop();
    return 1;
  }

  if (prov.frames_emitted() != 1 || prov.frames_released() != 1) {
    std::cerr << "Provider counters mismatch. emitted=" << prov.frames_emitted()
              << " released=" << prov.frames_released() << "\n";
    rt.stop();
    return 1;
  }

  if (stats.frames_received != 1 || stats.frames_released != 1) {
    std::cerr << "Dispatcher counters mismatch. received=" << stats.frames_received
              << " released=" << stats.frames_released << "\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  std::cout << "OK: core spine smoke passed\n";
  return 0;
}
