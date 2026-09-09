// Deterministic coverage for frame-rate selection (imaging/api/frame_rate_selection.h).
//
// The decision governs the rate every platform provider asks its backend for.
// Those providers are platform-only and unreachable from any host verifier, and
// the decision is pure, so it lives in imaging/api and is exercised here --
// the same arrangement as acquisition_seam_claims_verify and
// capture_sequence_settlement_verify.
//
// The case this exists to hold down: a requested frame rate was parsed, stored,
// published in the state snapshot, and never asked of any backend. It reached
// CaptureProfile on 2026-03-01 and the boundary parser on 2026-07-19, and was
// never applied by either platform provider until 2026-09-08. A caller asking
// for 15fps got the sensor's own choice, and CamBANG reported 15 regardless.
//
// The distinctions the tests below defend are the ones that failure had no way
// to express. Exact PROMISES a rate; Satisfied only promises a span the backend
// may sit anywhere inside; Unserviceable means nothing advertised honours the
// request, so nothing is substituted and start_stream refuses; NotReported means
// the backend said nothing about rates at all, which refuses nothing. Collapsing
// any of those into "ok" is how a set-point ends up published as realized truth,
// and collapsing the last two would refuse every rate request on every provider
// that has not implemented the capability yet.

#include "imaging/api/frame_rate_selection.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace cambang;

