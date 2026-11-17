/*
CamBANG Maintainer Utility

Tool: early_timing_dispatch_verify

Purpose
-------
Verifies the Core side of early acquisition-timing delivery
(CaptureImageTimingNotice -> ProviderCallbackIngress listener), host-native:

  1. End to end on the reference provider: every member of a bracket capture
     reaches the listener exactly once, off the core thread, naming the member
     index the retained result holds and an acquisition timing equal to it.
  2. Clearing the listener is quiescent: it does not return while an
     invocation is running, and nothing is delivered after it returns.
  3. A listener that throws does not wedge clearing or the next delivery.
  4. Replacing a listener never delivers one notice to both.
  5. Without a listener a notice is a no-op, and installing one later does not
     replay earlier notices.

Scope honesty
-------------
This does NOT verify the Godot boundary (queue, call_deferred, signal emit) or
the platform providers' notice points; those need Godot and real devices and
are covered by scene 80 on each surface.

Category
--------
Verification tool (maintainer/CI).
*/

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "early_timing_dispatch_verify: build through the repo SCons maintainer_tools alias so CAMBANG_INTERNAL_SMOKE=1 is defined."
#endif

#include "core/core_runtime.h"
#include "imaging/synthetic/provider.h"

using namespace cambang;

namespace {

int g_run = 0;
int g_failed = 0;

void check(bool ok, const char* what) {
  ++g_run;
  if (!ok) {
    ++g_failed;
    std::fprintf(stderr, "FAIL: %s\n", what);
  }
}

bool wait_live(CoreRuntime& rt) {
  for (int i = 0; i < 400; ++i) {
    if (rt.state_copy() == CoreRuntimeState::LIVE) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

bool same_timing(const std::optional<SourcedFact<ImageAcquisitionTiming>>& a,
                 const std::optional<SourcedFact<ImageAcquisitionTiming>>& b) {
  if (a.has_value() != b.has_value()) return false;
  if (!a) return true;
  return a->origin == b->origin &&
         a->value.acquisition_mark() == b->value.acquisition_mark() &&
         a->value.tick_period().numerator_ns() == b->value.tick_period().numerator_ns() &&
         a->value.tick_period().denominator() == b->value.tick_period().denominator() &&
         a->value.clock_domain() == b->value.clock_domain() &&
         a->value.reference_event() == b->value.reference_event() &&
         a->value.comparability() == b->value.comparability();
}

CaptureImageTimingNotice make_notice(uint64_t capture_id, uint32_t member) {
  CaptureImageTimingNotice n{};
  n.capture_id = capture_id;
  n.device_instance_id = 1;
  n.image_member_index = member;
  return n;
}

void case_end_to_end_synthetic_bracket() {
  CoreRuntime rt;
  if (!rt.start() || !wait_live(rt)) {
    check(false, "e2e: runtime did not reach LIVE");
    rt.stop();
    return;
  }

  struct Seen {
    CaptureImageTimingNotice notice;
    std::thread::id thread;
  };
  std::mutex mu;
  std::vector<Seen> seen;
  rt.set_capture_image_timing_listener([&](const CaptureImageTimingNotice& n) {
    std::lock_guard<std::mutex> lk(mu);
    seen.push_back({n, std::this_thread::get_id()});
  });

  // The core thread's id, observed from a task it runs.
  std::atomic<bool> got_core_id{false};
  std::thread::id core_id{};
  rt.post([&] {
    core_id = std::this_thread::get_id();
    got_core_id.store(true, std::memory_order_release);
  });

  SyntheticProviderConfig cfg{};
  cfg.endpoint_count = 1;
  cfg.nominal.width = 64;
  cfg.nominal.height = 64;
  cfg.nominal.format_fourcc = FOURCC_RGBA;
  SyntheticProvider provider(cfg);
  const auto cleanup = [&] {
    rt.clear_capture_image_timing_listener();
    rt.stop();
    rt.attach_provider(nullptr);
  };
  std::vector<CameraEndpoint> eps;
  if (!provider.initialize(rt.provider_callbacks()).ok()) {
    check(false, "e2e: provider init failed");
    cleanup();
    return;
  }
  rt.attach_provider(&provider);
  if (!provider.enumerate_endpoints(eps).ok() || eps.empty() ||
      !provider.open_device(eps[0].hardware_id, 71, 7101).ok()) {
    check(false, "e2e: open_device failed");
    cleanup();
    return;
  }
  CaptureRequest req{};
  for (int i = 0; i < 100 && req.device_instance_id != 71; ++i) {
    if (rt.materialize_capture_request(71, req)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (req.device_instance_id != 71) {
    check(false, "e2e: materialize_capture_request failed");
    cleanup();
    return;
  }
  req.capture_id = 9701;
  req.still_image_bundle = make_default_metered_still_image_bundle();
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{1u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, -1000});
  req.still_image_bundle.members.push_back(
      CaptureStillImageMember{2u, CaptureStillImageMemberRole::ADDITIONAL_BRACKET, +1000});
  if (!provider.trigger_capture(req).ok()) {
    check(false, "e2e: trigger_capture failed");
    cleanup();
    return;
  }
  SharedCaptureResultData result;
  for (int i = 0; i < 200 && !result; ++i) {
    result = rt.get_capture_result(req.capture_id, 71);
    if (!result) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  check(static_cast<bool>(result), "e2e: retained result missing");
  check(got_core_id.load(std::memory_order_acquire), "e2e: core thread id not observed");

  std::vector<Seen> copy;
  {
    std::lock_guard<std::mutex> lk(mu);
    copy = seen;
  }
  std::map<uint32_t, const Seen*> by_member;
  bool duplicate = false;
  bool off_core = true;
  for (const Seen& s : copy) {
    if (s.notice.capture_id != req.capture_id) continue;
    if (!by_member.emplace(s.notice.image_member_index, &s).second) duplicate = true;
    if (s.thread == core_id) off_core = false;
  }
  check(!duplicate, "e2e: a member was announced twice");
  check(off_core, "e2e: listener ran on the core thread");
  check(by_member.size() == 3, "e2e: expected one notice per member (3)");

  if (result && by_member.size() == 3) {
    const auto check_member = [&](uint32_t idx,
                                  const std::optional<SourcedFact<ImageAcquisitionTiming>>& final_timing,
                                  CaptureImageRouting routing) {
      const auto it = by_member.find(idx);
      if (it == by_member.end()) {
        check(false, "e2e: member notice missing");
        return;
      }
      check(it->second->notice.routing == routing, "e2e: notice routing differs from result");
      check(it->second->notice.acquisition_timing.has_value(), "e2e: notice timing absent");
      check(same_timing(it->second->notice.acquisition_timing, final_timing),
            "e2e: notice timing differs from retained result");
      check(it->second->notice.observed_core_monotonic_ns != 0,
            "e2e: notice observation time absent");
    };
    check_member(result->default_image.image_member_index,
                 result->default_image.acquisition_timing,
                 CaptureImageRouting::DEFAULT_METERED);
    for (const auto& img : result->additional_images) {
      check_member(img.image_member_index, img.acquisition_timing,
                   CaptureImageRouting::ADDITIONAL_BRACKET);
    }
  }

  (void)provider.close_device(71);
  cleanup();
  (void)provider.shutdown();
}

void case_clear_is_quiescent() {
  CoreRuntime rt;
  std::atomic<bool> inside{false};
  std::atomic<int> calls{0};
  rt.set_capture_image_timing_listener([&](const CaptureImageTimingNotice&) {
    inside.store(true, std::memory_order_release);
    calls.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    inside.store(false, std::memory_order_release);
  });
  std::thread producer([&] { rt.provider_callbacks()->on_capture_image_timing(make_notice(1, 0)); });
  for (int i = 0; i < 200 && !inside.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(inside.load(std::memory_order_acquire), "quiescent: listener never entered");
  rt.clear_capture_image_timing_listener();
  check(!inside.load(std::memory_order_acquire), "quiescent: clear returned mid-invocation");
  producer.join();
  rt.provider_callbacks()->on_capture_image_timing(make_notice(1, 1));
  check(calls.load() == 1, "quiescent: delivered after clear");
}

void case_throwing_listener() {
  CoreRuntime rt;
  std::atomic<int> calls{0};
  rt.set_capture_image_timing_listener([&](const CaptureImageTimingNotice&) {
    calls.fetch_add(1, std::memory_order_relaxed);
    throw std::runtime_error("listener failure");
  });
  rt.provider_callbacks()->on_capture_image_timing(make_notice(2, 0));
  rt.provider_callbacks()->on_capture_image_timing(make_notice(2, 1));
  check(calls.load() == 2, "throw: second notice not delivered after a throw");
  rt.clear_capture_image_timing_listener();  // must not hang
  check(true, "throw: clear returned");
}

void case_replace_never_double_delivers() {
  CoreRuntime rt;
  std::atomic<int> a{0};
  std::atomic<int> b{0};
  rt.set_capture_image_timing_listener([&](const CaptureImageTimingNotice&) { a.fetch_add(1); });
  rt.provider_callbacks()->on_capture_image_timing(make_notice(3, 0));
  rt.set_capture_image_timing_listener([&](const CaptureImageTimingNotice&) { b.fetch_add(1); });
  rt.provider_callbacks()->on_capture_image_timing(make_notice(3, 1));
  check(a.load() == 1 && b.load() == 1, "replace: each notice reached exactly one listener");
  rt.clear_capture_image_timing_listener();
}

void case_no_listener_no_replay() {
  CoreRuntime rt;
  rt.provider_callbacks()->on_capture_image_timing(make_notice(4, 0));
  std::atomic<int> calls{0};
  rt.set_capture_image_timing_listener([&](const CaptureImageTimingNotice&) { calls.fetch_add(1); });
  check(calls.load() == 0, "no-replay: earlier notice replayed to a later listener");
  rt.clear_capture_image_timing_listener();
}

}  // namespace

int main() {
  case_end_to_end_synthetic_bracket();
  case_clear_is_quiescent();
  case_throwing_listener();
  case_replace_never_double_delivers();
  case_no_listener_no_replay();
  if (g_failed != 0) {
    std::printf("FAIL early_timing_dispatch_verify run=%d ok=%d failed=%d\n", g_run,
                g_run - g_failed, g_failed);
    return 1;
  }
  std::printf("PASS early_timing_dispatch_verify run=%d ok=%d failed=0\n", g_run, g_run);
  return 0;
}
