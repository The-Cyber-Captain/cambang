/*
CamBANG Maintainer Utility

Tool: outstanding_payload_ledger_verify

Purpose
-------
Deterministically verifies OutstandingPayloadLedger, the accounting that stops a
capture payload delivered late -- after the capture that requested it was
abandoned -- from being adopted by a later capture on the same device.

The defect this guards against was observed on Quest 3 / Camera2: a still-only
session withheld a buffer past the sample wait and released it when the next
request pushed the pipeline. Without the ledger the next capture adopted it and
the device ran one image behind indefinitely, every capture reporting success.

Scope honesty
-------------
This verifies the ledger's semantics, host-native and deterministically. It does
NOT verify the provider wiring that calls it -- that a Camera2 arrival really is
routed through claim_arrival_for_debt() before reaching a collector. That path
requires the device.

Category
--------
Verification tool (maintainer/CI).
*/

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#if !defined(CAMBANG_INTERNAL_SMOKE)
  #error "outstanding_payload_ledger_verify: build through the repo SCons maintainer_tools alias so CAMBANG_INTERNAL_SMOKE=1 is defined."
#endif

#include "imaging/api/outstanding_payload_ledger.h"

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

// A capture that completes fully owes nothing, and a later arrival is therefore
// the next capture's to keep.
void case_complete_capture_owes_nothing() {
  OutstandingPayloadLedger ledger;
  ledger.record_abandoned(/*expected=*/1, /*accounted=*/1);
  check(ledger.outstanding() == 0, "complete capture leaves no debt");
  check(!ledger.claim_arrival_for_debt(), "arrival is not claimed when nothing is owed");
  check(ledger.settled_total() == 0, "nothing settled when nothing was owed");
}

// The Quest 3 shape: one member expected, none arrived, none explicitly failed.
// The next arrival must be discarded, and the one after that must be kept.
void case_abandoned_capture_consumes_exactly_one_late_payload() {
  OutstandingPayloadLedger ledger;
  ledger.record_abandoned(/*expected=*/1, /*accounted=*/0);
  check(ledger.outstanding() == 1, "abandoned single-member capture owes one payload");

  check(ledger.claim_arrival_for_debt(), "the late payload is claimed for debt");
  check(ledger.outstanding() == 0, "debt is discharged by that one payload");
  check(ledger.settled_total() == 1, "the discard is counted");

  check(!ledger.claim_arrival_for_debt(),
        "the NEXT arrival belongs to the current capture and is not discarded");
}

// A member the platform explicitly failed owes nothing: no payload is in flight
// for it, so discarding a later arrival would eat a genuine one.
void case_explicitly_failed_members_owe_nothing() {
  OutstandingPayloadLedger ledger;
  // Three members: one arrived, two explicitly failed. Nothing outstanding.
  ledger.record_abandoned(/*expected=*/3, /*accounted=*/3);
  check(ledger.outstanding() == 0, "explicitly-failed members owe nothing");
  check(!ledger.claim_arrival_for_debt(), "no discard after an all-accounted burst");
}

// A partially collected bracket owes only its shortfall.
void case_partial_burst_owes_only_the_shortfall() {
  OutstandingPayloadLedger ledger;
  ledger.record_abandoned(/*expected=*/5, /*accounted=*/2);
  check(ledger.outstanding() == 3, "five expected, two accounted -> three owed");

  int discarded = 0;
  for (int i = 0; i < 10; ++i) {
    if (ledger.claim_arrival_for_debt()) {
      ++discarded;
    }
  }
  check(discarded == 3, "exactly the shortfall is discarded, never more");
  check(ledger.outstanding() == 0, "debt fully discharged");
  check(ledger.settled_total() == 3, "settled count matches the shortfall");
}

// Debt accumulates across consecutive abandonments rather than being replaced.
void case_debt_accumulates() {
  OutstandingPayloadLedger ledger;
  ledger.record_abandoned(1, 0);
  ledger.record_abandoned(2, 0);
  check(ledger.outstanding() == 3, "successive abandonments accumulate");
}

// The positional-pairing guard: an unmatched payload may only be attributed
// positionally when the device owed nothing for the whole window.
void case_pairing_guard() {
  OutstandingPayloadLedger clean;
  const auto clean_before = clean.snapshot();
  check(clean.owed_nothing_since(clean_before),
        "a device that never owed anything permits positional pairing");

  OutstandingPayloadLedger owing;
  owing.record_abandoned(1, 0);
  const auto owing_before = owing.snapshot();
  check(!owing.owed_nothing_since(owing_before),
        "debt outstanding at burst start forbids positional pairing");

  // Debt discharged *during* the window is equally disqualifying: a payload
  // from the abandoned capture was in play while this capture collected.
  OutstandingPayloadLedger settling;
  settling.record_abandoned(1, 0);
  (void)settling.claim_arrival_for_debt();
  const auto after_settle = settling.snapshot();
  check(settling.owed_nothing_since(after_settle),
        "a window opened after settlement is clean");

  OutstandingPayloadLedger during;
  const auto during_before = during.snapshot();
  during.record_abandoned(1, 0);
  (void)during.claim_arrival_for_debt();
  check(!during.owed_nothing_since(during_before),
        "debt discharged during the window forbids positional pairing");
}

// Device teardown must not leave debt for a later reopen to inherit.
void case_forgive_on_teardown() {
  OutstandingPayloadLedger ledger;
  ledger.record_abandoned(4, 1);
  check(ledger.forgive_all() == 3, "forgive_all reports what it forgave");
  check(ledger.outstanding() == 0, "no debt survives teardown");
  check(!ledger.claim_arrival_for_debt(), "a reopened device discards nothing");
}

// Arrivals land on platform callback threads while a worker records shortfalls.
// Under contention the ledger must still discard exactly the amount owed --
// never more (which would eat genuine payloads) and never fewer (which would
// let a stale one through).
void case_concurrent_arrivals_discard_exactly_the_debt() {
  constexpr uint64_t kDebt = 500;
  constexpr int kThreads = 8;
  constexpr int kArrivalsPerThread = 500;

  OutstandingPayloadLedger ledger;
  ledger.record_abandoned(kDebt, 0);

  std::atomic<uint64_t> discarded{0};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&ledger, &discarded]() {
      for (int i = 0; i < kArrivalsPerThread; ++i) {
        if (ledger.claim_arrival_for_debt()) {
          discarded.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& th : threads) {
    th.join();
  }

  check(discarded.load() == kDebt, "concurrent arrivals discard exactly the debt");
  check(ledger.outstanding() == 0, "no debt left after concurrent settlement");
  check(ledger.settled_total() == kDebt, "settled count is exact under contention");
}

} // namespace

int main() {
  case_complete_capture_owes_nothing();
  case_abandoned_capture_consumes_exactly_one_late_payload();
  case_explicitly_failed_members_owe_nothing();
  case_partial_burst_owes_only_the_shortfall();
  case_debt_accumulates();
  case_pairing_guard();
  case_forgive_on_teardown();
  case_concurrent_arrivals_discard_exactly_the_debt();

  if (g_failed != 0) {
    std::fprintf(stdout, "FAIL outstanding_payload_ledger_verify run=%d ok=%d failed=%d\n",
                 g_run, g_run - g_failed, g_failed);
    std::fflush(stdout);
    return 1;
  }
  std::fprintf(stdout, "PASS outstanding_payload_ledger_verify run=%d ok=%d failed=0\n",
               g_run, g_run);
  std::fflush(stdout);
  return 0;
}
