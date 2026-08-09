// Deterministic coverage for capture settlement and payload-debt policy
// (imaging/api/capture_sequence_settlement.h).
//
// The policy governs Camera2's still-capture collector, which is Android-only
// and unreachable from any host verifier. The decision itself is pure, so it
// lives in imaging/api and is exercised here instead -- same arrangement as
// outstanding_payload_ledger_verify.
//
// The case this exists to hold down: a capture that finished short with its
// sequence ended must record NO debt. Getting that wrong does not lose one
// capture, it loses every subsequent capture on the device, because the
// unpayable debt consumes the next capture's payload and is immediately
// re-incurred. See the header for the measured 2 -> 0 -> 0 -> 0.

#include "imaging/api/capture_sequence_settlement.h"

#include <cstdint>
#include <iostream>
#include <string>

using namespace cambang;

namespace {

int g_run = 0;
int g_failed = 0;

// Sequence ended and its in-flight grace expired: the capture has waited as long
// as it is ever going to. Most cases below are about the terminal state, so this
// is the default shape.
CaptureSequenceProgress progress(size_t expected, size_t arrived, size_t failed,
                                 bool sequence_ended) {
  CaptureSequenceProgress p{};
  p.expected = expected;
  p.arrived = arrived;
  p.failed = failed;
  p.sequence_ended = sequence_ended;
  p.in_flight_grace_elapsed = sequence_ended;
  return p;
}

// Sequence ended, grace still running -- the window in which a buffer already
// being delivered can still land.
CaptureSequenceProgress in_grace(size_t expected, size_t arrived, size_t failed) {
  CaptureSequenceProgress p{};
  p.expected = expected;
  p.arrived = arrived;
  p.failed = failed;
  p.sequence_ended = true;
  p.in_flight_grace_elapsed = false;
  return p;
}

void check(const std::string& what, bool ok) {
  ++g_run;
  if (!ok) {
    ++g_failed;
    std::cerr << "FAIL " << what << "\n";
  }
}

void expect(const std::string& what, const CaptureSequenceProgress& p, bool settled,
            uint64_t debt, bool finished_short) {
  check(what + " [settled]", capture_sequence_is_settled(p) == settled);
  check(what + " [debt]", capture_outstanding_payload_debt(p) == debt);
  check(what + " [short]", capture_finished_short(p) == finished_short);
}

// ---------------------------------------------------------------------------

void run_complete_capture_checks() {
  // Every member arrived: settled, nothing owed, not short. Whether the
  // sequence has ended yet is irrelevant -- the capture has what it asked for.
  expect("single member arrived, sequence still open", progress(1, 1, 0, false),
         /*settled=*/true, /*debt=*/0, /*short=*/false);
  expect("single member arrived, sequence ended", progress(1, 1, 0, true), true, 0, false);
  expect("three members all arrived", progress(3, 3, 0, false), true, 0, false);

  // A member the platform explicitly failed is accounted for. It owes nothing:
  // the platform has already reported its outcome.
  expect("member failed, sequence open", progress(1, 0, 1, false), true, 0, false);
  expect("bracket with one failed member", progress(3, 2, 1, false), true, 0, false);
}

void run_still_waiting_checks() {
  // Nothing has ended and members are missing: not settled, and the wait
  // continues. No debt is recorded until the capture actually gives up.
  expect("nothing arrived, sequence open", progress(1, 0, 0, false),
         /*settled=*/false, /*debt=*/1, /*short=*/true);
  expect("partial bracket, sequence open", progress(3, 1, 0, false), false, 2, true);
}

void run_sequence_end_settlement_checks() {
  // THE CASE. The platform closed the submission with a member missing. The
  // capture is short -- truthfully -- but nothing is outstanding, so no debt.
  expect("nothing arrived, sequence ended", progress(1, 0, 0, true),
         /*settled=*/true, /*debt=*/0, /*short=*/true);
  expect("partial bracket, sequence ended", progress(3, 1, 0, true), true, 0, true);
  expect("partial bracket with a failure, sequence ended", progress(3, 1, 1, true), true, 0,
         true);

  // Short and owed are independent. A capture can be short while owing nothing,
  // which is precisely what a timeout-only model could not express.
  const CaptureSequenceProgress ended = progress(2, 0, 0, true);
  check("sequence ended: short but owes nothing",
        capture_finished_short(ended) && capture_outstanding_payload_debt(ended) == 0);

  const CaptureSequenceProgress open = progress(2, 0, 0, false);
  check("sequence open: short and owes",
        capture_finished_short(open) && capture_outstanding_payload_debt(open) == 2);
}

void run_in_flight_grace_checks() {
  // A short capture whose sequence has just ended is NOT settled: a buffer may
  // be mid-delivery. It waits out its grace first.
  const CaptureSequenceProgress waiting = in_grace(1, 0, 0);
  check("sequence just ended, short: not settled", !capture_sequence_is_settled(waiting));
  check("sequence just ended, short: grace is armed",
        capture_awaits_in_flight_grace(waiting));

  // The grace ends the wait even if nothing more arrives.
  expect("grace elapsed, nothing arrived", progress(1, 0, 0, true),
         /*settled=*/true, /*debt=*/0, /*short=*/true);

  // A capture that got everything never waits, grace or no grace: there is
  // nothing in flight to wait for.
  const CaptureSequenceProgress complete = in_grace(1, 1, 0);
  check("complete capture settles without waiting out the grace",
        capture_sequence_is_settled(complete));
  check("complete capture does not arm the grace",
        !capture_awaits_in_flight_grace(complete));

  // A member the platform explicitly failed is accounted for and needs no
  // grace: no buffer was ever coming for it.
  const CaptureSequenceProgress all_failed = in_grace(2, 1, 1);
  check("failures count against the grace as accounted members",
        capture_sequence_is_settled(all_failed) &&
            !capture_awaits_in_flight_grace(all_failed));

  // Grace never affects debt in either direction.
  check("grace running: still no debt", capture_outstanding_payload_debt(waiting) == 0);
  check("grace elapsed: still no debt",
        capture_outstanding_payload_debt(progress(1, 0, 0, true)) == 0);

  // An open sequence has no grace to arm: it is not a post-end state.
  const CaptureSequenceProgress open = in_grace(1, 0, 0);
  CaptureSequenceProgress never_ended = open;
  never_ended.sequence_ended = false;
  check("open sequence does not arm the grace",
        !capture_awaits_in_flight_grace(never_ended));
  check("open sequence is not settled by an elapsed grace flag", [] {
    CaptureSequenceProgress p{};
    p.expected = 1;
    p.sequence_ended = false;
    p.in_flight_grace_elapsed = true;
    return !capture_sequence_is_settled(p);
  }());
}

void run_s20p_late_buffer_regression_check() {
  // Replays the measured Galaxy S20+ camera 0 ordering: the buffer's delivery
  // callback is already running when onCaptureSequenceCompleted fires, and the
  // image lands 7-13ms later. Settling on sequence end alone loses it; the
  // grace collects it.
  //
  // Camera 1 of the same device fires the callbacks the other way round and is
  // the control arm: it must be unaffected.
  const int captures = 30;

  int lost_without_grace = 0;
  int lost_with_grace = 0;
  for (int i = 0; i < captures; ++i) {
    // At the moment the sequence ends, the buffer has not been pushed yet.
    CaptureSequenceProgress at_sequence_end = in_grace(1, 0, 0);

    // Without a grace, settlement is immediate and the capture is short.
    CaptureSequenceProgress no_grace = at_sequence_end;
    no_grace.in_flight_grace_elapsed = true;
    if (capture_sequence_is_settled(no_grace) && capture_finished_short(no_grace)) {
      ++lost_without_grace;
    }

    // With a grace, the capture keeps waiting, the buffer arrives, and the
    // capture settles complete before the grace runs out.
    check("S20+ replay: short capture waits out its grace",
          !capture_sequence_is_settled(at_sequence_end));
    CaptureSequenceProgress buffer_landed = at_sequence_end;
    buffer_landed.arrived = 1;
    if (!capture_sequence_is_settled(buffer_landed) ||
        capture_finished_short(buffer_landed)) {
      ++lost_with_grace;
    }
    check("S20+ replay: no debt either way",
          capture_outstanding_payload_debt(at_sequence_end) == 0 &&
              capture_outstanding_payload_debt(buffer_landed) == 0);
  }
  check("S20+ replay: settling on sequence end alone loses every capture",
        lost_without_grace == captures);
  check("S20+ replay: the grace loses none", lost_with_grace == 0);

  // Control arm -- camera 1 ordering, buffer first. Must deliver with or
  // without a grace, so the fix cannot be what makes that camera work.
  CaptureSequenceProgress buffer_first = in_grace(1, 1, 0);
  check("S20+ control: buffer-before-completion delivers during grace",
        capture_sequence_is_settled(buffer_first) && !capture_finished_short(buffer_first));
  check("S20+ control: buffer-before-completion delivers without grace",
        capture_sequence_is_settled(progress(1, 1, 0, true)) &&
            !capture_finished_short(progress(1, 1, 0, true)));
}

void run_debt_is_exactly_the_shortfall_check() {
  // Debt must be the shortfall, never the expected count: over-recording
  // starves later captures just as effectively as recording when nothing is
  // owed.
  for (size_t expected = 1; expected <= 5; ++expected) {
    for (size_t arrived = 0; arrived <= expected; ++arrived) {
      const CaptureSequenceProgress p = progress(expected, arrived, 0, false);
      const uint64_t want = static_cast<uint64_t>(expected - arrived);
      check("open sequence debt equals shortfall (expected=" + std::to_string(expected) +
                " arrived=" + std::to_string(arrived) + ")",
            capture_outstanding_payload_debt(p) == want);

      const CaptureSequenceProgress ended = progress(expected, arrived, 0, true);
      check("ended sequence never owes (expected=" + std::to_string(expected) +
                " arrived=" + std::to_string(arrived) + ")",
            capture_outstanding_payload_debt(ended) == 0);
    }
  }
}

void run_overdelivery_check() {
  // More payloads than expected must not produce negative debt via unsigned
  // wraparound, and must not report the capture short.
  expect("more arrived than expected", progress(1, 2, 0, false), true, 0, false);
  expect("more arrived than expected, sequence ended", progress(1, 3, 0, true), true, 0,
         false);
  // Arrived plus failed exceeding expected is the same shape.
  expect("arrived and failed exceed expected", progress(2, 2, 1, false), true, 0, false);
}

void run_degenerate_expectation_check() {
  // A capture expecting nothing is trivially settled and owes nothing. Guards
  // the boundary rather than asserting it is a legal request.
  expect("expects nothing, sequence open", progress(0, 0, 0, false), true, 0, false);
  expect("expects nothing, sequence ended", progress(0, 0, 0, true), true, 0, false);
}

void run_starvation_regression_check() {
  // Replays the measured Quest 3 four-capture run against the policy, with a
  // ledger-shaped debt counter, and asserts the sequence cannot starve.
  //
  // The device produces one image fewer than requested on capture 2 and
  // delivers its own payload promptly on every other capture. Under the
  // timeout-only model this yielded 2 -> 0 -> 0 -> 0; the policy must yield one
  // lost capture and full recovery.
  struct CaptureOutcome {
    bool delivered = false;
  };

  const bool device_produces_image[4] = {true, false, true, true};
  uint64_t outstanding_debt = 0;
  CaptureOutcome outcomes[4];

  for (int i = 0; i < 4; ++i) {
    size_t arrived = 0;
    if (device_produces_image[i]) {
      // A payload arrives. If the device is owed against, the debt consumes it
      // and the capture sees nothing -- this is the ledger's discard rule.
      if (outstanding_debt > 0) {
        --outstanding_debt;
      } else {
        arrived = 1;
      }
    }
    // Every submission's sequence ends, delivered or not: that is what the
    // platform reported on hardware.
    const CaptureSequenceProgress p = progress(1, arrived, 0, /*sequence_ended=*/true);
    check("replay capture settles", capture_sequence_is_settled(p));
    outstanding_debt += capture_outstanding_payload_debt(p);
    outcomes[i].delivered = !capture_finished_short(p);
  }

  check("replay: capture 1 delivered", outcomes[0].delivered);
  check("replay: capture 2 lost (the device produced no image)", !outcomes[1].delivered);
  check("replay: capture 3 recovered", outcomes[2].delivered);
  check("replay: capture 4 recovered", outcomes[3].delivered);
  check("replay: no debt outstanding at the end", outstanding_debt == 0);

  // And the counterfactual: with sequence end ignored, the same device starves.
  // This is what makes the check a regression test rather than a restatement.
  outstanding_debt = 0;
  bool delivered_after_loss = false;
  for (int i = 0; i < 4; ++i) {
    size_t arrived = 0;
    if (device_produces_image[i]) {
      if (outstanding_debt > 0) {
        --outstanding_debt;
      } else {
        arrived = 1;
      }
    }
    const CaptureSequenceProgress p = progress(1, arrived, 0, /*sequence_ended=*/false);
    outstanding_debt += capture_outstanding_payload_debt(p);
    if (i > 1 && !capture_finished_short(p)) {
      delivered_after_loss = true;
    }
  }
  check("counterfactual: ignoring sequence end starves every later capture",
        !delivered_after_loss && outstanding_debt > 0);
}

}  // namespace

int main() {
  run_complete_capture_checks();
  run_still_waiting_checks();
  run_sequence_end_settlement_checks();
  run_in_flight_grace_checks();
  run_s20p_late_buffer_regression_check();
  run_debt_is_exactly_the_shortfall_check();
  run_overdelivery_check();
  run_degenerate_expectation_check();
  run_starvation_regression_check();

  if (g_failed != 0) {
    std::cout << "FAIL capture_sequence_settlement_verify run=" << g_run
              << " failed=" << g_failed << "\n";
    return 1;
  }
  std::cout << "PASS capture_sequence_settlement_verify run=" << g_run << " ok=" << g_run
            << " failed=0\n";
  return 0;
}
