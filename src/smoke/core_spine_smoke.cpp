/*
CamBANG Maintainer Utility

Tool: core_spine_smoke

Purpose
-------
Validates the minimal Core runtime invariants using the stub provider.

This executable is a fast CI gate: it proves the core thread can start,
publish snapshots, and attach a provider without involving platform backends
or SyntheticProvider.

Category
--------
Smoke test (maintainer/CI).

Non-Goals
---------
- Not a performance benchmark
- Not a user-facing test harness (Godot)
- SyntheticProvider must not be introduced here
*/

#include <chrono>
#include <algorithm>
#include <atomic>
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
#include "imaging/stub/provider.h"
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


static bool wait_for_snapshot_version(StateSnapshotBuffer& buf, uint64_t gen, uint64_t min_version) {
  return wait_until([&]() {
    auto s = buf.snapshot_copy();
    return s && s->gen == gen && s->version >= min_version;
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

static const char* compiled_provider_name() {
#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
  return "stub";
#else
  return "unset";
#endif
}

static StreamRequest make_req() {
  StreamRequest req{};
  req.stream_id = kStreamId;
  req.device_instance_id = kDeviceInstanceId;
  req.intent = StreamIntent::PREVIEW;
  req.profile.width = 2;
  req.profile.height = 2;
  req.profile.format_fourcc = FOURCC_RGBA;
  req.profile.target_fps_min = 0;
  req.profile.target_fps_max = 0;

  req.picture.preset = PatternPreset::XyXor;
  req.picture.seed = 0;
  req.picture.overlay_frame_index_offsets = false;
  req.picture.overlay_moving_bar = false;
  req.profile_version = 1;
  return req;
}

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
static bool setup_one_stream(CoreRuntime& rt, StubProvider& prov) {
  if (!prov.initialize(rt.provider_callbacks()).ok()) return false;

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) return false;

  rt.retain_device_identity(kDeviceInstanceId, eps[0].hardware_id);

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
                                                     StubProvider& prov) {
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

  const StreamRequest req = make_req();
  if (!prov.start_stream(kStreamId, req.profile, req.picture).ok()) {
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
  if (!rec.created || !rec.started || rec.frames_received != 1) {
    std::cerr << "Stream registry mismatch. created=" << rec.created
              << " started=" << rec.started
              << " frames_received=" << rec.frames_received
              << " frames_released=" << rec.frames_released
              << " frames_dropped=" << rec.frames_dropped << "\n";
    rt.stop();
    return 1;
  }

  // Frame accounting boundary:
  // - delivered requires sink handoff (frames_released)
  // - no-sink release-on-drop increments frames_dropped
  if ((rec.frames_released + rec.frames_dropped) != 1) {
    std::cerr << "Stream registry mismatch. created=" << rec.created
              << " started=" << rec.started
              << " frames_received=" << rec.frames_received
              << " frames_released=" << rec.frames_released
              << " frames_dropped=" << rec.frames_dropped << "\n";
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
            return (st.mode == CBStreamMode::FLOWING && st.frames_received >= 1 &&
                    (st.frames_delivered + st.frames_dropped) >= 1);
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
      if (s.mode != CBStreamMode::FLOWING || s.frames_received != 1 ||
          (s.frames_delivered + s.frames_dropped) != 1) {
        std::cerr << "Snapshot stream mismatch. mode=" << static_cast<int>(s.mode)
                  << " frames_received=" << s.frames_received
                  << " frames_delivered=" << s.frames_delivered
                  << " frames_dropped=" << s.frames_dropped << "\n";
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
  if (!wait_for_snapshot_version(buf, /*gen=*/0, /*min_version=*/1)) {
    std::cerr << "Timeout waiting for request_publish() snapshot\n";
    rt.stop();
    return 1;
  }
  rt.stop();
  return 0;
}

#if defined(CAMBANG_SMOKE_WITH_STUB_PROVIDER)
static int test_overload_queuefull_release_accounting(CoreRuntime& rt, StubProvider& prov) {
    std::atomic<bool> stall_started{false};

  (void)rt.try_post_core_thread_unchecked([&stall_started]() {
    stall_started.store(true, std::memory_order_release);
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(25)) {
    }
  });
  // Barrier: ensure the core thread has *actually entered* the stall
  // before we emit the overload burst.
  while (!stall_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

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

static int test_shutdown_choreography(CoreRuntime& rt, StubProvider& prov) {
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

static int test_rig_preflight_materialization_smoke() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (rig preflight smoke)\n";
    return 1;
  }

  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed (rig preflight smoke)\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);

  // Missing rig.
  {
    const auto res = rt.preflight_rig_participants_materialize(9001);
    if (res.ok || res.failure != CoreRuntime::RigPreflightFailure::RigNotFound) {
      std::cerr << "Expected RigNotFound for missing rig\n";
      rt.stop();
      return 1;
    }
  }

  // Empty membership.
  if (!rt.smoke_set_rig_member_hardware_ids(7001, {})) {
    std::cerr << "Failed to set rig members for empty-membership case\n";
    rt.stop();
    return 1;
  }
  {
    const auto res = rt.preflight_rig_participants_materialize(7001);
    if (res.ok || res.failure != CoreRuntime::RigPreflightFailure::EmptyMembership) {
      std::cerr << "Expected EmptyMembership for rig with no members\n";
      rt.stop();
      return 1;
    }
  }

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    std::cerr << "Failed to enumerate endpoints (rig preflight smoke)\n";
    rt.stop();
    return 1;
  }
  const std::string live_hw = eps[0].hardware_id;

  // Unresolved hardware id.
  if (!rt.smoke_set_rig_member_hardware_ids(7002, {"missing:hw"})) {
    std::cerr << "Failed to set unresolved hardware id case\n";
    rt.stop();
    return 1;
  }
  {
    const auto res = rt.preflight_rig_participants_materialize(7002);
    if (res.ok || res.failure != CoreRuntime::RigPreflightFailure::HardwareIdUnresolved) {
      std::cerr << "Expected HardwareIdUnresolved\n";
      rt.stop();
      return 1;
    }
  }

  // Success.
  if (!rt.smoke_set_rig_member_hardware_ids(7003, {live_hw})) {
    std::cerr << "Failed to set success case membership\n";
    rt.stop();
    return 1;
  }
  {
    const auto res = rt.preflight_rig_participants_materialize(7003);
    if (!res.ok || res.failure != CoreRuntime::RigPreflightFailure::None || res.participants.size() != 1) {
      std::cerr << "Expected successful single participant preflight\n";
      rt.stop();
      return 1;
    }
    if (res.participants[0].hardware_id != live_hw ||
        res.participants[0].device_instance_id != kDeviceInstanceId ||
        res.participants[0].request.device_instance_id != kDeviceInstanceId ||
        res.participants[0].request.rig_id != 0) {
      std::cerr << "Unexpected participant resolution/materialization output\n";
      rt.stop();
      return 1;
    }
    const auto& members = res.participants[0].request.still_image_bundle.members;
    if (members.size() != 1 ||
        members[0].image_member_index != 0 ||
        members[0].role != CaptureStillImageMemberRole::DEFAULT_METERED) {
      std::cerr << "Expected rig preflight materialized request with one default-metered image member\n";
      rt.stop();
      return 1;
    }
  }

  // Duplicate resolved participant (same hardware id listed twice).
  if (!rt.smoke_set_rig_member_hardware_ids(7004, {live_hw, live_hw})) {
    std::cerr << "Failed to set duplicate-resolution case\n";
    rt.stop();
    return 1;
  }
  {
    const auto res = rt.preflight_rig_participants_materialize(7004);
    if (res.ok || res.failure != CoreRuntime::RigPreflightFailure::DuplicateResolvedDevice) {
      std::cerr << "Expected DuplicateResolvedDevice\n";
      rt.stop();
      return 1;
    }
  }

  rt.stop();
  return 0;
}

static int test_device_capture_request_materialization_smoke() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (device materialization smoke)\n";
    return 1;
  }
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed (device materialization smoke)\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);

  CaptureRequest req{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req)) {
    std::cerr << "Expected materialized capture request for live device\n";
    rt.stop();
    return 1;
  }
  if (req.still_image_bundle.members.size() != 1 ||
      req.still_image_bundle.members[0].image_member_index != 0 ||
      req.still_image_bundle.members[0].role != CaptureStillImageMemberRole::DEFAULT_METERED) {
    std::cerr << "Expected device materialized request with one default-metered image member\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_open_device_snapshot_retains_default_capture_profile_smoke() {
  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (default capture profile snapshot smoke)\n";
    return 1;
  }
  if (!wait_for_snapshot_gen(buf, 0)) {
    std::cerr << "Timeout waiting for initial snapshot (default capture profile snapshot smoke)\n";
    rt.stop();
    return 1;
  }

  StubProvider prov;
  if (!prov.initialize(rt.provider_callbacks()).ok()) {
    std::cerr << "Stub provider initialize failed (default capture profile snapshot smoke)\n";
    rt.stop();
    return 1;
  }
  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    std::cerr << "Stub provider enumerate failed (default capture profile snapshot smoke)\n";
    rt.stop();
    return 1;
  }

  rt.attach_provider(&prov);
  if (rt.try_open_device(eps[0].hardware_id, kDeviceInstanceId, kRootId) != TryOpenDeviceStatus::OK) {
    std::cerr << "try_open_device failed (default capture profile snapshot smoke)\n";
    rt.stop();
    return 1;
  }

  if (!wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
        for (const auto& d : s.devices) {
          if (d.instance_id != kDeviceInstanceId || d.phase != CBLifecyclePhase::LIVE) continue;
          const auto& still = d.capture_profile.still;
          const auto& members = still.still_image_bundle.members;
          return still.version == 0 &&
                 still.width != 0 &&
                 still.height != 0 &&
                 still.format != 0 &&
                 members.size() == 1 &&
                 members[0].image_member_index == 0 &&
                 members[0].role == CaptureStillImageMemberRole::DEFAULT_METERED &&
                 members[0].intended_exposure_compensation_milli_ev == 0;
        }
        return false;
      })) {
    std::cerr << "Timeout waiting for opened device snapshot with retained default still profile\n";
    rt.stop();
    return 1;
  }

  CaptureRequest req{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req)) {
    std::cerr << "Expected materialized request for default capture profile snapshot smoke\n";
    rt.stop();
    return 1;
  }

  auto snap = get_last_snapshot(buf);
  if (!snap) {
    std::cerr << "Snapshot missing after default capture profile wait\n";
    rt.stop();
    return 1;
  }
  const DeviceState* device = nullptr;
  for (const auto& d : snap->devices) {
    if (d.instance_id == kDeviceInstanceId) {
      device = &d;
      break;
    }
  }
  if (!device) {
    std::cerr << "Opened device missing from snapshot after default capture profile wait\n";
    rt.stop();
    return 1;
  }

  const auto& still = device->capture_profile.still;
  if (still.version != 0 ||
      still.width != req.width ||
      still.height != req.height ||
      still.format != req.format_fourcc) {
    std::cerr << "Snapshot default still profile mismatch. snapshot version=" << still.version
              << " width=" << still.width
              << " height=" << still.height
              << " format=" << still.format
              << " materialized width=" << req.width
              << " height=" << req.height
              << " format=" << req.format_fourcc << "\n";
    rt.stop();
    return 1;
  }
  const auto& members = still.still_image_bundle.members;
  if (members.size() != 1 ||
      members[0].image_member_index != 0 ||
      members[0].role != CaptureStillImageMemberRole::DEFAULT_METERED ||
      members[0].intended_exposure_compensation_milli_ev != 0) {
    std::cerr << "Snapshot default still_image_bundle was not a single DEFAULT_METERED member\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_still_capture_profile_version_idempotency_smoke(StateSnapshotBuffer& buf) {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (still profile idempotency smoke)\n";
    return 1;
  }
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed (still profile idempotency smoke)\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);
  rt.set_snapshot_publisher(&buf);
  if (!wait_for_snapshot_gen(buf, 0)) {
    std::cerr << "Timeout waiting for initial snapshot publication (still profile idempotency smoke)\n";
    rt.stop();
    return 1;
  }

  CaptureRequest req{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req)) {
    std::cerr << "Expected materialized request for baseline version check\n";
    rt.stop();
    return 1;
  }
  const uint64_t v0 = req.profile_version;
  const auto default_bundle_ready = wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
    if (s.devices.empty()) return false;
    const auto& d = s.devices[0];
    return d.instance_id == kDeviceInstanceId &&
           d.capture_profile.still.still_image_bundle.members.size() == 1 &&
           d.capture_profile.still.still_image_bundle.members[0].image_member_index == 0 &&
           d.capture_profile.still.still_image_bundle.members[0].role == CaptureStillImageMemberRole::DEFAULT_METERED &&
           d.capture_profile.still.still_image_bundle.members[0].intended_exposure_compensation_milli_ev == 0;
  });
  if (!default_bundle_ready) {
    std::cerr << "Expected default snapshot still_image_bundle with one DEFAULT_METERED member\n";
    rt.stop();
    return 1;
  }
  auto snap_before_profile = get_last_snapshot(buf);
  const uint64_t topo_before_profile = snap_before_profile ? snap_before_profile->topology_version : 0;

  req.capture_id = 9901;
  if (!prov.trigger_capture(req).ok()) {
    std::cerr << "Expected trigger_capture success for baseline version check\n";
    rt.stop();
    return 1;
  }
  CaptureRequest req_after_trigger{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req_after_trigger) ||
      req_after_trigger.profile_version != v0) {
    std::cerr << "trigger_capture must not change capture_profile_version\n";
    rt.stop();
    return 1;
  }

  CaptureProfile p{};
  p.width = req.width;
  p.height = req.height;
  p.format_fourcc = req.format_fourcc;
  CaptureStillImageBundle s = make_default_metered_still_image_bundle();
  s.members.push_back(CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, -1000});
  s.members.push_back(CaptureStillImageMember{2u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, 1000});

  if (rt.try_set_device_still_capture_profile(kDeviceInstanceId, p, s) != TrySetStillCaptureProfileStatus::OK) {
    std::cerr << "Expected first still profile set success\n";
    rt.stop();
    return 1;
  }
  if (!wait_until([&]() {
        CaptureRequest r{};
        return rt.materialize_capture_request(kDeviceInstanceId, r) && r.profile_version > v0;
      }, 400, 5)) {
    std::cerr << "Expected profile_version increment after first still profile set\n";
    rt.stop();
    return 1;
  }
  CaptureRequest req_after_set{};
  rt.materialize_capture_request(kDeviceInstanceId, req_after_set);
  const uint64_t v1 = req_after_set.profile_version;
  const auto device_three_member_ready = wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
    for (const auto& d : s.devices) {
      if (d.instance_id != kDeviceInstanceId) continue;
      if (d.capture_profile.still.still_image_bundle.members.size() != 3) return false;
      const auto& m0 = d.capture_profile.still.still_image_bundle.members[0];
      const auto& m1 = d.capture_profile.still.still_image_bundle.members[1];
      const auto& m2 = d.capture_profile.still.still_image_bundle.members[2];
      return m0.image_member_index == 0 && m0.role == CaptureStillImageMemberRole::DEFAULT_METERED && m0.intended_exposure_compensation_milli_ev == 0 &&
             m1.image_member_index == 1 && m1.role == CaptureStillImageMemberRole::ADDITIONAL_BRACKET && m1.intended_exposure_compensation_milli_ev == -1000 &&
             m2.image_member_index == 2 && m2.role == CaptureStillImageMemberRole::ADDITIONAL_BRACKET && m2.intended_exposure_compensation_milli_ev == 1000;
    }
    return false;
  });
  if (!device_three_member_ready) {
    std::cerr << "Expected three-member still_image_bundle snapshot on device after profile set\n";
    rt.stop();
    return 1;
  }
  auto snap_after_profile = get_last_snapshot(buf);
  if (snap_after_profile && snap_after_profile->topology_version != topo_before_profile) {
    std::cerr << "topology_version must not change solely on still profile/bundle configuration\n";
    rt.stop();
    return 1;
  }

  if (rt.try_set_device_still_capture_profile(kDeviceInstanceId, p, s) != TrySetStillCaptureProfileStatus::OK) {
    std::cerr << "Expected second identical still profile set success\n";
    rt.stop();
    return 1;
  }
  CaptureRequest req_after_same{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req_after_same) ||
      req_after_same.profile_version != v1) {
    std::cerr << "Identical still profile set must be idempotent for profile_version\n";
    rt.stop();
    return 1;
  }

  CaptureProfile p2 = p;
  p2.width = p.width + 16;
  if (rt.try_set_device_still_capture_profile(kDeviceInstanceId, p2, s) != TrySetStillCaptureProfileStatus::OK) {
    std::cerr << "Expected changed still profile set success\n";
    rt.stop();
    return 1;
  }
  CaptureRequest req_after_changed{};
  if (!wait_until([&]() {
        CaptureRequest r{};
        return rt.materialize_capture_request(kDeviceInstanceId, r) && r.profile_version == v1 + 1;
      }, 400, 5)) {
    std::cerr << "Changed still profile set must increment profile_version once\n";
    rt.stop();
    return 1;
  }
  rt.materialize_capture_request(kDeviceInstanceId, req_after_changed);
  req_after_changed.capture_id = 9902;
  if (!prov.trigger_capture(req_after_changed).ok()) {
    std::cerr << "Expected trigger_capture success after profile change\n";
    rt.stop();
    return 1;
  }
  CaptureRequest req_after_trigger2{};
  if (!rt.materialize_capture_request(kDeviceInstanceId, req_after_trigger2) ||
      req_after_trigger2.profile_version != v1 + 1) {
    std::cerr << "trigger_capture after profile set must not increment profile_version\n";
    rt.stop();
    return 1;
  }
  const auto acquisition_session_bundle_ready = wait_for_snapshot_pred(buf, [&](const CamBANGStateSnapshot& s) {
    for (const auto& a : s.acquisition_sessions) {
      if (a.device_instance_id != kDeviceInstanceId) continue;
      if (a.capture_profile.still.still_image_bundle.members.size() != 3) return false;
      const auto& m0 = a.capture_profile.still.still_image_bundle.members[0];
      const auto& m1 = a.capture_profile.still.still_image_bundle.members[1];
      const auto& m2 = a.capture_profile.still.still_image_bundle.members[2];
      return m0.image_member_index == 0 && m0.role == CaptureStillImageMemberRole::DEFAULT_METERED && m0.intended_exposure_compensation_milli_ev == 0 &&
             m1.image_member_index == 1 && m1.role == CaptureStillImageMemberRole::ADDITIONAL_BRACKET && m1.intended_exposure_compensation_milli_ev == -1000 &&
             m2.image_member_index == 2 && m2.role == CaptureStillImageMemberRole::ADDITIONAL_BRACKET && m2.intended_exposure_compensation_milli_ev == 1000;
    }
    return false;
  });
  if (!acquisition_session_bundle_ready) {
    std::cerr << "Expected three-member still_image_bundle snapshot on acquisition session after capture-context realization\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_rig_cohort_admission_from_preflight_smoke() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (rig cohort admit smoke)\n";
    return 1;
  }
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed (rig cohort admit smoke)\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    std::cerr << "Failed to enumerate endpoints (rig cohort admit smoke)\n";
    rt.stop();
    return 1;
  }
  const std::string live_hw = eps[0].hardware_id;
  if (!rt.smoke_set_rig_member_hardware_ids(8001, {live_hw})) {
    std::cerr << "Failed to set rig members (rig cohort admit smoke)\n";
    rt.stop();
    return 1;
  }
  const auto preflight_ok = rt.preflight_rig_participants_materialize(8001);
  if (!preflight_ok.ok) {
    std::cerr << "Preflight failed unexpectedly (rig cohort admit smoke)\n";
    rt.stop();
    return 1;
  }

  const auto admitted = rt.smoke_admit_rig_cohort_from_preflight(8001, 9001, preflight_ok);
  if (!admitted.ok || admitted.failure != CoreRuntime::RigCohortAdmissionFailure::None ||
      admitted.capture_id != 9001 || admitted.rig_id != 8001 || admitted.participants.size() != 1) {
    std::cerr << "Expected successful cohort admission from preflight\n";
    rt.stop();
    return 1;
  }
  const auto& p = admitted.participants[0];
  if (p.hardware_id != live_hw ||
      p.request.capture_id != 9001 ||
      p.request.rig_id != 8001 ||
      p.request.device_instance_id != kDeviceInstanceId) {
    std::cerr << "Admitted request bundle stamping/traceability mismatch\n";
    rt.stop();
    return 1;
  }

  const auto zero_capture = rt.smoke_admit_rig_cohort_from_preflight(8001, 0, preflight_ok);
  if (zero_capture.ok || zero_capture.failure != CoreRuntime::RigCohortAdmissionFailure::InvalidCaptureId) {
    std::cerr << "Expected InvalidCaptureId for capture_id=0\n";
    rt.stop();
    return 1;
  }

  const auto dup_capture = rt.smoke_admit_rig_cohort_from_preflight(8001, 9001, preflight_ok);
  if (dup_capture.ok || dup_capture.failure != CoreRuntime::RigCohortAdmissionFailure::DuplicateCaptureId) {
    std::cerr << "Expected DuplicateCaptureId on second insert\n";
    rt.stop();
    return 1;
  }

  const auto bad_preflight = rt.preflight_rig_participants_materialize(9999);
  if (bad_preflight.ok) {
    std::cerr << "Expected missing-rig preflight failure\n";
    rt.stop();
    return 1;
  }
  const auto admit_bad_preflight = rt.smoke_admit_rig_cohort_from_preflight(9999, 9002, bad_preflight);
  if (admit_bad_preflight.ok || admit_bad_preflight.failure != CoreRuntime::RigCohortAdmissionFailure::PreflightFailed) {
    std::cerr << "Expected PreflightFailed when admitting failed preflight\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_rig_bundle_submission_smoke() {
  CoreRuntime rt;
  if (!rt.start()) {
    std::cerr << "CoreRuntime failed to start (rig submit smoke)\n";
    return 1;
  }
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "Stub provider setup failed (rig submit smoke)\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    std::cerr << "Failed to enumerate endpoints (rig submit smoke)\n";
    rt.stop();
    return 1;
  }
  const std::string live_hw = eps[0].hardware_id;
  if (!rt.smoke_set_rig_member_hardware_ids(8101, {live_hw})) {
    std::cerr << "Failed to set rig members (rig submit smoke)\n";
    rt.stop();
    return 1;
  }

  const auto preflight_ok = rt.preflight_rig_participants_materialize(8101);
  const auto admitted_ok = rt.smoke_admit_rig_cohort_from_preflight(8101, 9101, preflight_ok);
  if (!admitted_ok.ok) {
    std::cerr << "Failed to admit cohort for submit smoke success path\n";
    rt.stop();
    return 1;
  }
  const auto submit_ok = rt.smoke_submit_admitted_rig_bundle(admitted_ok);
  if (!submit_ok.ok || submit_ok.submitted_count != admitted_ok.participants.size()) {
    std::cerr << "Expected successful participant submissions\n";
    rt.stop();
    return 1;
  }
  if (admitted_ok.participants[0].request.still_image_bundle.members.size() != 1 ||
      admitted_ok.participants[0].request.still_image_bundle.members[0].image_member_index != 0 ||
      admitted_ok.participants[0].request.still_image_bundle.members[0].role != CaptureStillImageMemberRole::DEFAULT_METERED) {
    std::cerr << "Expected one-member default-metered sequence in admitted request\n";
    rt.stop();
    return 1;
  }

  // Multi-image request rejected when provider lacks multi-image capability.
  auto multi_member_invalid = admitted_ok;
  multi_member_invalid.capture_id = 9104;
  multi_member_invalid.participants[0].request.capture_id = 9104;
  multi_member_invalid.participants[0].request.still_image_bundle.members.push_back(
      CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET});
  if (!rt.smoke_admit_rig_cohort_from_preflight(8101, 9104, preflight_ok).ok) {
    std::cerr << "Failed to admit cohort for multi-image rejection case\n";
    rt.stop();
    return 1;
  }
  const auto multi_member_fail = rt.smoke_submit_admitted_rig_bundle(multi_member_invalid);
  if (multi_member_fail.ok ||
      multi_member_fail.failure != CoreRuntime::RigSubmissionFailure::TriggerFailed ||
      multi_member_fail.provider_error_code != static_cast<uint32_t>(ProviderError::ERR_INVALID_ARGUMENT)) {
    std::cerr << "Expected deterministic rejection for two-member sequence without provider support\n";
    rt.stop();
    return 1;
  }

  // First participant synchronous failure: invalid device id in first entry.
  auto bad_first = admitted_ok;
  bad_first.capture_id = 9102;
  bad_first.participants[0].request.capture_id = 9102;
  bad_first.participants[0].request.device_instance_id = 999999;
  if (!rt.smoke_admit_rig_cohort_from_preflight(8101, 9102, preflight_ok).ok) {
    std::cerr << "Failed to admit cohort for first-fail case\n";
    rt.stop();
    return 1;
  }
  const auto first_fail = rt.smoke_submit_admitted_rig_bundle(bad_first);
  if (first_fail.ok || first_fail.failure != CoreRuntime::RigSubmissionFailure::TriggerFailed ||
      first_fail.submitted_count != 0 || first_fail.failed_index != 0) {
    std::cerr << "Expected first participant trigger failure\n";
    rt.stop();
    return 1;
  }

  // Later participant failure after earlier success (duplicate first participant).
  auto two_part = admitted_ok;
  two_part.capture_id = 9103;
  two_part.participants[0].request.capture_id = 9103;
  CoreRuntime::RigAdmittedParticipantRequest second = two_part.participants[0];
  second.request.capture_id = 9103;
  second.request.device_instance_id = 888888;
  two_part.participants.push_back(second);
  if (!rt.smoke_admit_rig_cohort_from_preflight(8101, 9103, preflight_ok).ok) {
    std::cerr << "Failed to admit cohort for later-fail case\n";
    rt.stop();
    return 1;
  }
  const auto later_fail = rt.smoke_submit_admitted_rig_bundle(two_part);
  if (later_fail.ok || later_fail.failure != CoreRuntime::RigSubmissionFailure::TriggerFailed ||
      later_fail.submitted_count != 1 || later_fail.failed_index != 1) {
    std::cerr << "Expected second participant trigger failure after first success\n";
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_cohort_aware_capture_result_set_smoke() {
  CoreRuntime rt;
  if (!rt.start()) return 1;
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) { rt.stop(); return 1; }
  rt.attach_provider(&prov);

  auto emit_capture = [&](uint64_t capture_id, uint64_t device_id, uint8_t fill) {
    static std::vector<uint8_t> bytes(2 * 2 * 4, 0);
    std::fill(bytes.begin(), bytes.end(), fill);
    FrameView frame{};
    frame.capture_id = capture_id;
    frame.device_instance_id = device_id;
    frame.stream_id = 0;
    frame.width = 2;
    frame.height = 2;
    frame.format_fourcc = FOURCC_RGBA;
    frame.data = bytes.data();
    frame.size_bytes = bytes.size();
    frame.stride_bytes = 0;
    frame.release = [](void*, const FrameView*) {};
    frame.release_user = nullptr;
    rt.provider_callbacks()->on_capture_started(capture_id, device_id);
    rt.provider_callbacks()->on_frame(frame);
    rt.provider_callbacks()->on_capture_completed(capture_id, device_id);
  };

  // No cohort path: accept-all assembly-successful candidates.
  emit_capture(9201, 100, 1);
  emit_capture(9201, 101, 2);
  if (!wait_until([&]() { return rt.get_capture_result_set(9201).size() == 2; }, 400, 5)) {
    rt.stop();
    return 1;
  }

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) { rt.stop(); return 1; }
  if (!rt.smoke_set_rig_member_hardware_ids(8201, {eps[0].hardware_id})) { rt.stop(); return 1; }
  const auto preflight = rt.preflight_rig_participants_materialize(8201);
  const auto admitted = rt.smoke_admit_rig_cohort_from_preflight(8201, 9202, preflight);
  if (!admitted.ok) { rt.stop(); return 1; }

  // Cohort OPEN but incomplete => empty.
  if (!rt.get_capture_result_set(9202).empty()) { rt.stop(); return 1; }

  // Emit expected participant + extra successful non-expected participant.
  emit_capture(9202, admitted.participants[0].request.device_instance_id, 3);
  emit_capture(9202, 4242, 4);
  if (!wait_until([&]() { return rt.get_capture_result_set(9202).size() == 1; }, 400, 5)) {
    rt.stop();
    return 1;
  }
  auto cohort_set = rt.get_capture_result_set(9202);
  if (cohort_set.size() != 1 ||
      cohort_set[0]->device_instance_id != admitted.participants[0].request.device_instance_id) {
    rt.stop();
    return 1;
  }

  // FAILED cohort => empty.
  rt.smoke_submit_admitted_rig_bundle(admitted); // may succeed; keep cohort OPEN
  // Force a new failed cohort via bad submit
  const auto admitted_fail = rt.smoke_admit_rig_cohort_from_preflight(8201, 9203, preflight);
  auto bad = admitted_fail;
  bad.participants[0].request.device_instance_id = 999999;
  (void)rt.smoke_submit_admitted_rig_bundle(bad);
  if (!rt.get_capture_result_set(9203).empty()) { rt.stop(); return 1; }

  rt.stop();
  return 0;
}


