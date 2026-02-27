#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "Core smoke: build with -DCAMBANG_INTERNAL_SMOKE=1 (via SCons: smoke=1)."
#endif
#include "core/core_runtime.h"
#include "core/state_snapshot_buffer.h"

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
#include "provider/stub/stub_camera_provider.h"
#endif

using namespace cambang;

namespace {

constexpr uint64_t kDeviceInstanceId = 1;
constexpr uint64_t kRootId = 1;
constexpr uint64_t kStreamId = 1;

struct Options {
  bool stress = false;
  int loops = 1;           // default non-stress
  int jitter_ms = 0;       // 0 = deterministic/no sleep jitter
  uint32_t seed = 1;       // only used if jitter_ms > 0
};

static void usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [--stress] [--loops=N] [--jitter_ms=K] [--seed=S]\n"
      << "Default: run once (smoke).\n"
      << "  --stress        Enable stress loop (default loops=50).\n"
      << "  --loops=N       Number of stress iterations.\n"
      << "  --jitter_ms=K   Max jitter sleep per step (0 disables).\n"
      << "  --seed=S        RNG seed for jitter (default 1).\n";
}

static bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

static bool parse_int(const std::string& s, int& out) {
  try {
    size_t idx = 0;
    int v = std::stoi(s, &idx, 10);
    if (idx != s.size()) return false;
    out = v;
    return true;
  } catch (...) {
    return false;
  }
}

static bool parse_u32(const std::string& s, uint32_t& out) {
  try {
    size_t idx = 0;
    unsigned long v = std::stoul(s, &idx, 10);
    if (idx != s.size()) return false;
    out = static_cast<uint32_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

static bool parse_opts(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return false;
    }
    if (a == "--stress") {
      opt.stress = true;
      continue;
    }
    if (starts_with(a, "--loops=")) {
      int v = 0;
      if (!parse_int(a.substr(8), v) || v <= 0) {
        std::cerr << "Invalid --loops\n";
        return false;
      }
      opt.loops = v;
      continue;
    }
    if (starts_with(a, "--jitter_ms=")) {
      int v = 0;
      if (!parse_int(a.substr(12), v) || v < 0) {
        std::cerr << "Invalid --jitter_ms\n";
        return false;
      }
      opt.jitter_ms = v;
      continue;
    }
    if (starts_with(a, "--seed=")) {
      uint32_t v = 0;
      if (!parse_u32(a.substr(7), v)) {
        std::cerr << "Invalid --seed\n";
        return false;
      }
      opt.seed = v;
      continue;
    }
    std::cerr << "Unknown arg: " << a << "\n";
    usage(argv[0]);
    return false;
  }

  if (opt.stress && opt.loops == 1) {
    opt.loops = 50; // default stress loops
  }
  return true;
}