namespace {

int g_run = 0;
int g_failed = 0;

void check(bool cond, const std::string& what) {
  ++g_run;
  if (!cond) {
    ++g_failed;
    std::cout << "FAIL " << what << "\n";
  }
}

FrameRateSelection select(uint32_t want_min, uint32_t want_max,
                          const std::vector<FrameRateRange>& candidates) {
  return select_frame_rate(FrameRateRequest{want_min, want_max}, candidates.data(),
                           candidates.size());
}

// The ranges both curated handsets actually advertise, read from device logs on
// 2026-09-03. Real advertisements rather than invented ones, so a case that
// passes here is a case that holds on hardware we own.
const std::vector<FrameRateRange> kGalaxyS20Plus = {
    {15, 15}, {7, 24}, {24, 24}, {7, 30}, {30, 30}};
const std::vector<FrameRateRange> kQuest3 = {{1, 15}, {15, 15}, {1, 30}, {30, 30}};

void run_no_request_checks() {
  // No request, but the backend reported capability: Core SELECTS, exactly as it
  // selects a pixel format the caller did not name. The outcome says the caller
  // asked for nothing; `chosen` says what it will get.
  const FrameRateSelection s = select(0, 0, kGalaxyS20Plus);
  check(s.outcome == FrameRateSelectionOutcome::NotRequested,
        "no request must report NotRequested");
  check(s.chosen.max_fps == 30,
        "no request must still select from capability, taking the highest ceiling");

  // Zero is not a request for zero frames, and with nothing advertised there is
  // nothing to select from either.
  const FrameRateSelection empty = select(0, 0, {});
  check(empty.outcome == FrameRateSelectionOutcome::NotReported,
        "no request and no capability is NotReported, not a refusal");
}

void run_not_reported_checks() {
  // THE DISTINCTION THAT MATTERS. A provider that has not implemented rate
  // capability -- the default, and every unimplemented seam -- reports nothing.
  // That must not read as "no rate is supported", or every rate request would be
  // refused on behalf of a backend that never objected.
  const FrameRateSelection s = select(15, 15, {});
  check(s.outcome == FrameRateSelectionOutcome::NotReported,
        "an unreported capability refuses nothing");
  check(s.chosen.min_fps == 0 && s.chosen.max_fps == 0,
        "NotReported selects nothing, so the caller's request stands");
}

void run_unserviceable_checks() {
  // Capability IS reported and nothing in it honours the request. This is the
  // geometry rule: an unobtainable rate is refused, never substituted.
  const FrameRateSelection fast = select(120, 120, kGalaxyS20Plus);
  check(fast.outcome == FrameRateSelectionOutcome::Unserviceable,
        "a rate above anything advertised must be Unserviceable");
  check(fast.chosen.min_fps == 0 && fast.chosen.max_fps == 0,
        "Unserviceable must substitute nothing -- the old clamp chose 30 here");

  const FrameRateSelection slow = select(2, 2, kGalaxyS20Plus);
  check(slow.outcome == FrameRateSelectionOutcome::Unserviceable,
        "a rate below anything advertised must be Unserviceable");

  // 2fps on the Quest, which advertises [1-15]. The range CONTAINS 2 and the old
  // clamp chose it. It still does not PROMISE 2, so under the geometry rule it
  // is refused rather than served -- the same reasoning as the [7-30] case below.
  const FrameRateSelection quest_slow = select(2, 2, kQuest3);
  check(quest_slow.outcome == FrameRateSelectionOutcome::Unserviceable,
        "a range containing the request still does not honour it");

  // Advertisements that cannot be honoured are ignored rather than reasoned
  // about, and ignoring all of them leaves nothing that satisfies the request.
  const FrameRateSelection junk = select(15, 15, {{0, 0}, {30, 10}});
  check(junk.outcome == FrameRateSelectionOutcome::Unserviceable,
        "malformed advertisements are not selectable");
}

void run_exact_checks() {
  // The motivating case, on both handsets: 15fps is advertised as a fixed range,
  // so the caller can actually be promised it.
  for (const auto* dev : {&kGalaxyS20Plus, &kQuest3}) {
    const FrameRateSelection s = select(15, 15, *dev);
    check(s.outcome == FrameRateSelectionOutcome::Exact,
          "15fps fixed must resolve Exact on a device advertising [15-15]");
    check(s.chosen.min_fps == 15 && s.chosen.max_fps == 15,
          "Exact must choose the fixed 15 range");
  }
}

void run_satisfied_is_not_exact_check() {
  // THE REGRESSION THIS FILE EXISTS FOR. [7-30] contains 15, and choosing it
  // when 15 was asked for would let AE sit anywhere in 7..30 while the caller
  // believed it had been given 15.
  const std::vector<FrameRateRange> spans_only = {{7, 30}, {1, 60}};
  const FrameRateSelection s = select(15, 15, spans_only);
  check(s.outcome == FrameRateSelectionOutcome::Unserviceable,
        "a span merely CONTAINING a fixed request does not satisfy it");

  // A caller that genuinely asked for a span gets one, and is told it is a span.
  const FrameRateSelection span = select(7, 30, spans_only);
  check(span.outcome == FrameRateSelectionOutcome::Satisfied,
        "a span request served by a span is Satisfied, not Exact");
  check(span.chosen.min_fps == 7 && span.chosen.max_fps == 30,
        "the wholly-contained span is the one chosen");
}

void run_range_request_checks() {
  // Several candidates qualify; the caller wants frames, so the highest ceiling
  // wins, and a fixed candidate breaks the tie because it alone promises a rate.
  const FrameRateSelection s = select(1, 30, kQuest3);
  check(s.outcome == FrameRateSelectionOutcome::Exact,
        "a wide request served by a fixed candidate is Exact");
  check(s.chosen.max_fps == 30, "highest ceiling wins among qualifying candidates");

  // An open-ended floor: "at least 24".
  const FrameRateSelection floor_only = select(24, 0, kGalaxyS20Plus);
  check(floor_only.outcome == FrameRateSelectionOutcome::Exact,
        "an omitted ceiling is not a ceiling");
  check(floor_only.chosen.min_fps == 30 && floor_only.chosen.max_fps == 30,
        "at-least-24 should take [30-30] over [24-24]");

  // An open-ended ceiling: "at most 15".
  const FrameRateSelection ceil_only = select(0, 15, kGalaxyS20Plus);
  check(ceil_only.outcome == FrameRateSelectionOutcome::Exact,
        "an omitted floor is not a floor");
  check(ceil_only.chosen.max_fps == 15, "at-most-15 must not choose 24 or 30");

  // A range that lands between advertised options is refused like any other
  // unobtainable configuration. Asking for a range widens the odds of being
  // served; it does not confer portability.
  const FrameRateSelection between = select(10, 12, kGalaxyS20Plus);
  check(between.outcome == FrameRateSelectionOutcome::Unserviceable,
        "a range containing no advertised candidate is refused");
}

}  // namespace

int main() {
  run_no_request_checks();
  run_not_reported_checks();
  run_unserviceable_checks();
  run_exact_checks();
  run_satisfied_is_not_exact_check();
  run_range_request_checks();

  if (g_failed != 0) {
    std::cout << "FAIL frame_rate_selection_verify run=" << g_run
              << " failed=" << g_failed << "\n";
    return 1;
  }
  std::cout << "PASS frame_rate_selection_verify run=" << g_run << " ok=" << g_run
            << " failed=0\n";
  return 0;
}
