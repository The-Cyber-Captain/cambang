#pragma once

// Acquisition coexistence: what a backend can serve concurrently on one device.
//
// The question Core needs answered before it can arbitrate between a triggered
// capture and a repeating stream (arbitration_policy.md §2, §6.2) is not "is
// this still profile supported" but "can these uses be concurrent on this
// device, and if not, what has to give". The two implemented backends answer
// differently in kind, which is why this is asked rather than assumed:
//
//   - android_camera2 fixes a session's whole output set at creation, so the
//     question is only answerable of a SET. A session may hold a stream output
//     and a still output at different geometries; what it cannot do is gain an
//     output without a rebuild, and a rebuild cancels the repeating request.
//   - windows_winrt shares one active MediaFrameFormat across every reader on a
//     MediaFrameSource, so on a shared-pin device two geometries genuinely
//     cannot be live at once and something must yield.
//
// A per-profile question would be unanswerable for the first and misleading for
// the second. Hence one query, over a proposed concurrent use set.
//
// SYMMETRY. The set carries no notion of who is asking, and deliberately so: a
// stream starting under a retained still profile and a still profile retained
// under a live stream are the same hardware question. Priority between the two
// is Core's decision and does not belong here -- this header is capability
// truth only. Nothing in it mentions preemption, and nothing in it should.
//
// ASKED FROM WHERE THE DEVICE IS NOW. Coexist and Unsupported are properties of
// the set alone, but Reconfigure and StreamMustYield are answers about the cost
// of getting there from the device's current configuration -- the same set can
// be free to reach from one state and disturbing to reach from another. On
// Camera2 that is the difference between a still output declared when the
// session was built and one that needs a rebuild to add. So a provider answers
// from its own live session state as well as from static characteristics, which
// is still no backend I/O: both are already in hand.
//
// NO I/O. Answering must not touch the backend: brief §2 forbids I/O in a
// capability query on the core thread, and Core consults this at profile-set,
// stream start and capture admission. Both platform providers cache the static
// characteristics they need at device open (Camera2's
// INFO_SUPPORTED_HARDWARE_LEVEL and StreamConfigurationMap, WinRT's
// SupportedFormats), so the answer is a pure function of those and the proposed
// set -- which is also what brief §7.1 requires of it.

// WHAT THE UNIMPLEMENTED SEAMS SHOULD EXPECT TO ANSWER. Taken at the documented
// face value of each API and NOT VERIFIED against any of them -- there is no
// Linux, macOS, iOS or web surface in this repo's validation matrix. Recorded
// so an implementer starts from the shape of the question rather than
// rediscovering it, and it is a starting point to check, never a specification.
//
//   - linux_v4l2. One active format per device node, and VIDIOC_S_FMT is
//     refused with EBUSY while streaming. Closest to windows_winrt: expect
//     StreamMustYield for differing geometries. A device exposing several
//     nodes could answer Coexist instead, so this is per-device, not per-family.
//   - apple_avfoundation. An AVCaptureSession can hold a photo output and a
//     video output at once, with the photo path delivering independently of the
//     video output's dimensions, so expect Coexist across a wider range than
//     either implemented backend. An activeFormat change does interrupt, which
//     is Reconfigure.
//   - web_getusermedia. applyConstraints() can retarget a live track and
//     ImageCapture.takePhoto() carries its own photoSettings, but engine
//     support varies enough that the honest answer is whatever the running
//     engine actually offers. Where that cannot be established, answer
//     conservatively: claiming coexistence and then failing is the one outcome
//     Core cannot recover from.

#include <cstdint>

#include "imaging/api/provider_contract_datatypes.h"

namespace cambang {

// What happens if the proposed uses are asked for concurrently.
enum class CoexistenceVerdict : uint8_t {
  // Both served, nothing disturbed. Any provider with no backend constraint
  // answers this, and so does a constrained one whose backend happens to be
  // able to serve the specific set proposed.
  Coexist = 0,

