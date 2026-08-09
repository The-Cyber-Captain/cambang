#pragma once

#include <atomic>
#include <cstdint>

namespace cambang {

// Per-device accounting for capture payloads a platform still owes after a
// capture was abandoned before every member arrived.
//
// Why this exists
// ---------------
// A platform may deliver a still payload long after the capture that requested
// it has given up waiting. Observed on Quest 3 / Camera2: a still-only session
// with no repeating request withheld a buffer past the provider's sample wait
// and released it only when the *next* request pushed the pipeline. The next
// capture's collector then adopted it, and because a single-member capture
// pairs an unmatched payload positionally, that stale payload was handed the
// new capture's facts. The device then ran one image behind indefinitely, every
// capture reporting success.
//
// The rule this encodes: a device that abandoned a capture short owes that many
// payloads, and the next arrivals discharge the debt rather than being adopted
// by whichever capture happens to be collecting.
//
// Attribution is by counting alone. Acquisition marks must never be used to
// decide ownership, freshness, or ordering (camera_fact_model.md 12.2), and
// marks may legitimately be identical across simultaneously triggered devices,
// so they cannot discriminate here even in principle.
//
// Threading: arrivals land on platform callback threads while a capture worker
// records shortfalls, so every operation is atomic and lock-free. No ordering
// between distinct devices is implied; one ledger belongs to one device.
class OutstandingPayloadLedger {
public:
  // Point-in-time reading, used to decide whether an unmatched payload can be
  // trusted to belong to the capture that collected it.
  struct Snapshot {
    uint64_t outstanding = 0;
    uint64_t settled_total = 0;
  };

  // A capture ended with `expected` members but only `accounted` of them
  // resolved (arrived, or were explicitly failed by the platform). The
  // difference is still owed. Explicitly-failed members owe nothing: the
  // platform has said no payload is coming for them.
  void record_abandoned(uint64_t expected, uint64_t accounted) noexcept {
    if (accounted >= expected) {
      return;
    }
    outstanding_.fetch_add(expected - accounted, std::memory_order_acq_rel);
  }

  // Called for each arriving payload, before any collector can see it.
  // Returns true when the caller must discard this payload: it belongs to a
  // capture that is already over.
  bool claim_arrival_for_debt() noexcept {
    uint64_t debt = outstanding_.load(std::memory_order_relaxed);
    while (debt > 0) {
      if (outstanding_.compare_exchange_weak(debt, debt - 1,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
        settled_.fetch_add(1, std::memory_order_acq_rel);
        return true;
      }
    }
    return false;
  }

  Snapshot snapshot() const noexcept {
    Snapshot s;
    s.outstanding = outstanding_.load(std::memory_order_acquire);
    s.settled_total = settled_.load(std::memory_order_acquire);
    return s;
  }

  // True when the device owed nothing at `before` and has discharged nothing
  // since -- i.e. no payload from an abandoned capture can have been in play
  // during the window that snapshot opened. Only then may an unmatched payload
  // be attributed positionally to the capture that collected it.
  bool owed_nothing_since(const Snapshot& before) const noexcept {
    if (before.outstanding != 0) {
      return false;
    }
    return settled_.load(std::memory_order_acquire) == before.settled_total;
  }

  uint64_t outstanding() const noexcept {
    return outstanding_.load(std::memory_order_acquire);
  }
  uint64_t settled_total() const noexcept {
    return settled_.load(std::memory_order_acquire);
  }

  // Device teardown / removal from a rig. Debt cannot outlive the device it
  // belongs to: leaving it set would make a later reopen inherit discards that
  // are not its own. Returns what was forgiven, so the caller can report it
  // rather than losing it silently.
  uint64_t forgive_all() noexcept {
    return outstanding_.exchange(0, std::memory_order_acq_rel);
  }

private:
  std::atomic<uint64_t> outstanding_{0};
  std::atomic<uint64_t> settled_{0};
};

} // namespace cambang