static int test_server_facing_rig_orchestration_adapter_smoke() {
  CoreRuntime rt;
  if (!rt.start()) return 1;
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) { rt.stop(); return 1; }
  rt.attach_provider(&prov);

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) { rt.stop(); return 1; }
  const std::string live_hw = eps[0].hardware_id;

  // Simulate CamBANGServer allocator shape: monotonic counter, skip 0.
  uint64_t next_capture_id = 1;
  auto allocate_capture_id = [&]() {
    uint64_t capture_id = next_capture_id++;
    if (capture_id == 0) {
      capture_id = next_capture_id++;
    }
    return capture_id;
  };

  if (!rt.smoke_set_rig_member_hardware_ids(8401, {live_hw})) { rt.stop(); return 1; }

  const uint64_t ok_capture_id = allocate_capture_id();
  const auto success = rt.orchestrate_rig_capture_with_capture_id_for_server(8401, ok_capture_id);
  if (!success.ok || success.capture_id != ok_capture_id || success.submitted_count != 1) {
    rt.stop();
    return 1;
  }

  if (!wait_until([&]() { return rt.get_capture_result_set(ok_capture_id).size() == 1; }, 400, 5)) {
    rt.stop();
    return 1;
  }

  const auto set = rt.get_capture_result_set(ok_capture_id);
  if (set.size() != 1 || !set[0] || set[0]->capture_id != ok_capture_id) {
    rt.stop();
    return 1;
  }
  if (!set[0]->additional_images.empty()) {
    rt.stop();
    return 1;
  }

  // Failure path: missing rig should fail and should not expose cohort result set.
  const uint64_t bad_capture_id = allocate_capture_id();
  const auto fail = rt.orchestrate_rig_capture_with_capture_id_for_server(999991, bad_capture_id);
  if (fail.ok || fail.failure != CoreRuntime::RigOrchestrationFailure::PreflightFailed) {
    rt.stop();
    return 1;
  }
  if (!rt.get_capture_result_set(bad_capture_id).empty()) {
    rt.stop();
    return 1;
  }

  rt.stop();
  return 0;
}