  // Both served, but reaching that state interrupts the stream: frames gap and
  // resume. The stream object survives and keeps its own profile.
  Reconfigure = 1,

  // Not concurrent at all. To serve the still, the stream must stop.
  StreamMustYield = 2,

  // Not serviceable at these shapes in any order -- neither concurrently nor by
  // stopping anything. This is a CAPABILITY denial and must never be reported
  // as, or converted into, a priority decision: no amount of yielding reaches
  // it, so preempting a stream on the strength of this verdict would destroy a
  // working stream for a capture that was never going to succeed.
  Unsupported = 3,
};

// A proposed set of concurrent uses on one device.
//
// Absent members mean "not proposed", not "don't care": a set with only a
// stream asks whether that stream alone is serviceable, which is a question
// Camera2 can answer differently from the same stream alongside a still.
struct AcquisitionUseSet {
  bool has_stream = false;
  CaptureProfile stream{};

  bool has_still = false;
  CaptureProfile still{};
};

struct AcquisitionCoexistence {
  CoexistenceVerdict verdict = CoexistenceVerdict::Coexist;

  // Whether a stream stopped to satisfy StreamMustYield can afterwards be
  // restored at its own profile. Core cannot honour the brief's guarantee that
  // public stream objects remain valid across preemption without knowing this,
  // and a provider that cannot restore must say so rather than let Core promise
  // a restart it will then refuse.
  //
  // Meaningful only for StreamMustYield. The other three verdicts stop nothing,
  // so there is nothing to restore and the field is not consulted.
  bool yielded_stream_restorable = true;

  static constexpr AcquisitionCoexistence coexist() noexcept {
    return AcquisitionCoexistence{CoexistenceVerdict::Coexist, true};
  }
  static constexpr AcquisitionCoexistence reconfigure() noexcept {
    return AcquisitionCoexistence{CoexistenceVerdict::Reconfigure, true};
  }
  static constexpr AcquisitionCoexistence stream_must_yield(
      bool restorable) noexcept {
    return AcquisitionCoexistence{CoexistenceVerdict::StreamMustYield, restorable};
  }
  static constexpr AcquisitionCoexistence unsupported() noexcept {
    return AcquisitionCoexistence{CoexistenceVerdict::Unsupported, false};
  }
};

// True when the proposed uses can be live together, whatever it costs to reach
// that state. Reconfigure counts: the interruption is transient and both uses
// are served on the far side of it.
constexpr bool coexistence_permits_concurrent_use(
    const AcquisitionCoexistence& c) noexcept {
  return c.verdict == CoexistenceVerdict::Coexist ||
         c.verdict == CoexistenceVerdict::Reconfigure;
}

// True when reaching the proposed state disturbs a live stream at all, which is
// the set of verdicts Core must account for truthfully to the caller rather
// than apply silently.
constexpr bool coexistence_disturbs_stream(
    const AcquisitionCoexistence& c) noexcept {
  return c.verdict == CoexistenceVerdict::Reconfigure ||
         c.verdict == CoexistenceVerdict::StreamMustYield;
}

// True when no ordering of operations serves the set. Kept separate from
// "cannot coexist" precisely because the two are so easily conflated: a
// StreamMustYield capture succeeds once the stream stops, an Unsupported one
// never succeeds.
constexpr bool coexistence_is_capability_denial(
    const AcquisitionCoexistence& c) noexcept {
  return c.verdict == CoexistenceVerdict::Unsupported;
}

constexpr const char* coexistence_verdict_name(CoexistenceVerdict v) noexcept {
  switch (v) {
    case CoexistenceVerdict::Coexist:
      return "coexist";
    case CoexistenceVerdict::Reconfigure:
      return "reconfigure";
    case CoexistenceVerdict::StreamMustYield:
      return "stream_must_yield";
    case CoexistenceVerdict::Unsupported:
      return "unsupported";
  }
  return "unknown";
}

}  // namespace cambang
