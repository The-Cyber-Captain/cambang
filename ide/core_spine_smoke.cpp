#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

#ifndef CAMBANG_INTERNAL_IDE_SMOKE
#error "IDE smoke harness: build with -DCAMBANG_INTERNAL_IDE_SMOKE=1 (or via SCons)."
#endif

#include "core/core_runtime.h"
#include "provider/stub/stub_camera_provider.h"

using namespace cambang;

namespace {

constexpr uint64_t kDeviceInstanceId = 1;
constexpr uint64_t kRootId = 1;
constexpr uint64_t kStreamId = 1;

// Mirrors CoreRuntime::ShutdownPhase numeric ordering (private to CoreRuntime).
// Temporary IDE smoke assertions only.
constexpr uint8_t kShutdownPhase_EXIT = 10;

static CoreDispatchStats get_dispatch_stats(CoreRuntime& rt) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    std::promise<CoreDispatchStats> p;
    auto fut = p.get_future();
    const auto r = rt.try_post([&rt, &p]() mutable { p.set_value(rt.dispatcher_stats()); });
    if (r == CoreThread::PostResult::Enqueued) {
      if (fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        return fut.get();
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::cerr << "Failed to retrieve dispatcher stats (core queue saturated or stopped).\n";
  std::exit(1);
}

static ProviderCallbackIngress::Stats get_ingress_stats(CoreRuntime& rt) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    std::promise<ProviderCallbackIngress::Stats> p;
    auto fut = p.get_future();
    const auto r = rt.try_post([&rt, &p]() mutable { p.set_value(rt.ingress_stats_copy()); });
    if (r == CoreThread::PostResult::Enqueued) {
      if (fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        return fut.get();
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::cerr << "Failed to retrieve ingress stats (core queue saturated or stopped).\n";
  std::exit(1);
}

static bool get_stream_record(CoreRuntime& rt, uint64_t stream_id, CoreStreamRegistry::StreamRecord& out) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    std::promise<bool> p;
    auto fut = p.get_future();
    const auto r = rt.try_post([&rt, stream_id, &out, &p]() mutable {
      const auto* rec = rt.stream_record(stream_id);
      if (!rec) {
        p.set_value(false);
        return;
      }
      out = *rec;
      p.set_value(true);
    });
    if (r == CoreThread::PostResult::Enqueued) {
      if (fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready) {
        return fut.get();
      }
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  std::cerr << "Failed to retrieve stream record (core queue saturated or stopped).\n";
  std::exit(1);
}

static CorePublisherBuffer::Stats get_publish_stats(CoreRuntime& rt) {
  return rt.publisher().stats_copy();
}

static CoreSnapshot get_last_snapshot(CoreRuntime& rt) {
  return rt.publisher().snapshot_copy();
}

static bool wait_until(std::function<bool()> pred, int max_iters = 200, int sleep_ms = 5) {
  for (int i = 0; i < max_iters; ++i) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return false;
}

static StreamRequest make_req() {
  StreamRequest req{};
  req.stream_id = kStreamId;
  req.device_instance_id = kDeviceInstanceId;
  req.intent = StreamIntent::PREVIEW;
  req.width = 2;
  req.height = 2;
  req.format_fourcc = 0;
  req.profile_version = 1;
  return req;
}

static bool setup_one_stream(CoreRuntime& rt, StubCameraProvider& prov) {
  if (!prov.initialize(rt.provider_callbacks()).ok()) {
    return false;
  }

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    return false;
  }

  if (!prov.open_device(eps[0].hardware_id, kDeviceInstanceId, kRootId).ok()) {
    return false;
  }

  const StreamRequest req = make_req();
  if (!prov.create_stream(req).ok()) {
    return false;
  }

  return true;
}

} // namespace

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