static int test_rig_orchestration_helper_smoke() {
  CoreRuntime rt;
  if (!rt.start()) return 1;
  StubProvider prov;
  if (!setup_one_stream(rt, prov)) { rt.stop(); return 1; }
  rt.attach_provider(&prov);

  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) { rt.stop(); return 1; }
  const std::string live_hw = eps[0].hardware_id;

  // Preflight failure: missing rig, no cohort/provider submit.
  const auto preflight_fail = rt.smoke_orchestrate_rig_capture_with_capture_id(9901, 9301);
  if (preflight_fail.ok || preflight_fail.failure != CoreRuntime::RigOrchestrationFailure::PreflightFailed) {
    rt.stop();
    return 1;
  }

  if (!rt.smoke_set_rig_member_hardware_ids(8301, {live_hw})) { rt.stop(); return 1; }

  // Invalid capture_id.
  const auto bad_capture_id = rt.smoke_orchestrate_rig_capture_with_capture_id(8301, 0);
  if (bad_capture_id.ok || bad_capture_id.failure != CoreRuntime::RigOrchestrationFailure::InvalidCaptureId) {
    rt.stop();
    return 1;
  }

  // Success.
  const auto success = rt.smoke_orchestrate_rig_capture_with_capture_id(8301, 9302);
  if (!success.ok || success.capture_id != 9302 || success.submitted_count != 1) {
    rt.stop();
    return 1;
  }
  const auto admission_fail = rt.smoke_orchestrate_rig_capture_with_capture_id(8301, 9302);
  if (admission_fail.ok || admission_fail.failure != CoreRuntime::RigOrchestrationFailure::AdmissionFailed) {
    rt.stop();
    return 1;
  }

  // Submission failure leaves cohort failed.
  if (!rt.smoke_set_rig_member_hardware_ids(8302, {live_hw, live_hw})) { rt.stop(); return 1; }
  // keep preflight/admission valid by using one member then mutate device in submit path via bad endpoint
  if (!rt.smoke_set_rig_member_hardware_ids(8302, {live_hw})) { rt.stop(); return 1; }
  auto preflight = rt.preflight_rig_participants_materialize(8302);
  auto admitted = rt.smoke_admit_rig_cohort_from_preflight(8302, 9303, preflight);
  admitted.participants[0].request.device_instance_id = 777777;
  const auto submit_fail = rt.smoke_submit_admitted_rig_bundle(admitted);
  if (submit_fail.ok) { rt.stop(); return 1; }
  if (!rt.get_capture_result_set(9303).empty()) { rt.stop(); return 1; }

  // Orchestration-level submission failure case.
  if (!rt.smoke_set_rig_member_hardware_ids(8303, {"missing:hw"})) { rt.stop(); return 1; }
  const auto orchestration_fail = rt.smoke_orchestrate_rig_capture_with_capture_id(8303, 9304);
  if (orchestration_fail.ok || orchestration_fail.failure != CoreRuntime::RigOrchestrationFailure::PreflightFailed) {
    rt.stop();
    return 1;
  }

  rt.stop();
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

  StubProvider prov;
  if (!setup_one_stream(rt, prov)) {
    std::cerr << "[iter " << iter_index << "] Stub provider setup failed\n";
    rt.stop();
    return 1;
  }
  rt.attach_provider(&prov);

  maybe_jitter(opt, rng);

  const StreamRequest req = make_req();
  if (!prov.start_stream(kStreamId, req.profile, req.picture).ok()) {
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

  std::cout << "[smoke] compiled provider: " << compiled_provider_name() << "\n";

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
    StubProvider prov;
    if (int r = test_baseline_live_one_frame_and_snapshot(rt, buf, prov)) return r;
    if (int r = test_overload_queuefull_release_accounting(rt, prov)) return r;
    if (int r = test_shutdown_choreography(rt, prov)) return r;
    if (int r = test_device_capture_request_materialization_smoke()) return r;
    if (int r = test_open_device_snapshot_retains_default_capture_profile_smoke()) return r;
    if (int r = test_still_capture_profile_version_idempotency_smoke(buf)) return r;
    if (int r = test_rig_preflight_materialization_smoke()) return r;
    if (int r = test_rig_cohort_admission_from_preflight_smoke()) return r;
    if (int r = test_rig_bundle_submission_smoke()) return r;
    if (int r = test_cohort_aware_capture_result_set_smoke()) return r;
    if (int r = test_rig_orchestration_helper_smoke()) return r;
    if (int r = test_server_facing_rig_orchestration_adapter_smoke()) return r;
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