static void maybe_jitter(const Options& opt, std::mt19937& rng) {
  if (opt.jitter_ms <= 0) return;
  std::uniform_int_distribution<int> d(0, opt.jitter_ms);
  const int ms = d(rng);
  if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

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

static bool wait_until(std::function<bool()> pred, int max_iters = 200, int sleep_ms = 5) {
  for (int i = 0; i < max_iters; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return false;
}

static std::shared_ptr<const CamBANGStateSnapshot> get_last_snapshot(StateSnapshotBuffer& buf) {
  return buf.snapshot_copy();
}

static bool wait_for_snapshot_gen(StateSnapshotBuffer& buf, uint64_t min_gen) {
  return wait_until([&]() {
    auto s = buf.snapshot_copy();
    return s && s->gen >= min_gen;
  });
}


static bool wait_for_snapshot_pred(
    StateSnapshotBuffer& buf,
    const std::function<bool(const CamBANGStateSnapshot&)>& pred) {
  return wait_until([&]() {
    auto s = buf.snapshot_copy();
    return s && pred(*s);
  });
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

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
static bool setup_one_stream(CoreRuntime& rt, StubCameraProvider& prov) {
  if (!prov.initialize(rt.provider_callbacks()).ok()) return false;

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) return false;

  if (!prov.open_device(eps[0].hardware_id, kDeviceInstanceId, kRootId).ok()) return false;

  const StreamRequest req = make_req();
  if (!prov.create_stream(req).ok()) return false;

  return true;
}
#endif

// ---- Tests (existing behaviour preserved) ----

static int test_publish_gating_before_start() {
  CoreRuntime pre;
  pre.request_publish();
  const auto st = pre.stats_copy();
  if (st.publish_requests_dropped_closed != 1) {
    std::cerr << "Expected publish drop before start (closed). got="
              << st.publish_requests_dropped_closed << "\n";
    return 1;
  }
  return 0;
}

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
static int test_baseline_live_one_frame_and_snapshot(CoreRuntime& rt,
                                                     StateSnapshotBuffer& buf,
                                                     StubCameraProvider& prov) {
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start\n";
    return 1;
  }

  // CoreRuntime now starts "dirty" and publishes an initial snapshot automatically.
  // The first snapshot is zero-indexed (gen=0).
  if (!wait_for_snapshot_gen(buf, 0)) {
    std::cerr << "Timeout waiting for initial snapshot publication\n";
    rt.stop();
    return 1;
  }

  if (!rt.start() || rt.state_copy() == CoreRuntimeState::STOPPED) {
    std::cerr << "Idempotent start failed\n";
    rt.stop();
    return 1;
  }

  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed\n";
    rt.stop();
    return 1;
  }

  rt.attach_provider(&prov);

  if (!prov.start_stream(kStreamId).ok()) {
    std::cerr << "start_stream failed\n";
    rt.stop();
    return 1;
  }

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

  // We need a snapshot that reflects the flowing stream and frame counters.
  // This may be published naturally (dirty-driven) when the frame fact is integrated,
  // or it may require an explicit smoke-only request_publish() to force a publish.
  rt.request_publish();
  if (!wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
        for (const auto& st : s.streams) {
          if (st.stream_id == kStreamId) {
            return (st.mode == CBStreamMode::FLOWING && st.frames_received >= 1 && st.frames_delivered >= 1);
          }
        }
        return false;
      })) {
    std::cerr << "Timeout waiting for snapshot containing flowing stream counters\n";
    rt.stop();
    return 1;
  }

  auto snap0 = get_last_snapshot(buf);
  if (!snap0 || snap0->schema_version != CamBANGStateSnapshot::kSchemaVersion) {
    std::cerr << "Snapshot missing or wrong schema_version\n";
    rt.stop();
    return 1;
  }

  bool found = false;
  for (const auto& s : snap0->streams) {
    if (s.stream_id == kStreamId) {
      found = true;
      if (s.mode != CBStreamMode::FLOWING || s.frames_received != 1 || s.frames_delivered != 1) {
        std::cerr << "Snapshot stream mismatch. mode=" << static_cast<int>(s.mode)
                  << " frames_received=" << s.frames_received
                  << " frames_delivered=" << s.frames_delivered << "\n";
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

  return 0;
}

#endif

static int test_baseline_publish_without_provider(CoreRuntime& rt, StateSnapshotBuffer& buf) {
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start\n";
    return 1;
  }

  // CoreRuntime starts "dirty" and publishes an initial snapshot automatically.
  // The first snapshot is zero-indexed (gen=0).
  if (!wait_for_snapshot_gen(buf, 0)) {
    std::cerr << "Timeout waiting for initial snapshot publication\n";
    rt.stop();
    return 1;
  }
  auto snap = get_last_snapshot(buf);
  if (!snap || snap->schema_version != CamBANGStateSnapshot::kSchemaVersion) {
    std::cerr << "Snapshot missing or wrong schema_version\n";
    rt.stop();
    return 1;
  }

  // Still validate request_publish() works once the core is live.
  // (Smoke-only; production should rely on dirty-driven publication.)
  rt.request_publish();
  if (!wait_for_snapshot_gen(buf, 1)) {
    std::cerr << "Timeout waiting for request_publish() snapshot\n";
    rt.stop();
    return 1;
  }
  rt.stop();
  return 0;
}

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
static int test_overload_queuefull_release_accounting(CoreRuntime& rt, StubCameraProvider& prov) {
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

  return 0;
}

static int test_shutdown_choreography(CoreRuntime& rt, StubCameraProvider& prov) {
  rt.stop();

  if (!prov.shutting_down()) {
    std::cerr << "Expected provider to be in shutting_down state after rt.stop()\n";
    return 1;
  }

  const auto diag = rt.shutdown_diag_copy();
  const uint8_t exit_code = CoreRuntime::shutdown_phase_exit_code();
  if (diag.phase_changes == 0 || diag.phase_code != exit_code) {
    std::cerr << "Shutdown phase diagnostics unexpected. phase_code="
              << static_cast<int>(diag.phase_code)
              << " changes=" << diag.phase_changes << "\n";
    return 1;
  }

  rt.stop(); // idempotent
  return 0;
}

#endif // CAMBANG_SMOKE_WITH_STUB_PROVIDER

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
// Lightweight per-iteration stress loop body (stub-provider mode).
static int stress_iteration(const Options& opt, std::mt19937& rng, int iter_index) {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);
  if (!rt.start()) {
    std::cerr << "[iter " << iter_index << "] CoreRuntime failed to start\n";
    return 1;
  }

  StubCameraProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "[iter " << iter_index << "] Stub provider setup failed\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);

  maybe_jitter(opt, rng);

  if (!prov.start_stream(kStreamId).ok()) {
    std::cerr << "[iter " << iter_index << "] start_stream failed\n";
    rt.stop();
    return 1;
  }

  maybe_jitter(opt, rng);

  // Emit a small burst and ensure all released.
  prov.emit_test_frames(kStreamId, 20);
  const uint64_t emitted = prov.frames_emitted();

  if (!wait_until([&]() { return prov.frames_released() >= emitted; }, 500, 2)) {
    std::cerr << "[iter " << iter_index << "] Timeout waiting for releases. emitted="
              << emitted << " released=" << prov.frames_released() << "\n";
    rt.stop();
    return 1;
  }

  // Occasionally validate overload accounting (every 10th iteration).
  if ((iter_index % 10) == 0) {
    const int r = test_overload_queuefull_release_accounting(rt, prov);
    if (r != 0) {
      std::cerr << "[iter " << iter_index << "] overload test failed\n";
      rt.stop();
      return r;
    }
  }

  maybe_jitter(opt, rng);

  rt.stop();

  if (!prov.shutting_down()) {
    std::cerr << "[iter " << iter_index << "] Expected provider shutting_down after stop\n";
    return 1;
  }

  if (prov.frames_released() != prov.frames_emitted()) {
    std::cerr << "[iter " << iter_index << "] Leak risk: emitted=" << prov.frames_emitted()
              << " released=" << prov.frames_released() << "\n";
    return 1;
  }

  const auto diag = rt.shutdown_diag_copy();
  if (diag.phase_changes == 0 || diag.phase_code != CoreRuntime::shutdown_phase_exit_code()) {
    std::cerr << "[iter " << iter_index << "] Shutdown diag unexpected. phase_code="
              << static_cast<int>(diag.phase_code)
              << " changes=" << diag.phase_changes << "\n";
    return 1;
  }

  return 0;
}

