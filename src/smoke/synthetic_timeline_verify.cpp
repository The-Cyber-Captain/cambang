/*
CamBANG Maintainer Utility

Tool: synthetic_timeline_verify

Purpose
-------
Deterministically verifies SyntheticProvider Timeline behaviour and Core registry
truth using scheduled verification-case events and virtual time.

This utility validates lifecycle reporting, FrameProducer tracking, capture
timestamp semantics, and event ordering. It is intended for maintainer
verification and CI regression checks.

Category
--------
Verification tool (maintainer/CI).

Non-Goals
---------
- Not a performance benchmark
- Not a user-facing test harness (Godot)
- Not a platform backend validator
*/

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "synthetic_timeline_verify: build with -DCAMBANG_INTERNAL_SMOKE=1 (via SCons: smoke=1)."
#endif

#include "core/core_runtime.h"
#include "core/state_snapshot_buffer.h"
#include "core/snapshot/state_snapshot.h"

#include "imaging/synthetic/provider.h"

using namespace cambang;

namespace {

constexpr uint64_t kDeviceInstanceId = 1;
constexpr uint64_t kRootId = 1;
constexpr uint64_t kStreamId = 1;

constexpr uint64_t kOneSecNs = 1'000'000'000ull;

struct Options {
  std::string verify_case = "basic_lifecycle";
  bool dump_snapshots = false;
};

static void usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [--verify_case=<name>] [--dump_snapshots]\n"
      << "Verification cases:\n"
      << "  basic_lifecycle\n"
      << "  invalid_sequence\n"
      << "  catchup_stress\n"
      << "Compatibility: --scenario=<name> is accepted as a legacy alias.\n";
}

static bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

static bool parse_opts(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return false;
    }
    if (a == "--dump_snapshots") {
      opt.dump_snapshots = true;
      continue;
    }
    if (starts_with(a, "--verify_case=")) {
      opt.verify_case = a.substr(std::string("--verify_case=").size());
      continue;
    }
    if (starts_with(a, "--scenario=")) {
      opt.verify_case = a.substr(std::string("--scenario=").size());
      continue;
    }
    std::cerr << "Unknown arg: " << a << "\n";
    usage(argv[0]);
    return false;
  }
  return true;
}

