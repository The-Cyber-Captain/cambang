#pragma once

#include <cstddef>
#include <cstdint>

namespace cambang {

// When a device capture is over, and what it still owes.
//
// Why this exists
// ---------------
// A capture collects N member payloads. Deciding it is finished by waiting a
// fixed sample window and then counting what turned up conflates two very
// different situations: a payload that has not arrived *yet*, and a payload that
// is never going to arrive. Only the first leaves the platform holding
// something, and only the first should leave a debt behind (see
// outstanding_payload_ledger.h for what that debt then does).
//
// Getting it wrong compounds. A device that produces one image fewer than it was
// asked for leaves a debt nothing can ever pay: the next capture's own payload
// discharges it, which leaves that capture short, which records the debt again.
// Measured on Quest 3 as a rig delivering 2 -> 0 -> 0 -> 0 over four captures,
// submissions and arrivals differing by exactly one for the entire run. The
// device had lost a single frame; the accounting turned that into total,
// permanent starvation. Settling on the platform's own end-of-sequence signal
// instead restored 2 -> 0 -> 2 -> 2.
//
// The signal, and its limits
// --------------------------
// Backends that can say "this submission is over" should say so.
// Camera2 does, via onCaptureSequenceCompleted/onCaptureSequenceAborted.
//
// NdkCameraCaptureSession.h defines sequence completion over capture *results
// and failures* and says nothing about buffers, so this is not a contractual
// guarantee that every buffer has already landed.
//
// It once said here that where sequence end proves early the capture is merely
// reported short -- "the same outcome the timeout produced anyway, only sooner".
// That was wrong, and measurement disproved it. Sequence end is authoritative
// about what the platform still OWES, and not about what it is still HANDING
// OVER. Those are separate questions and this header now answers them
// separately:
//
//   debt      settled by sequence end alone. Nothing more is coming for this
//             submission, so nothing is outstanding.
//   waiting   settled by sequence end PLUS a bounded grace, because a buffer
//             whose delivery callback is already running is still on its way.
//
// Why the grace is not optional. On Galaxy S20+ camera 0 the buffer callback
// and onCaptureSequenceCompleted fire 7-13ms apart, completion second. Settling
// on completion alone snapshots the collector while the arriving image is still
// being converted, and the image lands in a collector nobody reads: 21 of 30
// captures lost, every loss with completion falling between arrival and
// collection and no delivered capture doing so. Camera 1 of the same device
// fires them in the opposite order and lost nothing, as did Quest 3, where
// buffers preceded sequence end by 19-50ms in every observed capture. Ordering
// between the two callbacks is a per-camera property and must not be assumed.
//
// The grace costs nothing on a capture that got everything it asked for, and is
// paid only by one that is genuinely short. A payload arriving after the grace
// finds no collector installed and is discarded, which is the behaviour a truly
// late payload needs regardless.
//
// Acquisition timing is not an input here and must not become one:
// camera_fact_model.md 12.2 forbids using acquisition marks for ordering,
// freshness or ownership, and marks may legitimately be identical across
// simultaneously triggered devices.

// What a capture's collector knows when it is asked whether it is done.
struct CaptureSequenceProgress {
  // Members the capture asked the platform for.
  size_t expected = 0;
  // Payloads that have arrived for it.
  size_t arrived = 0;
  // Members the platform explicitly reported as failed. A failed member owes
  // nothing: the platform has already accounted for it.
  size_t failed = 0;
  // The platform has reported this submission's sequence as completed or
  // aborted. Backends with no such signal leave this false and fall back to
  // the caller's wait window.
  bool sequence_ended = false;
  // The grace allowed after sequence end for buffers already in flight has run
  // out. Meaningless while sequence_ended is false. A backend that can prove no
  // buffer is in flight may set it with sequence_ended in the same breath; one
  // that cannot -- Camera2 cannot -- must let a timer decide.
  bool in_flight_grace_elapsed = false;
};

// Members the capture has heard about one way or the other.
constexpr size_t capture_accounted_members(const CaptureSequenceProgress& p) noexcept {
  return p.arrived + p.failed;
}

// Whether the capture can stop waiting.
//
// Every member accounted for is the ordinary case, and needs no grace: there is
// nothing left to wait for.
//
// Otherwise the sequence must have ended AND its in-flight grace expired. Ending
// alone is not enough -- see the header note; a buffer mid-delivery at the moment
// of sequence end is lost by anything that stops waiting right then.
constexpr bool capture_sequence_is_settled(const CaptureSequenceProgress& p) noexcept {
  return capture_accounted_members(p) >= p.expected ||
         (p.sequence_ended && p.in_flight_grace_elapsed);
}

// Whether the capture should start its in-flight grace: the platform has closed
// the submission but members are still missing. Distinct from settlement so a
// caller can arm a timer at the right moment rather than polling.
constexpr bool capture_awaits_in_flight_grace(const CaptureSequenceProgress& p) noexcept {
  return p.sequence_ended && !p.in_flight_grace_elapsed &&
         capture_accounted_members(p) < p.expected;
}

// How many payloads remain outstanding with the platform once this capture
// stops waiting -- the debt to record, and zero when there is none.
//
// A capture is short whenever fewer members were accounted for than expected,
// but short is not the same as owed. If the sequence has ended, the platform is
// not going to deliver any more for this submission, so nothing is outstanding
// however short the capture was. Only a capture that gave up while its sequence
// was still open leaves an obligation behind.
//
// The in-flight grace is deliberately NOT consulted here. Grace governs how long
// to keep waiting; it does not change who owes what. A capture that waited out
// its grace and still came up short owes nothing, exactly as one that settled the
// instant its sequence ended would have.
constexpr uint64_t capture_outstanding_payload_debt(
    const CaptureSequenceProgress& p) noexcept {
  const size_t accounted = capture_accounted_members(p);
  if (accounted >= p.expected || p.sequence_ended) {
    return 0;
  }
  return static_cast<uint64_t>(p.expected - accounted);
}

// Whether the capture finished short. Independent of whether anything is owed:
// a capture can be truthfully short and owe nothing, which is exactly the case
// the timeout-only model could not express.
constexpr bool capture_finished_short(const CaptureSequenceProgress& p) noexcept {
  return capture_accounted_members(p) < p.expected;
}

}  // namespace cambang