#endif // CAMBANG_SMOKE_WITH_STUB_PROVIDER

} // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_opts(argc, argv, opt)) {
    return 2;
  }

  // Default behaviour: run once, same structure/output as original.
  if (!opt.stress) {
    if (int r = test_publish_gating_before_start()) return r;

    CoreRuntime rt;
    StateSnapshotBuffer buf;
    rt.set_snapshot_publisher(&buf);

    // Providerless baseline (default smoke mode).
    if (int r = test_baseline_publish_without_provider(rt, buf)) return r;

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
    // Stub-provider integration tests (opt-in).
    StubCameraProvider prov;
    if (int r = test_baseline_live_one_frame_and_snapshot(rt, buf, prov)) return r;
    if (int r = test_overload_queuefull_release_accounting(rt, prov)) return r;
    if (int r = test_shutdown_choreography(rt, prov)) return r;
#endif

    std::cout << "OK: core spine smoke passed\n";
    return 0;
  }

  // Stress mode: portable churn loop.
  std::mt19937 rng(opt.seed);

  // Still do the pre-start gating check once.
  if (int r = test_publish_gating_before_start()) return r;

  const int progress_interval = 25;

#if !defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
  std::cerr << "Stress mode requires CAMBANG_SMOKE_WITH_STUB_PROVIDER (build with provider=stub).\n";
  return 2;
#else
  for (int i = 1; i <= opt.loops; ++i) {
    const int r = stress_iteration(opt, rng, i);
    if (r != 0) return r;

    if ((i % progress_interval) == 0 || i == opt.loops) {
      std::cout << "[stress] iteration " << i << "/" << opt.loops << " OK\n";
    }
  }

  std::cout << "OK: core spine smoke stress passed (loops=" << opt.loops << ")\n";
  return 0;
#endif
}