static bool wait_until(const std::function<bool()>& pred, int max_iters = 400, int sleep_ms = 2) {
  for (int i = 0; i < max_iters; ++i) {
    if (pred()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
  }
  return false;
}

static std::shared_ptr<const CamBANGStateSnapshot> snapshot_copy(StateSnapshotBuffer& buf) {
  return buf.snapshot_copy();
}

static void dump_snapshot(const CamBANGStateSnapshot& s) {
  std::cout << "[snap] gen=" << s.gen << " ver=" << s.version << " topo=" << s.topology_version << " ts=" << s.timestamp_ns
            << " devices=" << s.devices.size() << " acquisition_sessions=" << s.acquisition_sessions.size()
            << " streams=" << s.streams.size()
            << " native_objects=" << s.native_objects.size() << "\n";
  for (const auto& acq : s.acquisition_sessions) {
    std::cout << "  acquisition_session id=" << acq.acquisition_session_id
              << " device_instance_id=" << acq.device_instance_id
              << " phase=" << static_cast<int>(acq.phase)
              << "\n";
  }
  for (const auto& st : s.streams) {
    std::cout << "  stream id=" << st.stream_id
              << " phase=" << static_cast<int>(st.phase)
              << " recv=" << st.frames_received
              << " delivered=" << st.frames_delivered
              << " dropped=" << st.frames_dropped
              << " last_ts=" << st.last_frame_ts_ns
              << "\n";
  }
  for (const auto& no : s.native_objects) {
    std::cout << "  native id=" << no.native_id
              << " type=" << no.type
              << " phase=" << static_cast<int>(no.phase)
              << " owner_acquisition_session=" << no.owner_acquisition_session_id
              << " owner_stream=" << no.owner_stream_id
              << " created=" << no.created_ns
              << " destroyed=" << no.destroyed_ns
              << "\n";
  }
}

static bool assert_unique_native_ids(const CamBANGStateSnapshot& s) {
  std::set<uint64_t> ids;
  for (const auto& no : s.native_objects) {
    if (no.native_id == 0) {
      std::cerr << "FAIL: native_id 0 present in snapshot\n";
      return false;
    }
    if (!ids.insert(no.native_id).second) {
      std::cerr << "FAIL: duplicate native_id in snapshot: " << no.native_id << "\n";
      return false;
    }
  }
  return true;
}

static bool has_live_frame_producer_for_stream(const CamBANGStateSnapshot& s, uint64_t stream_id) {
  for (const auto& no : s.native_objects) {
    if (no.owner_stream_id != stream_id) continue;
    if (no.type != static_cast<uint32_t>(NativeObjectType::FrameProducer)) continue;
    if (no.destroyed_ns == 0) return true;
  }
  return false;
}

static bool has_acquisition_session_for_device(const CamBANGStateSnapshot& s, uint64_t device_instance_id) {
  for (const auto& acq : s.acquisition_sessions) {
    if (acq.device_instance_id == device_instance_id) {
      return true;
    }
  }
  return false;
}

static uint64_t frames_received_for_stream(const CamBANGStateSnapshot& s, uint64_t stream_id) {
  for (const auto& st : s.streams) {
    if (st.stream_id == stream_id) return st.frames_received;
  }
  return 0;
}

static bool stream_is_flowing(const CamBANGStateSnapshot& s, uint64_t stream_id) {
  for (const auto& st : s.streams) {
    if (st.stream_id == stream_id) {
      return st.mode == CBStreamMode::FLOWING;
    }
  }
  return false;
}

static bool stream_exists(const CamBANGStateSnapshot& s, uint64_t stream_id) {
  for (const auto& st : s.streams) {
    if (st.stream_id == stream_id) {
      return true;
    }
  }
  return false;
}

static bool wait_for_frames(StateSnapshotBuffer& buf, uint64_t stream_id, uint64_t min_frames, bool dump) {
  return wait_until([&]() {
    auto s = snapshot_copy(buf);
    if (!s) return false;
    if (dump) dump_snapshot(*s);
    return frames_received_for_stream(*s, stream_id) >= min_frames;
  });
}

static bool wait_for_pred(StateSnapshotBuffer& buf, const std::function<bool(const CamBANGStateSnapshot&)>& pred, bool dump) {
  return wait_until([&]() {
    auto s = snapshot_copy(buf);
    if (!s) return false;
    if (dump) dump_snapshot(*s);
    return pred(*s);
  });
}

static uint64_t fps_period_ns(uint32_t fps_num, uint32_t fps_den) {
  if (fps_num == 0 || fps_den == 0) return 0;
  return (kOneSecNs * static_cast<uint64_t>(fps_den)) / static_cast<uint64_t>(fps_num);
}

// Drive the synthetic virtual clock deterministically.
static void tick_synthetic(CoreRuntime& rt, uint64_t dt_ns) {
  auto* p = rt.attached_provider();
  auto* syn = dynamic_cast<SyntheticProvider*>(p);
  if (!syn) {
    std::cerr << "FAIL: attached provider is not SyntheticProvider\n";
    std::exit(2);
  }
  syn->advance(dt_ns);
}

static int run_basic_lifecycle(CoreRuntime& rt, StateSnapshotBuffer& buf, const Options& opt, uint64_t period) {
  // Create + start (core-initiated).
  {
    const auto cs = rt.try_create_stream(kStreamId, kDeviceInstanceId, StreamIntent::PREVIEW, nullptr, nullptr, 1);
    if (cs != TryCreateStreamStatus::OK) {
      std::cerr << "FAIL: try_create_stream not enqueued\n";
      return 1;
    }
    const auto ss = rt.try_start_stream(kStreamId);
    if (ss != TryStartStreamStatus::OK) {
      std::cerr << "FAIL: try_start_stream not enqueued\n";
      return 1;
    }
  }

  // Wait until a live FrameProducer exists.
  if (!wait_for_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return has_live_frame_producer_for_stream(s, kStreamId) &&
               has_acquisition_session_for_device(s, kDeviceInstanceId);
      }, opt.dump_snapshots)) {
    std::cerr << "FAIL: FrameProducer/AcquisitionSession seam not observed after start\n";
    return 1;
  }

  // Emit 5 frames (virtual time ticks).
  const uint64_t want_frames = 5;
  for (uint64_t i = 0; i < want_frames; ++i) {
    tick_synthetic(rt, period);
    rt.request_publish();
  }

  if (!wait_for_frames(buf, kStreamId, want_frames, opt.dump_snapshots)) {
    std::cerr << "FAIL: expected frames_received >= " << want_frames << "\n";
    return 1;
  }

  // Stop + destroy.
  {
    const auto st = rt.try_stop_stream(kStreamId);
    if (st != TryStopStreamStatus::OK) {
      std::cerr << "FAIL: try_stop_stream not enqueued\n";
      return 1;
    }
    const auto ds = rt.try_destroy_stream(kStreamId);
    if (ds != TryDestroyStreamStatus::OK) {
      std::cerr << "FAIL: try_destroy_stream not enqueued\n";
      return 1;
    }
  }

  // Assert FrameProducer is no longer live.
  if (!wait_for_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return !has_live_frame_producer_for_stream(s, kStreamId) &&
               !has_acquisition_session_for_device(s, kDeviceInstanceId);
      }, opt.dump_snapshots)) {
    std::cerr << "FAIL: FrameProducer/AcquisitionSession seam still visible after stop+destroy\n";
    return 1;
  }

  // Assert native_ids are unique.
  auto s = snapshot_copy(buf);
  if (!s) {
    std::cerr << "FAIL: no snapshot\n";
    return 1;
  }
  if (!assert_unique_native_ids(*s)) return 1;

  return 0;
}