  // ---- Baseline LIVE path: one frame end-to-end + snapshot ----
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
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed\n";
    rt.stop();
    return 1;
  }

  // Attach provider so CoreRuntime::stop() can perform deterministic shutdown choreography.
  rt.attach_provider(&prov);

  if (!prov.start_stream(kStreamId).ok()) {
    std::cerr << "start_stream failed\n";
    rt.stop();
    return 1;
  }

  // Wait (bounded) for the core thread to dispatch and release the frame.
  if (!wait_until([&]() { return prov.frames_released() >= 1; })) {
    std::cerr << "Timeout waiting for frame release\n";
    rt.stop();
    return 1;
  }

  const CoreDispatchStats ds0 = get_dispatch_stats(rt);
  if (prov.frames_emitted() != 1 || prov.frames_released() != 1) {
    std::cerr << "Provider counters mismatch. emitted=" << prov.frames_emitted()
              << " released=" << prov.frames_released() << "\n";
    rt.stop();
    return 1;
  }
  if (ds0.frames_received != 1 || ds0.frames_released != 1) {
    std::cerr << "Dispatcher counters mismatch. received=" << ds0.frames_received
              << " released=" << ds0.frames_released << "\n";
    rt.stop();
    return 1;
  }

  CoreStreamRegistry::StreamRecord rec{};
  if (!get_stream_record(rt, kStreamId, rec)) {
    std::cerr << "Stream registry missing stream_id=" << kStreamId << "\n";
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
  if (!wait_until([&]() { return get_publish_stats(rt).publishes >= 1; })) {
    std::cerr << "Timeout waiting for snapshot publication\n";
    rt.stop();
    return 1;
  }

  const CoreSnapshot snap0 = get_last_snapshot(rt);
  if (snap0.seq == 0 || snap0.streams.empty()) {
    std::cerr << "Snapshot empty or missing seq\n";
    rt.stop();
    return 1;
  }

  bool found = false;
  for (const auto& s : snap0.streams) {
    if (s.stream_id == kStreamId) {
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
    std::cerr << "Snapshot missing stream_id=" << kStreamId << "\n";
    rt.stop();
    return 1;
  }

  // ---- 2) Overload facts during LIVE: QueueFull drops must be accounted + released ----
  {
    // Stall the core briefly so the ingress posting queue fills deterministically.
    (void)rt.try_post_core_thread_unchecked([]() {
      const auto t0 = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(25)) {
      }
    });

    const uint32_t kBurst = 1100;
    prov.emit_test_frames(kStreamId, kBurst);

    const uint64_t emitted = prov.frames_emitted();

    if (!wait_until([&]() { return prov.frames_released() >= emitted; }, 800, 2)) {
      std::cerr << "Timeout waiting for overload releases. emitted=" << emitted
                << " released=" << prov.frames_released() << "\n";
      rt.stop();
      return 1;
    }

    const auto ingress = get_ingress_stats(rt);
    const auto disp = get_dispatch_stats(rt);

    if (prov.frames_released() != emitted) {
      std::cerr << "Release mismatch under overload. emitted=" << emitted
                << " released=" << prov.frames_released() << "\n";
      rt.stop();
      return 1;
    }

    const uint64_t dropped = ingress.frames_dropped_full + ingress.frames_dropped_closed + ingress.frames_dropped_allocfail;
    if (dropped == 0) {
      std::cerr << "Expected at least one dropped frame under overload (QueueFull).\n";
      rt.stop();
      return 1;
    }

    if (disp.frames_received + dropped != emitted) {
      std::cerr << "Accounting mismatch under overload. received=" << disp.frames_received
                << " dropped=" << dropped << " emitted=" << emitted << "\n";
      rt.stop();
      return 1;
    }

    if (disp.frames_released != disp.frames_received) {
      std::cerr << "Dispatcher release mismatch under overload. received=" << disp.frames_received
                << " released=" << disp.frames_released << "\n";
      rt.stop();
      return 1;
    }

    const uint64_t released_on_drop = ingress.frames_released_on_drop_full +
                                     ingress.frames_released_on_drop_closed +
                                     ingress.frames_released_on_drop_allocfail;
    if (released_on_drop != dropped) {
      std::cerr << "Ingress release-on-drop mismatch. dropped=" << dropped
                << " released_on_drop=" << released_on_drop << "\n";
      rt.stop();
      return 1;
    }
  }

  // ---- Shutdown choreography (provider attached) + phase markers ----
  rt.stop();

  if (!prov.shutting_down()) {
    std::cerr << "Expected provider to be in shutting_down state after rt.stop()\n";
    return 1;
  }

  // Shutdown phase markers must have advanced and reached EXIT.
  {
    const auto diag = rt.shutdown_diag_copy();
    if (diag.phase_changes == 0 || diag.phase_code != kShutdownPhase_EXIT) {
      std::cerr << "Shutdown phase diagnostics unexpected. phase_code="
                << static_cast<int>(diag.phase_code)
                << " changes=" << diag.phase_changes << "\n";
      return 1;
    }
  }

  // Idempotent stop(): must not crash/hang.
  rt.stop();

  // ---- 3) Admission invariants + late provider facts during TEARING_DOWN (no provider attached) ----
  {
    CoreRuntime rt4;
    if (!rt4.start()) {
      std::cerr << "CoreRuntime rt4 failed to start\n";
      return 1;
    }

    StubCameraProvider prov4;
    if (!setup_one_stream(rt4, prov4)) {
      std::cerr << "Stub provider4 setup failed\n";
      rt4.stop();
      return 1;
    }

    if (!prov4.start_stream(kStreamId).ok()) {
      std::cerr << "start_stream (prov4) failed\n";
      rt4.stop();
      return 1;
    }

    // Ensure stream is started in core canonical state.
    CoreStreamRegistry::StreamRecord r0{};
    if (!get_stream_record(rt4, kStreamId, r0) || !r0.started) {
      std::cerr << "Expected started stream in core before teardown\n";
      rt4.stop();
      return 1;
    }

    std::thread stopper([&rt4]() { rt4.stop(); });

    // Emit the late stop fact immediately after stop begins (best-effort until core thread closes).
    prov4.emit_fact_stream_stopped(kStreamId, ProviderError::OK);

    // Wait until stop has begun (state leaves LIVE).
    if (!wait_until([&]() { return rt4.state_copy() != CoreRuntimeState::LIVE; }, 200, 1)) {
      std::cerr << "rt4 stop did not begin (state still LIVE)\n";
      stopper.join();
      return 1;
    }

    // Commands + publish must be rejected deterministically once TEARING_DOWN/STOPPED.
    const auto r_post = rt4.try_post([]() {});
    if (r_post != CoreThread::PostResult::Closed) {
      std::cerr << "Expected try_post Closed during TEARING_DOWN\n";
      stopper.join();
      return 1;
    }

    const auto st_before = rt4.stats_copy();
    rt4.request_publish();
    const auto st_after = rt4.stats_copy();
    if (st_after.publish_requests_dropped_closed != st_before.publish_requests_dropped_closed + 1) {
      std::cerr << "Expected publish request to be dropped Closed during TEARING_DOWN\n";
      stopper.join();
      return 1;
    }

    stopper.join();

    // Final snapshot should reflect the stop fact if it was integrated before final publish.
    const CoreSnapshot snap = get_last_snapshot(rt4);
    bool found_stream = false;
    for (const auto& s : snap.streams) {
      if (s.stream_id == kStreamId) {
        found_stream = true;
        if (s.started) {
          std::cerr << "Expected final snapshot to show stream stopped (started=false)\n";
          return 1;
        }
      }
    }
    if (!found_stream) {
      std::cerr << "Expected final snapshot to contain the stream record\n";
      return 1;
    }
  }

  // ---- 4) Frames after STOPPED: ingress must drop Closed + release-on-drop ----
  {
    CoreRuntime rt2;
    if (!rt2.start()) {
      std::cerr << "CoreRuntime rt2 failed to start\n";
      return 1;
    }

    StubCameraProvider prov2;
    if (!setup_one_stream(rt2, prov2)) {
      std::cerr << "Stub provider2 setup failed\n";
      rt2.stop();
      return 1;
    }

    rt2.stop();

    (void)prov2.start_stream(kStreamId);

    if (!wait_until([&]() { return prov2.frames_released() >= 1; })) {
      std::cerr << "Timeout waiting for frame release after runtime stop\n";
      return 1;
    }
  }

  // ---- 5) Frames during TEARING_DOWN: best-effort drain until Closed, no leaks ----
  {
    CoreRuntime rt3;
    if (!rt3.start()) {
      std::cerr << "CoreRuntime rt3 failed to start\n";
      return 1;
    }

    StubCameraProvider prov3;
    if (!setup_one_stream(rt3, prov3)) {
      std::cerr << "Stub provider3 setup failed\n";
      rt3.stop();
      return 1;
    }

    rt3.attach_provider(&prov3);

    if (!prov3.start_stream(kStreamId).ok()) {
      std::cerr << "start_stream (prov3) failed\n";
      rt3.stop();
      return 1;
    }

    // Begin stop() in another thread.
    std::thread stopper([&rt3]() { rt3.stop(); });

    // Emit frames during teardown. Some may enqueue and be released by dispatcher;
    // late ones may be dropped Closed and released on drop.
    prov3.emit_test_frames(kStreamId, 200);

    stopper.join();

    const uint64_t emitted = prov3.frames_emitted();
    if (prov3.frames_released() != emitted) {
      std::cerr << "Leak risk: frames not fully released during teardown. emitted="
                << emitted << " released=" << prov3.frames_released() << "\n";
      return 1;
    }

    if (!prov3.shutting_down()) {
      std::cerr << "Expected provider to be shutting_down after teardown\n";
      return 1;
    }
  }

  std::cout << "OK: core spine smoke passed\n";
  return 0;
}
