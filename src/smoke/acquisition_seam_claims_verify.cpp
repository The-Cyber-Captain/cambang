// Deterministic coverage for acquisition-seam claim policy
// (imaging/api/acquisition_seam_claims.h).
//
// The policy governs platform-backed providers, which are OS-only and
// unreachable from any host verifier: WinrtCameraProvider needs MSVC, WinRT and
// a camera; Camera2CameraProvider needs an Android device. The decisions
// themselves are pure, so they live in imaging/api and are exercised here --
// the same arrangement as capture_sequence_settlement_verify, and for the same
// reason.
//
// The case this exists to hold down: a capture retains its seam claim at
// ADMISSION and then asks the worker for the geometry it was admitted for. A
// policy that consults the raw counts refuses the capture on the strength of
// its own claim. That self-block is silent -- from outside it is
// indistinguishable from a genuine hardware constraint -- and it was written
// and nearly shipped during the WinRT conformance work.

#include "imaging/api/acquisition_seam_claims.h"

#include <iostream>
#include <string>

using namespace cambang;

namespace {

int g_run = 0;
int g_failed = 0;

SeamClaims claims(uint32_t stream, uint32_t capture, uint32_t capture_parent) {
  SeamClaims c{};
  c.stream_refs = stream;
  c.capture_refs = capture;
  c.capture_parent_refs = capture_parent;
  return c;
}

void check(const std::string& what, bool ok) {
  ++g_run;
  if (!ok) {
    ++g_failed;
    std::cerr << "FAIL " << what << "\n";
  }
}

// ---------------------------------------------------------------------------

void run_held_and_teardown_checks() {
  check("nothing held", !seam_is_held(claims(0, 0, 0)));
  check("stream holds", seam_is_held(claims(1, 0, 0)));
  check("capture holds", seam_is_held(claims(0, 1, 0)));
  check("capture parent holds", seam_is_held(claims(0, 0, 1)));

  // Teardown is permitted only when nothing at all holds the seam. Every
  // claimant counts: destroying the object underneath any of them breaks it.
  check("teardown permitted when unheld", seam_teardown_permitted(claims(0, 0, 0)));
  check("stream forbids teardown", !seam_teardown_permitted(claims(1, 0, 0)));
  check("capture forbids teardown", !seam_teardown_permitted(claims(0, 1, 0)));
  check("capture parent forbids teardown", !seam_teardown_permitted(claims(0, 0, 1)));
  check("many claimants forbid teardown", !seam_teardown_permitted(claims(3, 2, 1)));
}

void run_capture_parent_never_blocks_reconfiguration_checks() {
  // THE CAPTURE-PARENT RULE, and the one this file exists to hold down hardest.
  // The retained-profile claim exists so the profile can shape the seam. If it
  // blocks reconfiguration it stops the capture-parent path doing its own job.
  const SeamClaims parent_only = claims(0, 0, 1);
  for (SeamClaimant asker :
       {SeamClaimant::Stream, SeamClaimant::Capture, SeamClaimant::CaptureParent}) {
    check("capture-parent claim never blocks reconfiguration",
          seam_reconfiguration_permitted(parent_only, asker, OwnClaim::NotHeld));
  }

  // It held for an in-place change already. The case that regressed was the
  // change that REPLACES the seam's native object: on Camera2 every geometry
  // change is one of those, so a stream starting at a new geometry while a
  // retained still profile stood was refused. It must not be.
  check("capture-parent claim does not block a replacing reconfiguration",
        seam_reconfiguration_permitted(parent_only, SeamClaimant::Stream,
                                       OwnClaim::NotHeld));

  // But it DOES still block an outright teardown, which is the case that would
  // leave Core believing a primed seam exists when none does.
  check("capture-parent claim blocks teardown by a stream",
        !seam_teardown_permitted_by(parent_only, SeamClaimant::Stream,
                                    OwnClaim::NotHeld));
  check("capture-parent claim blocks teardown by a capture",
        !seam_teardown_permitted_by(parent_only, SeamClaimant::Capture,
                                    OwnClaim::AlreadyHeld));
  // ...and not its own, so the holder may release what it alone holds.
  check("capture parent may tear down the seam its own latch holds",
        seam_teardown_permitted_by(parent_only, SeamClaimant::CaptureParent,
                                   OwnClaim::AlreadyHeld));
}

void run_self_block_checks() {
  // THE CASE. One admitted capture, asking for the geometry it was admitted
  // for. Its own claim must not refuse it.
  const SeamClaims sole_capture = claims(0, 1, 0);
  check("a capture is not blocked by its own claim",
        seam_reconfiguration_permitted(sole_capture, SeamClaimant::Capture,
                                       OwnClaim::AlreadyHeld));
  // Exactly one is discounted, not all of them.
  const SeamClaims two_captures = claims(0, 2, 0);
  check("a second capture still forbids reconfiguration",
        !seam_reconfiguration_permitted(two_captures, SeamClaimant::Capture,
                                        OwnClaim::AlreadyHeld));

  // A capture discount does not excuse a stream claim.
  check("a capture cannot reconfigure under a live stream",
        !seam_reconfiguration_permitted(claims(1, 1, 0), SeamClaimant::Capture,
                                        OwnClaim::AlreadyHeld));

  // THE CAPTURE-PARENT SELF-BLOCK. sync_capture_parent_priming retains its
  // latch and THEN asks for a seam matching the retained profile. Its own latch
  // must not refuse it, or the device can never reach the profile it was given
  // and keeps the wrong geometry silently.
  check("capture parent cannot reconfigure under a live stream",
        !seam_reconfiguration_permitted(claims(1, 0, 1), SeamClaimant::CaptureParent,
                                    OwnClaim::AlreadyHeld));
  check("capture parent cannot reconfigure under an in-flight capture",
        !seam_reconfiguration_permitted(claims(0, 1, 1), SeamClaimant::CaptureParent,
                                    OwnClaim::AlreadyHeld));
}

// The discount is the CALLER declaring its own ordering, never inferred from
// the claimant: providers retain at different points and an inferred rule would
// be silently wrong for one of them.
void run_ordering_is_the_callers_statement_check() {
  const SeamClaims one_stream = claims(1, 0, 0);

  // A WinRT stream realizes first and retains only once started, so when it
  // asks it holds nothing and a stream claim present belongs to somebody else.
  check("stream that has not yet retained is refused by an existing stream",
        !seam_reconfiguration_permitted(one_stream, SeamClaimant::Stream,
                                    OwnClaim::NotHeld));

  // A Camera2 stream retains before requesting realization, so identical counts
  // mean its own claim and must not refuse it.
  check("stream that has already retained may reconfigure its own seam",
        seam_reconfiguration_permitted(one_stream, SeamClaimant::Stream,
                                   OwnClaim::AlreadyHeld));

  check("a second stream still refuses a stream that has retained",
        !seam_reconfiguration_permitted(claims(2, 0, 0), SeamClaimant::Stream,
                                    OwnClaim::AlreadyHeld));
}

void run_unheld_seam_checks() {
  // Nothing held: every operation is permitted for every asker.
  for (SeamClaimant asker :
       {SeamClaimant::Stream, SeamClaimant::Capture, SeamClaimant::CaptureParent}) {
    check("unheld seam permits geometry change",
          seam_reconfiguration_permitted(claims(0, 0, 0), asker, OwnClaim::NotHeld));
    check("unheld seam permits replacement",
          seam_reconfiguration_permitted(claims(0, 0, 0), asker, OwnClaim::NotHeld));
  }
}

void run_discount_targets_own_claim_check() {
  // The discount must never wrap. A capture asking with zero capture claims is
  // a bookkeeping bug elsewhere, but it must not turn into a huge count that
  // pins the seam forever.
  check("discount does not underflow on zero",
        seam_reconfiguration_blocking_claims(claims(0, 0, 0), SeamClaimant::Capture,
                                     OwnClaim::AlreadyHeld) == 0);

  // And it must come off capture_refs SPECIFICALLY, never off the total. An
  // earlier version subtracted from the sum, so a capture holding no claim of
  // its own silently excused somebody else's -- caught by this verifier on its
  // first run, against a policy that had already been wired into the provider.
  check("a capture with no claim does not excuse a stream's",
        seam_reconfiguration_blocking_claims(claims(1, 0, 0), SeamClaimant::Capture,
                                     OwnClaim::AlreadyHeld) == 1);
  // Asked of teardown, not reconfiguration: the capture parent deliberately
  // takes no part in reconfiguration at all, so only teardown can show whether
  // a capture's discount wrongly reaches another category's claim.
  check("a capture with no claim does not excuse the capture parent's",
        !seam_teardown_permitted_by(claims(0, 0, 1), SeamClaimant::Capture,
                                    OwnClaim::AlreadyHeld));
  check("a capture with no claim cannot replace a stream-held seam",
        !seam_reconfiguration_permitted(claims(1, 0, 0), SeamClaimant::Capture,
                                    OwnClaim::AlreadyHeld));
}

void run_teardown_is_permission_not_instruction_check() {
  // Zero references makes teardown PERMITTED. The policy must not be readable
  // as "tear it down now": a provider may keep a warm seam, and a release only
  // says that claimant no longer needs the seam.
  //
  // Expressed as the property that matters: permission is stable under
  // re-asking, and says nothing about what the provider then does.
  const SeamClaims unheld = claims(0, 0, 0);
  check("teardown permission is idempotent",
        seam_teardown_permitted(unheld) && seam_teardown_permitted(unheld));

  // And a seam that becomes held again forbids it once more -- permission is a
  // function of current claims, never a latched decision.
  check("teardown permission is recomputed, not latched",
        !seam_teardown_permitted(claims(0, 0, 1)));
}

// THE REPROVISION QUESTION. A capture-session reconfiguration that carries every
// other claimant's output into the new configuration: the seam's native object
// is replaced, but a stream is interrupted rather than discarded.
void run_reprovision_checks() {
  // THE POINT OF THE WHOLE QUESTION, and the one that must never regress: a live
  // stream permits a reprovision while STILL forbidding a plain reconfiguration.
  // If those two ever agree, the new question has collapsed into the old one and
  // buys nothing -- or worse, the old one has been loosened and a WinRT stream
  // can have its geometry taken away underneath it.
  const SeamClaims one_stream = claims(1, 0, 0);
  check("a live stream permits a preserving reprovision",
        seam_reprovision_permitted(one_stream, SeamClaimant::Capture,
                                   OwnClaim::NotHeld));
  check("a live stream still forbids a plain reconfiguration",
        !seam_reconfiguration_permitted(one_stream, SeamClaimant::Capture,
                                        OwnClaim::NotHeld));

  // Several streams are no more of an obstacle than one. Each gaps and resumes.
  check("many streams still permit a reprovision",
        seam_reprovision_permitted(claims(3, 0, 0), SeamClaimant::Capture,
                                   OwnClaim::NotHeld));

  // AN IN-FLIGHT CAPTURE BLOCKS. Its request is bound to the session being
  // replaced, so the image would never arrive -- carrying outputs forward
  // cannot rescue a request already submitted.
  check("an in-flight capture forbids a reprovision",
        !seam_reprovision_permitted(claims(0, 1, 0), SeamClaimant::Stream,
                                    OwnClaim::NotHeld));
  check("an in-flight capture forbids a reprovision even beside a stream",
        !seam_reprovision_permitted(claims(1, 1, 0), SeamClaimant::Stream,
                                    OwnClaim::NotHeld));

  // The capture parent takes no part here either, for the same reason it takes
  // none in reconfiguration: its claim exists to shape the seam.
  check("the capture-parent claim does not block a reprovision",
        seam_reprovision_permitted(claims(0, 0, 1), SeamClaimant::Capture,
                                   OwnClaim::NotHeld));
  // ...but it still blocks an outright teardown. Teardown is untouched by this
  // question and must stay that way.
  check("the capture-parent claim still blocks teardown",
        !seam_teardown_permitted_by(claims(0, 0, 1), SeamClaimant::Capture,
                                    OwnClaim::NotHeld));

  // Own-claim discount, on exactly the terms the other questions use.
  check("a capture is not blocked from reprovisioning by its own claim",
        seam_reprovision_permitted(claims(0, 1, 0), SeamClaimant::Capture,
                                   OwnClaim::AlreadyHeld));
  check("a SECOND capture still forbids a reprovision",
        !seam_reprovision_permitted(claims(0, 2, 0), SeamClaimant::Capture,
                                    OwnClaim::AlreadyHeld));
  check("reprovision discount does not underflow on zero",
        seam_reprovision_blocking_claims(claims(0, 0, 0), SeamClaimant::Capture,
                                         OwnClaim::AlreadyHeld) == 0);
  // The discount must come off the asker's OWN category. A stream discounting
  // itself must not excuse a capture.
  check("a stream's discount does not excuse an in-flight capture",
        seam_reprovision_blocking_claims(claims(1, 1, 0), SeamClaimant::Stream,
                                         OwnClaim::AlreadyHeld) == 1);

  check("an unheld seam permits a reprovision",
        seam_reprovision_permitted(claims(0, 0, 0), SeamClaimant::Capture,
                                   OwnClaim::NotHeld));

  // THE ORDERING INVARIANT. A reprovision preserves what a reconfiguration
  // destroys, so it can never be the stricter of the two: anything
  // reconfiguration permits, reprovision must permit as well. Stated over a
  // matrix rather than as prose so a future change to either rule that inverts
  // them fails here.
  for (uint32_t s = 0; s <= 2; ++s) {
    for (uint32_t cap = 0; cap <= 2; ++cap) {
      for (uint32_t parent = 0; parent <= 1; ++parent) {
        const SeamClaims c = claims(s, cap, parent);
        for (SeamClaimant asker : {SeamClaimant::Stream, SeamClaimant::Capture,
                                   SeamClaimant::CaptureParent}) {
          for (OwnClaim own : {OwnClaim::NotHeld, OwnClaim::AlreadyHeld}) {
            if (!seam_reconfiguration_permitted(c, asker, own)) {
              continue;
            }
            check("reprovision is never stricter than reconfiguration",
                  seam_reprovision_permitted(c, asker, own));
          }
        }
      }
    }
  }
}

void run_winrt_admission_sequence_check() {
  // Replays the WinRT capture sequence against the policy, in order, so the
  // interaction between admission-time retention and worker-time geometry is
  // covered as a sequence rather than as isolated predicates.
  SeamClaims c = claims(0, 0, 0);

  // Core sets a retained profile: capture-parent latch, then released.
  c.capture_parent_refs = 1;
  check("sequence: profile-set may shape geometry",
        seam_reconfiguration_permitted(c, SeamClaimant::CaptureParent,
                                       OwnClaim::AlreadyHeld));
  c.capture_parent_refs = 0;
  check("sequence: releasing the latch leaves teardown permitted",
        seam_teardown_permitted(c));

  // A capture is admitted: claim taken at admission.
  c.capture_refs = 1;
  check("sequence: admitted capture reaches its own geometry",
        seam_reconfiguration_permitted(c, SeamClaimant::Capture,
                                       OwnClaim::AlreadyHeld));
  check("sequence: admitted capture forbids teardown", !seam_teardown_permitted(c));

  // A stream starts while the capture is in flight.
  c.stream_refs = 1;
  check("sequence: capture cannot reconfigure under the new stream",
        !seam_reconfiguration_permitted(c, SeamClaimant::Capture,
                                        OwnClaim::AlreadyHeld));
  check("sequence: stream cannot reconfigure under the in-flight capture",
        !seam_reconfiguration_permitted(c, SeamClaimant::Stream,
                                        OwnClaim::NotHeld));

  // Capture reaches a terminal outcome.
  c.capture_refs = 0;
  check("sequence: stream alone still pins its own geometry",
        !seam_reconfiguration_permitted(c, SeamClaimant::Stream,
                                        OwnClaim::NotHeld));
  check("sequence: seam still held by the stream", !seam_teardown_permitted(c));

  // Stream stops.
  c.stream_refs = 0;
  check("sequence: seam released by everything", seam_teardown_permitted(c));
}

}  // namespace

int main() {
  run_held_and_teardown_checks();
  run_capture_parent_never_blocks_reconfiguration_checks();
  run_self_block_checks();
  run_unheld_seam_checks();
  run_ordering_is_the_callers_statement_check();
  run_discount_targets_own_claim_check();
  run_teardown_is_permission_not_instruction_check();
  run_reprovision_checks();
  run_winrt_admission_sequence_check();

  if (g_failed != 0) {
    std::cout << "FAIL acquisition_seam_claims_verify run=" << g_run
              << " failed=" << g_failed << "\n";
    return 1;
  }
  std::cout << "PASS acquisition_seam_claims_verify run=" << g_run << " ok=" << g_run
            << " failed=0\n";
  return 0;
}
