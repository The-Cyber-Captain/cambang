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

static bool get_stream_record(CoreRuntime& rt, uint64_t stream_id, CoreStreamRegistry::StreamRecord& out) {
  std::promise<bool> p;
  auto fut = p.get_future();
  rt.post([&rt, stream_id, &out, &p]() mutable {
    const auto* rec = rt.stream_record(stream_id);
    if (!rec) {
      p.set_value(false);
      return;
    }
    out = *rec;
    p.set_value(true);
  });
  return fut.get();
}

static CorePublisherBuffer::Stats get_publish_stats(CoreRuntime& rt) {
  return rt.publisher().stats_copy();
}

static CoreSnapshot get_last_snapshot(CoreRuntime& rt) {
  return rt.publisher().snapshot_copy();
}


int main() {
  // 1) Lifecycle gating before start(): publish requests must be rejected (Closed).
  {
    CoreRuntime pre;
    pre.request_publish();
    const auto st = pre.stats_copy();
    if (st.publish_requests_dropped_closed != 1) {
      std::cerr << "Expected publish drop before start (closed). got="
                << st.publish_requests_dropped_closed << "\n";
      return 1;
    }
  }

  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start\n";
    return 1;
  }

  // Idempotent start(): must succeed and keep running.
  if (!rt.start() || rt.state_copy() == CoreRuntimeState::STOPPED) {
    std::cerr << "Idempotent start failed\n";
    rt.stop();
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

  CoreStreamRegistry::StreamRecord rec{};
  if (!get_stream_record(rt, stream_id, rec)) {
    std::cerr << "Stream registry missing stream_id=" << stream_id << "\n";
    rt.stop();
    return 1;
  }
  if (!rec.created || !rec.started || rec.frames_received != 1 || rec.frames_released != 1) {
    std::cerr << "Stream registry mismatch. created=" << rec.created
              << " started=" << rec.started
              << " frames_received=" << rec.frames_received
              << " frames_released=" << rec.frames_released << "\n";
    rt.stop();
    return 1;
  }

  // Publish a snapshot and verify it contains the stream record.
  rt.request_publish();

  bool published = false;
  for (int i = 0; i < 200; ++i) {
    const auto ps = get_publish_stats(rt);
    if (ps.publishes >= 1) {
      published = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  if (!published) {
    std::cerr << "Timeout waiting for snapshot publication\n";
    rt.stop();
    return 1;
  }

  const CoreSnapshot snap = get_last_snapshot(rt);
  if (snap.seq == 0 || snap.streams.empty()) {
    std::cerr << "Snapshot empty or missing seq\n";
    rt.stop();
    return 1;
  }

  bool found = false;
  for (const auto& s : snap.streams) {
    if (s.stream_id == stream_id) {
      found = true;
      if (!s.created || !s.started || s.frames_received != 1 || s.frames_released != 1) {
        std::cerr << "Snapshot stream mismatch. created=" << s.created
                  << " started=" << s.started
                  << " frames_received=" << s.frames_received
                  << " frames_released=" << s.frames_released << "\n";
        rt.stop();
        return 1;
      }
      break;
    }
  }
  if (!found) {
    std::cerr << "Snapshot missing stream_id=" << stream_id << "\n";
    rt.stop();
    return 1;
  }

  rt.stop();

  // Idempotent stop(): must not crash/hang.
  rt.stop();

  // 2) After stop(): provider callbacks must be rejected and frames released on drop.
  // Re-start a new runtime for the provider to use.
  CoreRuntime rt2;
  if (!rt2.start()) {
    std::cerr << "CoreRuntime rt2 failed to start\n";
    return 1;
  }

  StubCameraProvider prov2;
  if (!prov2.initialize(rt2.provider_callbacks()).ok()) {
    std::cerr << "Stub provider2 initialize failed\n";
    rt2.stop();
    return 1;
  }

  std::vector<CameraEndpoint> eps2;
  if (!prov2.enumerate_endpoints(eps2).ok() || eps2.empty()) {
    std::cerr << "enumerate_endpoints (prov2) failed\n";
    rt2.stop();
    return 1;
  }

  if (!prov2.open_device(eps2[0].hardware_id, device_instance_id, root_id).ok()) {
    std::cerr << "open_device (prov2) failed\n";
    rt2.stop();
    return 1;
  }

  if (!prov2.create_stream(req).ok()) {
    std::cerr << "create_stream (prov2) failed\n";
    rt2.stop();
    return 1;
  }

  // Stop runtime before starting the stream. The provider will emit one frame on start_stream().
  rt2.stop();

  // Start stream after runtime is STOPPED. Ingress must treat as Closed and release the frame.
  (void)prov2.start_stream(stream_id);

  bool released = false;
  for (int i = 0; i < 200; ++i) {
    if (prov2.frames_released() >= 1) {
      released = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!released) {
    std::cerr << "Timeout waiting for frame release after runtime stop\n";
    return 1;
  }

  
  // 3) Provider facts during TEARING_DOWN: best-effort drain until core thread closes.
  {
    CoreRuntime rt3;
    if (!rt3.start()) {
      std::cerr << "CoreRuntime rt3 failed to start\n";
      return 1;
    }

    StubCameraProvider prov3;
    if (!prov3.initialize(rt3.provider_callbacks()).ok()) {
      std::cerr << "Stub provider3 initialize failed\n";
      rt3.stop();
      return 1;
    }

    std::vector<CameraEndpoint> eps3;
    if (!prov3.enumerate_endpoints(eps3).ok() || eps3.empty()) {
      std::cerr << "enumerate_endpoints (prov3) failed\n";
      rt3.stop();
      return 1;
    }

    if (!prov3.open_device(eps3[0].hardware_id, device_instance_id, root_id).ok()) {
      std::cerr << "open_device (prov3) failed\n";
      rt3.stop();
      return 1;
    }

    if (!prov3.create_stream(req).ok()) {
      std::cerr << "create_stream (prov3) failed\n";
      rt3.stop();
      return 1;
    }

    // Queue some bounded work so TEARING_DOWN has a deterministic window where the core loop is alive.
    for (int i = 0; i < 64; ++i) {
      rt3.post([]() {
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::microseconds(500)) {
        }
      });
    }

    std::thread stopper([&rt3]() { rt3.stop(); });

    // Emit a provider frame while teardown is in flight (best-effort).
    // We attempt after stop() begins; admission is ultimately decided by CoreThread::try_post().
    (void)prov3.start_stream(stream_id);

    stopper.join();

    // The emitted frame must have been released exactly once (either by dispatcher or by ingress on failure).
    if (prov3.frames_emitted() < 1 || prov3.frames_released() < 1) {
      std::cerr << "Expected provider frame release during teardown. emitted="
                << prov3.frames_emitted() << " released=" << prov3.frames_released() << "\n";
      return 1;
    }

    // Best-effort drain: if the frame was accepted before closure it will be dispatched and released on-core.
    // If the core loop closed before the callback could be enqueued, ingress must still release-on-drop.
    const auto ds = rt3.dispatcher_stats();
    if (ds.frames_received == 1) {
      if (ds.frames_released != 1) {
        std::cerr << "Dispatcher received frame but did not release it. released=" << ds.frames_released << "\n";
        return 1;
      }
    } else {
      // Drain did not occur (likely Closed during teardown). The provider must still observe the release.
      // This validates resource correctness under late callbacks.
      if (prov3.frames_released() != 1) {
        std::cerr << "Expected ingress release-on-drop during teardown\n";
        return 1;
      }
    }

  }

std::cout << "OK: core spine smoke passed\n";
  return 0;
}
