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
//     call site is gated on "no session exists" and releases immediately
//     afterwards, so a provider that tore down on release would destroy the
//     seam microseconds after creating it.
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

// Claims that would be disturbed by changing the seam's geometry, from the
// point of view of `requester`.
//
// Geometry is a different question from teardown and reads a different set:
//
//   * The capture parent is excluded entirely. Its purpose is to set geometry
//     from the retained profile.
//   * The asker's own claim is discounted, and only for a capture. A capture
//     holds its claim from admission and then asks for the geometry it was
//     admitted for. A stream takes its claim only once started -- after the
//     geometry call has already succeeded -- so it has nothing of its own to
//     discount, and discounting one anyway would let it reconfigure out from
//     under a different live stream.
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

// Claims that would be disturbed by a geometry change, excluding the asker's.
constexpr uint32_t seam_geometry_pinning_claims(const SeamClaims& c,
                                                SeamClaimant requester,
                                                OwnClaim own) noexcept {
  const SeamClaims others = detail::without_own_claim(c, requester, own);
  return others.stream_refs + others.capture_refs;
}

// May the seam's geometry be changed by this claimant?
constexpr bool seam_geometry_change_permitted(const SeamClaims& c,
                                              SeamClaimant requester,
                                              OwnClaim own) noexcept {
  return seam_geometry_pinning_claims(c, requester, own) == 0;
}

// May the seam's native object be REPLACED by this claimant -- retired and
// re-created, ending one seam and beginning another?
//
// Unlike a geometry change this destroys the object, so every claimant counts,
// including the capture parent: a primed seam that vanished under Core would
// leave it believing a seam exists when none does. The asker's own claim is
// still discounted on the same terms as above.
// The capture parent is discounted here too when it already holds its latch: it
// retains and THEN asks for a seam matching the retained profile, so refusing
// on its own latch would make the capture-parent path refuse itself. The
// symptom is a device that can never reach the profile it was given, and it
// keeps the wrong geometry silently.
constexpr bool seam_replacement_permitted(const SeamClaims& c, SeamClaimant requester,
                                          OwnClaim own) noexcept {
  const SeamClaims others = detail::without_own_claim(c, requester, own);
  return !seam_is_held(others);
}

}  // namespace cambang