static int run_invalid_sequence(CoreRuntime& rt, StateSnapshotBuffer& buf, const Options& opt) {
  // Create stream only.
  {
    const auto cs = rt.try_create_stream(kStreamId, kDeviceInstanceId, StreamIntent::PREVIEW, nullptr, nullptr, 1);
    if (cs != TryCreateStreamStatus::OK) {
      std::cerr << "FAIL: try_create_stream not enqueued\n";
      return 1;
    }
  }

  // Attempt stop without start.
  const auto st = rt.try_stop_stream(kStreamId);
  if (st != TryStopStreamStatus::OK) {
    // If core rejects immediately (e.g. Busy/BadState), that's also acceptable.
    std::cerr << "NOTE: try_stop_stream returned " << static_cast<int>(st) << " (acceptable)\n";
  }

  rt.request_publish();

  // Assert no live FrameProducer exists and AcquisitionSession seam is present
  // for create_stream-only (stream exists but not started).
  if (!wait_for_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return !has_live_frame_producer_for_stream(s, kStreamId) &&
               has_acquisition_session_for_device(s, kDeviceInstanceId);
      }, opt.dump_snapshots)) {
    std::cerr << "FAIL: invalid-sequence seam expectations failed (FrameProducer/AcquisitionSession)\n";
    return 1;
  }

  // Cleanup.
  (void)rt.try_destroy_stream(kStreamId);
  rt.request_publish();
  if (!wait_for_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return !stream_exists(s, kStreamId) &&
               !has_acquisition_session_for_device(s, kDeviceInstanceId);
      }, opt.dump_snapshots)) {
    std::cerr << "FAIL: AcquisitionSession seam not cleared after invalid-sequence destroy\n";
    return 1;
  }
  return 0;
}

