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
// The distinction the tests below defend is the one
// that failure had no way to express -- Exact PROMISES a rate, Satisfied only
// promises a span the backend may sit anywhere inside, and Clamped means the
// request could not be served at all. Collapsing those three into "ok" is how a
// set-point ends up published as realized truth.

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
  const FrameRateSelection s = select(0, 0, kGalaxyS20Plus);
  check(s.outcome == FrameRateSelectionOutcome::NotRequested,
        "no request must be NotRequested, never a rate invented for the caller");

  // Zero is not a request for zero frames; a provider must keep its own default.
  const FrameRateSelection empty = select(0, 0, {});
  check(empty.outcome == FrameRateSelectionOutcome::NotRequested,
        "no request outranks an empty advertisement");
}

void run_unavailable_checks() {
  const FrameRateSelection s = select(15, 15, {});
  check(s.outcome == FrameRateSelectionOutcome::Unavailable,
        "asked but nothing advertised must be Unavailable, distinct from NotRequested");

  // An advertisement that cannot be honoured is ignored rather than reasoned
  // about; a zero or inverted range is a broken advertisement, not an option.
  const FrameRateSelection junk = select(15, 15, {{0, 0}, {30, 10}});
  check(junk.outcome == FrameRateSelectionOutcome::Unavailable,
        "malformed advertisements must not be selectable");
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
  // believed it had been given 15. A containing range must never satisfy a
  // fixed request.
  const std::vector<FrameRateRange> spans_only = {{7, 30}, {1, 60}};
  const FrameRateSelection s = select(15, 15, spans_only);
  check(s.outcome == FrameRateSelectionOutcome::Clamped,
        "a span merely CONTAINING the fixed request must not read as satisfying it");

  // A caller that genuinely asked for a span gets one, and is told it is a span.
  const FrameRateSelection span = select(7, 30, spans_only);
  check(span.outcome == FrameRateSelectionOutcome::Satisfied,
        "a span request served by a span is Satisfied, not Exact");
  check(span.chosen.min_fps == 7 && span.chosen.max_fps == 30,
        "the wholly-contained span is the one chosen");
}

void run_range_request_checks() {
  // Asked for anything up to 30. Several candidates qualify; the caller wants
  // frames, so the highest ceiling wins, and a fixed candidate breaks the tie
  // because it is the only kind that promises anything.
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
}

void run_clamped_checks() {
  // 120fps on a device topping out at 30. The caller is getting something it did
  // not ask for, which is permitted only because it is reported as Clamped.
  const FrameRateSelection fast = select(120, 120, kGalaxyS20Plus);
  check(fast.outcome == FrameRateSelectionOutcome::Clamped,
        "an unreachable rate must be Clamped, never silently Satisfied");
  check(fast.chosen.max_fps == 30, "clamping high picks the nearest, which is 30");

  // 2fps on the S20+, whose slowest floor is 7.
  const FrameRateSelection slow = select(2, 2, kGalaxyS20Plus);
  check(slow.outcome == FrameRateSelectionOutcome::Clamped,
        "a rate below anything advertised must be Clamped");
  check(slow.chosen.min_fps == 7,
        "clamping low picks the nearest floor");

  // 2fps on the Quest, which advertises [1-15]. The range CONTAINS 2, and it is
  // tempting to call that served -- but it promises 2 no more than [7-30]
  // promises 15, so it is Clamped for exactly the reason asserted in
  // run_satisfied_is_not_exact_check. What the overlap does buy the caller is
  // the choice itself: [1-15] is nearest, so that is what gets asked for, and 2
  // is at least reachable within it.
  const FrameRateSelection quest_slow = select(2, 2, kQuest3);
  check(quest_slow.outcome == FrameRateSelectionOutcome::Clamped,
        "a range containing the request still cannot promise it");
  check(quest_slow.chosen.min_fps == 1 && quest_slow.chosen.max_fps == 15,
        "the overlapping range is nearest and must be the one chosen");

  // KNOWN CONFLATION, asserted so it is a decision rather than an accident.
  // Both of these are Clamped, though they differ for a caller: the first
  // overlaps the request and might yield it, the second cannot possibly. The
  // outcome does not currently separate them, and the chosen range is what
  // distinguishes them. Splitting Clamped would be the change if that stops
  // being enough.
  check(select(2, 2, kQuest3).outcome == select(120, 120, kQuest3).outcome,
        "overlapping and unreachable both report Clamped today");
}

}  // namespace

int main() {
  run_no_request_checks();
  run_unavailable_checks();
  run_exact_checks();
  run_satisfied_is_not_exact_check();
  run_range_request_checks();
  run_clamped_checks();

  if (g_failed != 0) {
    std::cout << "FAIL frame_rate_selection_verify run=" << g_run
              << " failed=" << g_failed << "\n";
    return 1;
  }
  std::cout << "PASS frame_rate_selection_verify run=" << g_run << " ok=" << g_run
            << " failed=0\n";
  return 0;
}
