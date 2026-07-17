/*
CamBANG Maintainer Utility

Tool: synthetic_timeline_verify

Purpose
-------
Deterministically verifies SyntheticProvider Timeline behaviour and Core registry
truth using scheduled verification-case events and virtual time.

This utility validates lifecycle reporting, continuity tracking, capture
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
#include <cstdio>
#include <future>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <ext/stdio_filebuf.h>
#endif

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "synthetic_timeline_verify: build through the repo SCons maintainer_tools alias so CAMBANG_INTERNAL_SMOKE=1 is defined."
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

enum class ParseOptsResult {
  Ok,
  Help,
  Error,
};

class QuietOutputCapture final {
public:
  QuietOutputCapture() = default;
  QuietOutputCapture(const QuietOutputCapture&) = delete;
  QuietOutputCapture& operator=(const QuietOutputCapture&) = delete;

  bool begin() {
#if !defined(_WIN32)
    return false;
#else
    std::fflush(stdout);
    std::fflush(stderr);
    saved_stdout_fd_ = _dup(_fileno(stdout));
    saved_stderr_fd_ = _dup(_fileno(stderr));
    if (saved_stdout_fd_ < 0 || saved_stderr_fd_ < 0) {
      restore_saved_fds_();
      return false;
    }
    stdout_file_.reset(std::tmpfile());
    stderr_file_.reset(std::tmpfile());
    if (!stdout_file_ || !stderr_file_) {
      end();
      return false;
    }
    if (_dup2(_fileno(stdout_file_.get()), _fileno(stdout)) != 0 ||
        _dup2(_fileno(stderr_file_.get()), _fileno(stderr)) != 0) {
      end();
      return false;
    }
    // The _dup2() redirection above is necessary and sufficient for raw C
    // stdio (printf/fprintf(stdout|stderr, ...)) but is NOT sufficient for
    // std::cout/std::cerr on this toolchain: empirically, sync_with_stdio's
    // association with fd 1/2 does not track a _dup2() swap performed after
    // process start -- writes through std::cout/std::cerr during the capture
    // window were silently lost (or, worse, misrouted into the other
    // stream's captured file) rather than ending up in stdout_file_/
    // stderr_file_. Redirect cout/cerr explicitly at the streambuf level
    // instead, bypassing whatever sync_with_stdio is (or isn't) doing.
    stdout_filebuf_ = std::make_unique<__gnu_cxx::stdio_filebuf<char>>(stdout_file_.get(), std::ios::out);
    stderr_filebuf_ = std::make_unique<__gnu_cxx::stdio_filebuf<char>>(stderr_file_.get(), std::ios::out);
    saved_cout_buf_ = std::cout.rdbuf(stdout_filebuf_.get());
    saved_cerr_buf_ = std::cerr.rdbuf(stderr_filebuf_.get());
    active_ = true;
    return true;
#endif
  }

  void end() {
    if (!active_) {
      restore_saved_fds_();
      stdout_file_.reset();
      stderr_file_.reset();
      return;
    }
#if defined(_WIN32)
    // Flush and detach cout/cerr from the tmpfile-backed streambufs BEFORE
    // touching the fds those buffers wrap, so no buffered C++-stream content
    // is lost when the fds get redirected back to the real console below.
    std::cout.flush();
    std::cerr.flush();
    if (saved_cout_buf_) {
      std::cout.rdbuf(saved_cout_buf_);
      saved_cout_buf_ = nullptr;
    }
    if (saved_cerr_buf_) {
      std::cerr.rdbuf(saved_cerr_buf_);
      saved_cerr_buf_ = nullptr;
    }
    stdout_filebuf_.reset();
    stderr_filebuf_.reset();
#endif
    std::fflush(stdout);
    std::fflush(stderr);
#if defined(_WIN32)
    if (saved_stdout_fd_ >= 0) {
      (void)_dup2(saved_stdout_fd_, _fileno(stdout));
    }
    if (saved_stderr_fd_ >= 0) {
      (void)_dup2(saved_stderr_fd_, _fileno(stderr));
    }
#endif
    restore_saved_fds_();
    active_ = false;
  }

  std::string captured_stdout() const { return read_file_(stdout_file_.get()); }
  std::string captured_stderr() const { return read_file_(stderr_file_.get()); }

  ~QuietOutputCapture() {
    end();
  }

private:
  struct FileCloser {
    void operator()(FILE* f) const noexcept {
      if (f) {
        std::fclose(f);
      }
    }
  };

  static std::string read_file_(FILE* f) {
    if (!f) {
      return {};
    }
    std::fflush(f);
    if (std::fseek(f, 0, SEEK_END) != 0) {
      return {};
    }
    const long size = std::ftell(f);
    if (size <= 0) {
      std::rewind(f);
      return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    std::rewind(f);
    const size_t read = std::fread(out.data(), 1, out.size(), f);
    out.resize(read);
    return out;
  }

  void restore_saved_fds_() noexcept {
#if defined(_WIN32)
    if (saved_stdout_fd_ >= 0) {
      _close(saved_stdout_fd_);
      saved_stdout_fd_ = -1;
    }
    if (saved_stderr_fd_ >= 0) {
      _close(saved_stderr_fd_);
      saved_stderr_fd_ = -1;
    }
#endif
  }

  bool active_ = false;
  int saved_stdout_fd_ = -1;
  int saved_stderr_fd_ = -1;
  std::unique_ptr<FILE, FileCloser> stdout_file_{};
  std::unique_ptr<FILE, FileCloser> stderr_file_{};
#if defined(_WIN32)
  std::unique_ptr<__gnu_cxx::stdio_filebuf<char>> stdout_filebuf_{};
  std::unique_ptr<__gnu_cxx::stdio_filebuf<char>> stderr_filebuf_{};
  std::streambuf* saved_cout_buf_ = nullptr;
  std::streambuf* saved_cerr_buf_ = nullptr;
#endif
};

static void usage(const char* argv0) {
  std::cerr
      << "Usage: " << argv0 << " [--verify_case=<name>] [--dump_snapshots]\n"
      << "Verification cases:\n"
      << "  basic_lifecycle\n"
      << "  invalid_sequence\n"
      << "  catchup_stress_uncapped\n"
      << "  one_active_stream_admission\n"
      << "  staged_endpoint_span_inference\n";
}

static bool starts_with(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

static ParseOptsResult parse_opts(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      usage(argv[0]);
      return ParseOptsResult::Help;
    }
    if (a == "--dump_snapshots") {
      opt.dump_snapshots = true;
      continue;
    }
    if (starts_with(a, "--verify_case=")) {
      opt.verify_case = a.substr(std::string("--verify_case=").size());
      continue;
    }
    std::cerr << "Unknown arg: " << a << "\n";
    usage(argv[0]);
    return ParseOptsResult::Error;
  }
  return ParseOptsResult::Ok;
}

static bool is_known_verify_case(const std::string& verify_case) {
  return verify_case == "basic_lifecycle" ||
         verify_case == "invalid_sequence" ||
         verify_case == "catchup_stress_uncapped" ||
         verify_case == "one_active_stream_admission" ||
         verify_case == "staged_endpoint_span_inference";
}

static bool catchup_uncapped_case_is_hermetic() {
  const char* env = std::getenv("CAMBANG_DEV_SYNTH_CATCHUP_CAP");
  return env == nullptr || env[0] == '\0';
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

static bool stream_is_stopped(const CamBANGStateSnapshot& s, uint64_t stream_id) {
  for (const auto& st : s.streams) {
    if (st.stream_id == stream_id) {
      return st.mode == CBStreamMode::STOPPED;
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

static bool native_stream_has_phase(const CamBANGStateSnapshot& s,
                                    uint64_t stream_id,
                                    CBLifecyclePhase phase) {
  for (const auto& native : s.native_objects) {
    if (native.type == static_cast<uint32_t>(NativeObjectType::Stream) &&
        native.owner_stream_id == stream_id &&
        native.phase == phase) {
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

static const char* provider_error_label(ProviderError e) {
  switch (e) {
    case ProviderError::OK: return "OK";
    case ProviderError::ERR_NOT_SUPPORTED: return "ERR_NOT_SUPPORTED";
    case ProviderError::ERR_INVALID_ARGUMENT: return "ERR_INVALID_ARGUMENT";
    case ProviderError::ERR_BUSY: return "ERR_BUSY";
    case ProviderError::ERR_BAD_STATE: return "ERR_BAD_STATE";
    case ProviderError::ERR_PLATFORM_CONSTRAINT: return "ERR_PLATFORM_CONSTRAINT";
    case ProviderError::ERR_TRANSIENT_FAILURE: return "ERR_TRANSIENT_FAILURE";
    case ProviderError::ERR_PROVIDER_FAILED: return "ERR_PROVIDER_FAILED";
    case ProviderError::ERR_SHUTTING_DOWN: return "ERR_SHUTTING_DOWN";
    default: return "ERR_UNKNOWN";
  }
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
  auto fail = [](const std::string& stage, const std::string& detail) -> int {
    std::cerr << "basic_lifecycle FAIL [" << stage << "]: " << detail << "\n";
    return 1;
  };

  // Submit create and start in their documented Core order.
  const auto create_status =
      rt.try_create_stream(kStreamId, kDeviceInstanceId, StreamIntent::PREVIEW, nullptr, nullptr, 1);
  if (create_status != TryCreateStreamStatus::OK) {
    return fail("create_submission", "try_create_stream was not accepted");
  }
  const auto start_status = rt.try_start_stream(kStreamId);
  if (start_status != TryStartStreamStatus::OK) {
    return fail("start_submission", "try_start_stream was not accepted");
  }

  // Do not begin frame work merely because structural rows have appeared. Wait
  // for the complete observable start state that the following steps require:
  // flowing Core stream truth, a live native Stream, and its session seam.
  if (!wait_for_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return stream_is_flowing(s, kStreamId) &&
               native_stream_has_phase(s, kStreamId, CBLifecyclePhase::LIVE) &&
               has_acquisition_session_for_device(s, kDeviceInstanceId);
      }, opt.dump_snapshots)) {
    return fail("start_convergence",
                "FLOWING stream, LIVE native Stream, and AcquisitionSession were not observed together");
  }

  // Advance one frame period at a time and settle the corresponding Core-visible
  // frame before submitting the next tick. Repeating frames are intentionally
  // latest-state/lossy under pressure; this basic lifecycle case must not turn
  // into an accidental burst/backpressure test.
  constexpr uint64_t kFrameSteps = 5;
  for (uint64_t step = 1; step <= kFrameSteps; ++step) {
    const auto before = snapshot_copy(buf);
    if (!before) {
      return fail("frame_step", "snapshot disappeared before virtual-time advance");
    }
    const uint64_t target_frames = frames_received_for_stream(*before, kStreamId) + 1;

    tick_synthetic(rt, period);
    rt.request_publish();

    if (!wait_for_frames(buf, kStreamId, target_frames, opt.dump_snapshots)) {
      return fail("frame_step_" + std::to_string(step),
                  "frame did not settle; expected frames_received >= " +
                      std::to_string(target_frames));
    }
  }

  // Stop first and positively observe STOPPED truth before destroy. This keeps
  // the verifier independent of command/provider acknowledgement interleaving.
  const auto stop_status = rt.try_stop_stream(kStreamId);
  if (stop_status != TryStopStreamStatus::OK) {
    return fail("stop_submission", "try_stop_stream was not accepted");
  }
  if (!wait_for_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return stream_exists(s, kStreamId) && stream_is_stopped(s, kStreamId);
      }, opt.dump_snapshots)) {
    return fail("stop_convergence", "stream did not become observably STOPPED before destroy");
  }

  const auto destroy_status = rt.try_destroy_stream(kStreamId);
  if (destroy_status != TryDestroyStreamStatus::OK) {
    return fail("destroy_submission", "try_destroy_stream was not accepted after settled stop");
  }

  // Absence alone is ambiguous: it may mean either not-yet-integrated creation
  // or completed teardown. Require positive retained terminal native truth.
  // AcquisitionSession absence is deliberately not required because capture
  // parent priming may truthfully retain that device-owned seam.
  if (!wait_for_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return !stream_exists(s, kStreamId) &&
               native_stream_has_phase(s, kStreamId, CBLifecyclePhase::DESTROYED);
      }, opt.dump_snapshots)) {
    return fail("destroy_convergence",
                "Core stream row was not absent together with retained DESTROYED native Stream truth");
  }

  const auto final_snapshot = snapshot_copy(buf);
  if (!final_snapshot) {
    return fail("final_snapshot", "no snapshot available after lifecycle convergence");
  }
  if (!assert_unique_native_ids(*final_snapshot)) {
    return fail("native_identity", "native object identifiers were not unique");
  }

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

  // Assert canonical create-only seam: stream exists and AcquisitionSession exists
  // for create_stream-only (stream exists but not started).
  if (!wait_for_pred(buf, [&](const CamBANGStateSnapshot& s) {
        return stream_exists(s, kStreamId) &&
               has_acquisition_session_for_device(s, kDeviceInstanceId);
      }, opt.dump_snapshots)) {
    std::cerr << "FAIL: invalid-sequence seam expectations failed (stream/session)\n";
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

static int run_catchup_stress_uncapped(CoreRuntime& rt, StateSnapshotBuffer& buf, const Options& opt, uint64_t period) {
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
               has_acquisition_session_for_device(*s, kDeviceInstanceId);
      })) {
    std::cerr << "FAIL: stream did not reach FLOWING with expected session seam before catch-up tick\n";
    return 1;
  }

  // Bounded multi-frame catch-up pump.
  //
  // This case proves that one host-side synthetic-time advance can emit a
  // deterministic burst of due stream frames. It intentionally stays below the
  // runtime queue-pressure threshold: a full one-second / 31-frame burst is a
  // stress-pressure scenario and is not a stable exact-count verifier under the
  // current dispatcher/backpressure model.
  constexpr uint64_t kCatchupStressNs = 300'000'000ull;
  tick_synthetic(rt, kCatchupStressNs);

  // Expected frames in the bounded catch-up window with first frame at t=0:
  // floor(window/period)+1. At 30fps and 300ms this is 10 frames, ending at
  // timestamp 299999997ns.
  const uint64_t expected = (period == 0) ? 0 : (kCatchupStressNs / period) + 1;
  bool reached_expected = false;
  uint64_t best_seen = 0;
  for (int i = 0; i < 800; ++i) {
    rt.request_publish();
    // request_publish() is asynchronous; give the core thread one settle beat
    // before reading the next snapshot so the exact-count verifier does not
    // race partial post-catchup publications.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    auto s = snapshot_copy(buf);
    if (!s) {
      continue;
    }
    if (opt.dump_snapshots) {
      dump_snapshot(*s);
    }
    const uint64_t got_now = frames_received_for_stream(*s, kStreamId);
    if (got_now > best_seen) {
      best_seen = got_now;
    }
    if (got_now >= expected) {
      reached_expected = true;
      break;
    }
  }
  if (!reached_expected) {
    std::cerr << "FAIL: expected frames_received >= " << expected
              << " after catch-up tick, best_seen=" << best_seen << "\n";
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

static int run_one_active_stream_admission(CoreRuntime& rt, StateSnapshotBuffer& buf, const Options& opt) {
  constexpr uint64_t kStreamA = 101;
  constexpr uint64_t kStreamB = 102;

  if (rt.try_create_stream(kStreamA, kDeviceInstanceId, StreamIntent::PREVIEW, nullptr, nullptr, 1) != TryCreateStreamStatus::OK) {
    std::cerr << "FAIL: stream A create not accepted\n";
    return 1;
  }
  if (rt.try_create_stream(kStreamB, kDeviceInstanceId, StreamIntent::PREVIEW, nullptr, nullptr, 1) != TryCreateStreamStatus::OK) {
    std::cerr << "FAIL: stream B create not accepted\n";
    return 1;
  }
  if (rt.try_start_stream(kStreamA) != TryStartStreamStatus::OK) {
    std::cerr << "FAIL: stream A start not accepted\n";
    return 1;
  }
  if (rt.try_start_stream(kStreamB) != TryStartStreamStatus::Busy) {
    std::cerr << "FAIL: stream B start should be Busy while stream A is flowing\n";
    return 1;
  }

  rt.request_publish();
  if (!wait_until([&]() {
        auto s = snapshot_copy(buf);
        if (!s) return false;
        if (opt.dump_snapshots) dump_snapshot(*s);
        return stream_is_flowing(*s, kStreamA);
      })) {
    std::cerr << "FAIL: stream A did not reach flowing state\n";
    return 1;
  }

  if (rt.try_stop_stream(kStreamA) != TryStopStreamStatus::OK) {
    std::cerr << "FAIL: stream A stop not accepted\n";
    return 1;
  }
  if (rt.try_destroy_stream(kStreamA) != TryDestroyStreamStatus::OK) {
    std::cerr << "FAIL: stream A destroy not accepted\n";
    return 1;
  }
  if (rt.try_destroy_stream(kStreamB) != TryDestroyStreamStatus::OK) {
    std::cerr << "FAIL: stream B destroy not accepted\n";
    return 1;
  }
  return 0;
}

static int run_staged_endpoint_span_inference_regression(SyntheticProvider& prov) {
  constexpr uint64_t kBaseRoot = 9000;
  constexpr uint64_t kDid0 = 9100;
  constexpr uint64_t kDid1 = 9101;
  constexpr uint64_t kDid5Before = 9105;
  constexpr uint64_t kDid5After = 9205;
  constexpr uint64_t kDid5AfterStop = 9305;

  auto step_fail = [](const std::string& step, ProviderError err) -> int {
    std::cerr << "staged_endpoint_span_inference FAIL: " << step
              << ", got " << provider_error_label(err) << "\n";
    return 1;
  };
  auto step_fail_plain = [](const std::string& step) -> int {
    std::cerr << "staged_endpoint_span_inference FAIL: " << step << "\n";
    return 1;
  };

  auto r = prov.open_device("synthetic:0", kDid0, kBaseRoot + 0);
  if (!r.ok()) return step_fail("expected synthetic:0 open to succeed before staging", r.code);
  r = prov.open_device("synthetic:1", kDid1, kBaseRoot + 1);
  if (!r.ok()) return step_fail("expected synthetic:1 open to succeed before staging", r.code);
  r = prov.open_device("synthetic:5", kDid5Before, kBaseRoot + 5);
  if (r.ok()) return step_fail_plain("expected synthetic:5 open to be rejected before staging");
  (void)prov.close_device(kDid0);
  (void)prov.close_device(kDid1);

  SyntheticCanonicalScenario canonical{};
  for (uint32_t i = 0; i <= 5; ++i) {
    SyntheticScenarioDeviceDeclaration d{};
    d.key = std::string("Device") + static_cast<char>('A' + static_cast<int>(i));
    d.endpoint_index = i;
    canonical.devices.push_back(std::move(d));
  }
  r = prov.set_timeline_scenario_for_host(canonical);
  if (!r.ok()) return step_fail("expected canonical scenario staging to succeed", r.code);
  r = prov.start_timeline_scenario_for_host();
  if (!r.ok()) return step_fail("expected staged canonical scenario start to succeed", r.code);

  r = prov.open_device("synthetic:5", kDid5After, kBaseRoot + 15);
  if (!r.ok()) return step_fail("expected synthetic:5 open to succeed after staged endpoint widening", r.code);
  (void)prov.close_device(kDid5After);

  r = prov.stop_timeline_scenario_for_host();
  if (!r.ok()) return step_fail("expected staged scenario stop to succeed", r.code);
  r = prov.open_device("synthetic:5", kDid5AfterStop, kBaseRoot + 25);
  if (r.ok()) return step_fail_plain("expected synthetic:5 open to be rejected after staged scenario stop");
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  Options opt;
  const ParseOptsResult parse_result = parse_opts(argc, argv, opt);
  if (parse_result == ParseOptsResult::Help) {
    return 0;
  }
  if (parse_result != ParseOptsResult::Ok) {
    std::fprintf(stdout,
                 "FAIL: synthetic_timeline_verify failed (verify_case=%s, reason=invalid_arguments)\n",
                 opt.verify_case.c_str());
    std::fflush(stdout);
    return 2;
  }
  if (opt.verify_case == "catchup_stress_uncapped" &&
      !catchup_uncapped_case_is_hermetic()) {
    std::cerr << "FAIL: verify_case=catchup_stress_uncapped requires "
                 "CAMBANG_DEV_SYNTH_CATCHUP_CAP to be unset\n";
    std::fprintf(stdout,
                 "FAIL: synthetic_timeline_verify failed (verify_case=%s, reason=catchup_cap_env_set)\n",
                 opt.verify_case.c_str());
    std::fflush(stdout);
    return 1;
  }
  if (!is_known_verify_case(opt.verify_case)) {
    std::cerr << "Unknown verification case: " << opt.verify_case << "\n";
    usage(argv[0]);
    std::fprintf(stdout,
                 "FAIL: synthetic_timeline_verify failed (verify_case=%s, reason=unknown_verify_case)\n",
                 opt.verify_case.c_str());
    std::fflush(stdout);
    return 2;
  }

  QuietOutputCapture quiet_capture;
  bool capture_active = !opt.dump_snapshots && quiet_capture.begin();
  auto restore_quiet_capture = [&]() {
    if (capture_active) {
      quiet_capture.end();
      capture_active = false;
    }
  };
  auto replay_quiet_failure = [&]() {
    const std::string captured_stdout = quiet_capture.captured_stdout();
    const std::string captured_stderr = quiet_capture.captured_stderr();

    if (!captured_stdout.empty()) {
      std::fputs(captured_stdout.c_str(), stdout);
      std::fflush(stdout);
    }
    if (!captured_stderr.empty()) {
      std::fputs(captured_stderr.c_str(), stderr);
      std::fflush(stderr);
    }
  };

  CoreRuntime rt;
  StateSnapshotBuffer buf;
  rt.set_snapshot_publisher(&buf);

  if (!rt.start()) {
    restore_quiet_capture();
    replay_quiet_failure();
    std::cerr << "FAIL: core runtime did not start\n";
    std::fprintf(stdout,
                 "FAIL: synthetic_timeline_verify failed (verify_case=%s, reason=core_runtime_start_failed)\n",
                 opt.verify_case.c_str());
    std::fflush(stdout);
    return 2;
  }
  if (!wait_until([&]() {
        return rt.state_copy() == CoreRuntimeState::LIVE;
      }, 200, 1)) {
    restore_quiet_capture();
    replay_quiet_failure();
    std::cerr << "FAIL: core runtime did not become LIVE before verifier setup\n";
    rt.stop();
    std::fprintf(stdout,
                 "FAIL: synthetic_timeline_verify failed (verify_case=%s, reason=core_runtime_not_live)\n",
                 opt.verify_case.c_str());
    std::fflush(stdout);
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
    restore_quiet_capture();
    replay_quiet_failure();
    std::cerr << "FAIL: synthetic provider initialize failed\n";
    rt.stop();
    std::fprintf(stdout,
                 "FAIL: synthetic_timeline_verify failed (verify_case=%s, reason=synthetic_provider_initialize_failed)\n",
                 opt.verify_case.c_str());
    std::fflush(stdout);
    return 2;
  }

  rt.attach_provider(&prov);
  auto stop_attached_runtime = [&]() {
    rt.stop();
    rt.attach_provider(nullptr);
  };

  // Open the first endpoint deterministically.
  std::vector<CameraEndpoint> eps;
  if (!prov.enumerate_endpoints(eps).ok() || eps.empty()) {
    restore_quiet_capture();
    replay_quiet_failure();
    std::cerr << "FAIL: enumerate_endpoints failed\n";
    stop_attached_runtime();
    std::fprintf(stdout,
                 "FAIL: synthetic_timeline_verify failed (verify_case=%s, reason=enumerate_endpoints_failed)\n",
                 opt.verify_case.c_str());
    std::fflush(stdout);
    return 2;
  }
  const auto retain_identity = rt.retain_device_identity(kDeviceInstanceId, eps[0].hardware_id);
  if (retain_identity != CoreThread::PostResult::Enqueued) {
    restore_quiet_capture();
    replay_quiet_failure();
    std::cerr << "FAIL: retain_device_identity admission failed: "
              << static_cast<int>(retain_identity) << "\n";
    stop_attached_runtime();
    std::fprintf(stdout,
                 "FAIL: synthetic_timeline_verify failed (verify_case=%s, reason=retain_device_identity_failed)\n",
                 opt.verify_case.c_str());
    std::fflush(stdout);
    return 2;
  }
  if (!prov.open_device(eps[0].hardware_id, kDeviceInstanceId, kRootId).ok()) {
    restore_quiet_capture();
    replay_quiet_failure();
    std::cerr << "FAIL: open_device failed\n";
    stop_attached_runtime();
    std::fprintf(stdout,
                 "FAIL: synthetic_timeline_verify failed (verify_case=%s, reason=open_device_failed)\n",
                 opt.verify_case.c_str());
    std::fflush(stdout);
    return 2;
  }

  // Prime a publish so the snapshot pipeline is live.
  rt.request_publish();
  (void)wait_until([&]() { return snapshot_copy(buf) != nullptr; }, 200, 2);

  const uint64_t period = fps_period_ns(cfg.nominal.fps_num, cfg.nominal.fps_den);
  if (period == 0) {
    restore_quiet_capture();
    replay_quiet_failure();
    std::cerr << "FAIL: invalid fps period\n";
    stop_attached_runtime();
    std::fprintf(stdout,
                 "FAIL: synthetic_timeline_verify failed (verify_case=%s, reason=invalid_fps_period)\n",
                 opt.verify_case.c_str());
    std::fflush(stdout);
    return 2;
  }

  int r = 0;
  std::string failure_reason;
  if (opt.verify_case == "basic_lifecycle") {
    r = run_basic_lifecycle(rt, buf, opt, period);
  } else if (opt.verify_case == "invalid_sequence") {
    r = run_invalid_sequence(rt, buf, opt);
  } else if (opt.verify_case == "catchup_stress_uncapped") {
    r = run_catchup_stress_uncapped(rt, buf, opt, period);
  } else if (opt.verify_case == "one_active_stream_admission") {
    r = run_one_active_stream_admission(rt, buf, opt);
  } else if (opt.verify_case == "staged_endpoint_span_inference") {
    r = run_staged_endpoint_span_inference_regression(prov);
  }
  if (r != 0 && failure_reason.empty()) {
    failure_reason = "case_returned_nonzero";
  }

  // CoreRuntime owns attached-provider shutdown while the core thread is live.
  stop_attached_runtime();
  restore_quiet_capture();

  if (r == 0) {
    std::fprintf(stdout,
                 "OK: synthetic_timeline_verify passed (verify_case=%s)\n",
                 opt.verify_case.c_str());
    std::fflush(stdout);
  } else {
    replay_quiet_failure();
    std::fprintf(stdout,
                 "FAIL: synthetic_timeline_verify failed (verify_case=%s, reason=%s)\n",
                 opt.verify_case.c_str(),
                 failure_reason.c_str());
    std::fflush(stdout);
  }
  return r;
}