static int run_catchup_stress(CoreRuntime& rt, StateSnapshotBuffer& buf, const Options& opt, uint64_t period) {
  {
    const auto cs = rt.try_create_stream(kStreamId, kDeviceInstanceId, StreamIntent::PREVIEW, nullptr, nullptr, 1);
    if (cs != TryCreateStreamStatus::OK) {
      std::cerr << "FAIL: try_create_stream not enqueued\n";
      return 1;
    }
    const auto ss = rt.try_start_stream(kStreamId);
    if (ss != TryStartStreamStatus::OK) {
      std::cerr << "FAIL: try_start_stream not enqueued\n";
      return 1;
    }
  }

  // Ensure stream-start provider/core integration is visible before advancing
  // virtual time for catch-up. Advancing too early can schedule against a stream
  // that has not transitioned to FLOWING yet.
  if (!wait_until([&]() {
        rt.request_publish();
        auto s = snapshot_copy(buf);
        if (!s) return false;
        if (opt.dump_snapshots) dump_snapshot(*s);
        return stream_is_flowing(*s, kStreamId) &&
               has_live_frame_producer_for_stream(*s, kStreamId) &&
               has_acquisition_session_for_device(*s, kDeviceInstanceId);
      })) {
    std::cerr << "FAIL: stream did not reach FLOWING with expected FrameProducer/AcquisitionSession seam before catch-up tick\n";
    return 1;
  }

  // One big tick: catch-up pump.
  tick_synthetic(rt, kOneSecNs);
  rt.request_publish();

  // Expected frames in 1s with first frame at t=0: floor(1s/period)+1.
  const uint64_t expected = (period == 0) ? 0 : (kOneSecNs / period) + 1;
  if (!wait_until([&]() {
        rt.request_publish();
        auto s = snapshot_copy(buf);
        if (!s) return false;
        if (opt.dump_snapshots) dump_snapshot(*s);
        return frames_received_for_stream(*s, kStreamId) >= expected;
      }, 800, 2)) {
    std::cerr << "FAIL: expected frames_received >= " << expected << " after catch-up tick\n";
    return 1;
  }

  auto s = snapshot_copy(buf);
  if (!s) {
    std::cerr << "FAIL: no snapshot\n";
    return 1;
  }
  const uint64_t got = frames_received_for_stream(*s, kStreamId);
  if (got != expected) {
    std::cerr << "FAIL: expected frames_received == " << expected << " got " << got << "\n";
    return 1;
  }

  // Cleanup: drive stop/destroy deterministically until stream disappears.
  if (!wait_until([&]() {
        (void)rt.try_stop_stream(kStreamId);
        (void)rt.try_destroy_stream(kStreamId);
        rt.request_publish();
        auto s2 = snapshot_copy(buf);
        if (!s2) return false;
        return !stream_exists(*s2, kStreamId);
      }, 800, 2)) {
    std::cerr << "FAIL: catchup cleanup did not fully destroy stream\n";
    return 1;
  }

  return 0;
}

} // namespace

int main(int argc, char** argv) {
  Options opt;
  if (!parse_opts(argc, argv, opt)) {
    return 2;
  }

  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);

  if (!rt.start()) {
    std::cerr << "FAIL: core runtime did not start\n";
    return 2;
  }

  // SyntheticProvider configured for Timeline role and VirtualTime driver.
  SyntheticProviderConfig cfg{};
  cfg.synthetic_role = SyntheticRole::Timeline;
  cfg.timing_driver = TimingDriver::VirtualTime;

  // Keep verify runs cheap.
  cfg.nominal.width = 320;
  cfg.nominal.height = 180;
  cfg.nominal.fps_num = 30;
  cfg.nominal.fps_den = 1;
  cfg.pattern.preset = PatternPreset::XyXor;
  cfg.pattern.seed = 1;

  SyntheticProvider prov(cfg);
  if (!prov.initialize(rt.provider_callbacks()).ok()) {
    std::cerr << "FAIL: synthetic provider initialize failed\n";
    rt.stop();
    return 2;
  }

  rt.attach_provider(&prov);

  // Open the first endpoint deterministically.
  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    std::cerr << "FAIL: enumerate_endpoints failed\n";
    rt.stop();
    return 2;
  }
  rt.retain_device_identity(kDeviceInstanceId, eps[0].hardware_id);
  if (!prov.open_device(eps[0].hardware_id, kDeviceInstanceId, kRootId).ok()) {
    std::cerr << "FAIL: open_device failed\n";
    rt.stop();
    return 2;
  }

  // Prime a publish so the snapshot pipeline is live.
  rt.request_publish();
  (void)wait_until([&]() { return snapshot_copy(buf) != nullptr; }, 200, 2);

  const uint64_t period = fps_period_ns(cfg.nominal.fps_num, cfg.nominal.fps_den);
  if (period == 0) {
    std::cerr << "FAIL: invalid fps period\n";
    rt.stop();
    return 2;
  }

  int r = 0;
  if (opt.verify_case == "basic_lifecycle") {
    r = run_basic_lifecycle(rt, buf, opt, period);
  } else if (opt.verify_case == "invalid_sequence") {
    r = run_invalid_sequence(rt, buf, opt);
  } else if (opt.verify_case == "catchup_stress") {
    r = run_catchup_stress(rt, buf, opt, period);
  } else {
    std::cerr << "Unknown verification case: " << opt.verify_case << "\n";
    usage(argv[0]);
    r = 2;
  }

  // Provider shutdown choreography.
  (void)prov.close_device(kDeviceInstanceId);
  (void)prov.shutdown();

  rt.attach_provider(nullptr);
  rt.stop();

  if (r == 0) {
    std::cout << "OK: synthetic_timeline_verify passed (verify_case=" << opt.verify_case << ")\n";
  }
  return r;
}
