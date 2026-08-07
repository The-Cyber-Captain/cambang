#pragma once

#include <cstdint>

namespace cambang {

// Who holds an acquisition seam, and what that permits.
//
// Why this exists
// ---------------
// lifecycle_model.md §2 gives the seam three independent claimants -- a stream,
// a device capture, and the capture parent -- and providers must be able to say
// which of them currently do. The bookkeeping is trivial; the DECISIONS taken
// from it are not, and getting one wrong is silent:
//
//   * A capture retains its claim at admission and then asks for the geometry
//     it was admitted for. Consulting the raw counts refuses the capture on the
//     strength of its own claim -- a self-block that looks exactly like a
//     hardware constraint from the outside.
//   * The capture-parent claim exists so a retained still profile can shape the
//     seam. Letting it pin geometry stops it doing its own job.
//   * Zero references makes teardown PERMITTED, not mandatory. Core's creation
//     call site holds this claim for as long as the retained still profile
//     stands (CoreRuntime::sync_capture_parent_priming_ sets
//     provider_hold_active and does not release; the releases live in
//     refresh_capture_retained_plan_state_). A provider that tore the seam
//     down when that claim is finally dropped would destroy it under a
//     profile that is merely changing.
//
// The decisions are pure functions of three counts and the asker's identity, so
// they live here and are exercised host-native -- the same arrangement as
// capture_sequence_settlement.h, and for the same reason: the providers that
// use them are platform-only and unreachable from any deterministic verifier.
//
// This header decides nothing about WHAT a seam is. That mapping is per
// provider and belongs in provider source.

// The claimant asking for an operation. Needed because a claimant must never be
// blocked by its own claim.
enum class SeamClaimant : uint8_t { Stream, Capture, CaptureParent };

// What a device's seam is currently held by.
struct SeamClaims {
  // Streams whose session realization is live. Counter: several may hold.
  uint32_t stream_refs = 0;
  // Captures admitted and not yet terminal. Counter, for the same reason.
  uint32_t capture_refs = 0;
  // Retained-profile claim. A LATCH (0 or 1), never a counter: the contract
  // requires repeated equivalent priming calls to be idempotent and Core issues
  // one on every profile-set, so counting them would pin the seam with claims
  // no caller can ever release.
  uint32_t capture_parent_refs = 0;
};

// Is anything holding the seam at all?
constexpr bool seam_is_held(const SeamClaims& c) noexcept {
  return c.stream_refs != 0 || c.capture_refs != 0 || c.capture_parent_refs != 0;
}

// May the seam's native object be destroyed?
//
// All three claimants count here: destroying the object underneath any of them
// breaks it. Note what this does NOT say -- that it should be destroyed. Zero
// references makes teardown permitted, and a provider may keep a warm seam.
constexpr bool seam_teardown_permitted(const SeamClaims& c) noexcept {
  return !seam_is_held(c);
}

// Whether the asker's own claim is already counted in `c`, and must therefore
// be discounted before asking whether anyone ELSE objects.
//
// This is stated by the caller rather than inferred from the claimant, because
// it is a property of the provider's call ordering and providers differ:
// a Camera2 stream retains before requesting realization, while a WinRT stream
// realizes first and retains only once started. Inferring it would be right for
// one provider and wrong for the other, silently.
enum class OwnClaim : uint8_t { NotHeld, AlreadyHeld };

namespace detail {

// The asker's own claim, taken off ITS OWN category and only when there is one.
// Never off the total: subtracting from a sum cancels whichever claim happens
// to be counted first, so an asker holding nothing would excuse somebody else.
constexpr SeamClaims without_own_claim(const SeamClaims& c, SeamClaimant requester,
                                       OwnClaim own) noexcept {
  SeamClaims out = c;
  if (own != OwnClaim::AlreadyHeld) {
    return out;
  }
  switch (requester) {
    case SeamClaimant::Stream:
      if (out.stream_refs != 0) --out.stream_refs;
      break;
    case SeamClaimant::Capture:
      if (out.capture_refs != 0) --out.capture_refs;
      break;
    case SeamClaimant::CaptureParent:
      if (out.capture_parent_refs != 0) --out.capture_parent_refs;
      break;
  }
  return out;
}

}  // namespace detail

// Claims that would be disturbed by reconfiguring the seam, excluding the
// asker's own.
//
// The capture parent is excluded, and this is the load-bearing part. Its claim
// exists so a retained still profile can shape the seam; a rule that let it
// block reconfiguration would stop it doing the one thing it is for.
constexpr uint32_t seam_reconfiguration_blocking_claims(const SeamClaims& c,
                                                        SeamClaimant requester,
                                                        OwnClaim own) noexcept {
  const SeamClaims others = detail::without_own_claim(c, requester, own);
  return others.stream_refs + others.capture_refs;
}

// May the seam be reconfigured by this claimant?
//
// "Reconfigured" covers BOTH shapes this takes across backends, deliberately:
//
//   * a change applied in place, leaving the seam's native object alive --
//     WinRT's SetFormatAsync on the MediaFrameSource;
//   * a change that can only be made by replacing that object, which ends one
//     seam and begins another with both facts reported -- Camera2, whose
//     ACameraCaptureSession fixes its output set at creation, and WinRT again
//     when the reader's subtype must change.
//
// An earlier version of this header split those into separate questions and
// consulted the capture-parent latch for the second. That was wrong, and wrong
// in a way that could only bite one provider: on WinRT the two are genuinely
// different operations, but on Camera2 EVERY geometry change is a replacement,
// so the capture-parent exclusion above never applied there and the latch
// blocked exactly what it was written not to block. With a retained still
// profile in force, a stream starting at a different geometry was refused.
//
// What justifies excluding the capture parent from both is that neither shape
// leaves Core believing a primed seam exists when none does: an in-place change
// keeps the seam, and a replacement reports the destruction and the creation.
// Only an outright teardown does that, and teardown is the separate question
// below.
constexpr bool seam_reconfiguration_permitted(const SeamClaims& c,
                                              SeamClaimant requester,
                                              OwnClaim own) noexcept {
  return seam_reconfiguration_blocking_claims(c, requester, own) == 0;
}

// Claims that would be disturbed by a PRESERVING REPROVISION, excluding the
// asker's own. Only in-flight captures count; see below for why.
constexpr uint32_t seam_reprovision_blocking_claims(const SeamClaims& c,
                                                    SeamClaimant requester,
                                                    OwnClaim own) noexcept {
  const SeamClaims others = detail::without_own_claim(c, requester, own);
  return others.capture_refs;
}

// May the seam be REPROVISIONED by this claimant?
//
// A reprovision is a capture-session reconfiguration that CARRIES EVERY OTHER
// CLAIMANT'S OUTPUT INTO THE NEW CONFIGURATION. The native object is replaced,
// but nothing else's needs are dropped: a stream is interrupted, not discarded,
// and resumes at its own geometry on the far side.
//
// That is a third shape, and the reason it needs its own question.
// seam_reconfiguration_permitted above covers an in-place change and a
// replacement together, which is right while both destroy what the other
// claimant depended on. Here the other claimant survives, so a live stream is
// not a reason to refuse.
//
// WHAT BLOCKS IT, and what does not:
//
//   * A STREAM does not block. It gaps while the session is swapped and comes
//     back at its own geometry, so the reprovision costs it frames, not its
//     configuration. Whether that gap is acceptable is a PRIORITY decision and
//     belongs to Core (arbitration_policy.md §2), not here -- this header only
//     answers whether the operation is coherent.
//   * An IN-FLIGHT CAPTURE blocks. Its request is bound to the session being
//     replaced, so the image it is waiting for would never arrive. No amount of
//     carrying outputs forward saves a request that was already submitted.
//   * The CAPTURE PARENT does not block, on the same grounds as reconfiguration:
//     the retained-profile claim exists to shape the seam, and a reprovision
//     reports the destroyed/created pair, so Core is never left believing a
//     primed seam exists when none does.
//
// The asker's own claim is discounted on exactly the terms used above.
//
// THE PRESERVING PRECONDITION IS ASSERTED BY THE CALLER, NOT CHECKED HERE. This
// header knows three counts; it cannot see whether outputs were actually carried
// forward. A provider that asks this question and then drops another claimant's
// output has violated the contract, not found a loophole, and
// provider_compliance_verify binds that.
//
// WHY THE TWO QUESTIONS ARE NOT INTERCHANGEABLE. They map onto real and
// different backend constraints. A WinRT MediaFrameSource carries one active
// format shared by every reader, so a geometry change genuinely takes the
// stream's configuration away -- WinRT cannot preserve, and keeps asking
// seam_reconfiguration_permitted. A Camera2 session can hold a stream output and
// a still output at different geometries, so it can preserve, and asks this.
// Reaching for this question on a backend that cannot preserve would be a lie
// with no symptom until a caller's stream came back wrong.
//
// NOT AN ALTERNATIVE: releasing another claimant's reference around the
// operation so the raw counts permit it. That falsifies the counts while the
// claimant still depends on the seam, and opens a window in which a third party
// could tear it down outright. The counts being true is what this header is for.
constexpr bool seam_reprovision_permitted(const SeamClaims& c,
                                          SeamClaimant requester,
                                          OwnClaim own) noexcept {
  return seam_reprovision_blocking_claims(c, requester, own) == 0;
}

// May the seam be torn down outright -- destroyed with nothing put in its
// place?
//
// Every claimant blocks this, the capture parent included, because a primed
// seam that simply vanishes leaves Core believing one exists when none does.
// That is the case seam_reconfiguration_permitted is NOT, and the reason the
// two are separate questions rather than one.
//
// The asker's own claim is still discounted: a claimant that holds the only
// reference may release the seam it alone depends on.
constexpr bool seam_teardown_permitted_by(const SeamClaims& c, SeamClaimant requester,
                                          OwnClaim own) noexcept {
  const SeamClaims others = detail::without_own_claim(c, requester, own);
  return !seam_is_held(others);
}

}  // namespace cambang
