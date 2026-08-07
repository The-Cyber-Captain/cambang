# Current tranche

## Auto-reprovision: stop preempting streams the backend could have kept

### Terminology

A **reprovision** is a capture-session reconfiguration. It is never a "rebuild" —
that word means SCons here. The previous tranche let "rebuild" back into new
Camera2 and contract comments; those uses are corrected as part of this work.
Pre-existing uses elsewhere in the Camera2 provider are a separate sweep and are
out of scope.

### Problem

Camera2 preempts a producing stream for a capture it could have served alongside
it. Measured on S20+ camera 0 via scene 911, state 3:

```
seam realize claimant=stream  flow=caller_stream 640x480 still=640x480
seam realize claimant=capture flow=caller_stream 640x480 still=1280x720
```

The session was provisioned against whatever still profile was retained when the
stream started — here 640x480, the same as the stream. The state then asks for a
still at 1280x720, a geometry no output was declared for. Reaching it needs the
session replaced, the producing stream's claim forbids that, so the provider
answers `StreamMustYield` and Core stops the viewfinder.

Nothing about the hardware requires this. Camera2 can hold a 640x480 stream
output and a 1280x720 still output in one session. The stream dies for want of a
prediction, which is precisely the outcome `arbitration_policy.md` §6.2 was not
written to produce: preemption is for where the backend genuinely cannot serve
both, and this is not that case.

The provisioning work in the previous tranche is real but narrower than it was
described as. It avoids a reprovision only when the profile retained AT STREAM
START is the one later captures use. Change the profile after the stream is up
and the preemption returns.

### Decision, taken

**1. The claim policy gains a third question.**

`seam_reconfiguration_permitted` blocks on any other stream or capture claim,
and deliberately covers both an in-place change and a replacement of the native
object. That conflation was right while both shapes destroyed what the other
claimant depended on. A *preserving reprovision* is a third shape: the session is
replaced, and every other claimant's output is carried into the new one. The
stream is interrupted, not discarded, and the header has no word for it.

Add `seam_reprovision_permitted(claims, requester, own)` alongside the existing
two, differing only in what blocks it:

| Held by someone else | reconfiguration | reprovision | teardown |
|---|---|---|---|
| stream | blocks | **does not block** | blocks |
| capture in flight | blocks | blocks | blocks |
| capture parent | does not block | does not block | blocks |

A stream does not block because it gaps and resumes on the far side. An in-flight
capture does, because its request is bound to the session being replaced and its
image would never arrive. The capture parent is unchanged. Own-claim discounting
works exactly as it does today, off its own category.

**The two questions map onto the two backends' real constraints, which is what
makes this a distinction rather than a loophole.** WinRT's reconfiguration cannot
preserve a stream — one shared `MediaFrameSource` format, so the stream's
geometry is genuinely lost — so it keeps asking `seam_reconfiguration_permitted`
and keeps yielding. Camera2's can, so it asks the new question and gaps instead.

"Preserving" is a precondition the CALLER asserts. The header knows only counts
and cannot verify that outputs were carried forward, so this is documented the
way `OwnClaim` already is and bound by a compliance check.

**NOT permitted as an alternative:** having Camera2 release the stream's claim
around its own reprovision and reacquire it afterwards. That falsifies the counts
while the stream still depends on the seam, and opens a window in which a third
party could tear the seam down entirely. The counts existing to be true is what
the header is for.

**2. A gap is reported by silence, and needs no new vocabulary.**

Core has no starvation watchdog: `last_frame_ts_ns` is recorded and nothing acts
on it, and `snapshot_builder` deliberately does not project `STARVED`. Camera2
posts `stream_stopped` in exactly three places — the device-failure latch, a
caller's `stop_stream`, and device close — and session teardown/recreate posts
none of them.

So a preserving reprovision reports `native_destroyed` + `native_created` for the
AcquisitionSession, which are real transitions, and nothing about the stream. The
stream record stays started, frames pause and resume. That is the truth.

It also keeps the two verdicts distinguishable at the boundary, which is what
makes them separately testable:

| | stream record | snapshot |
|---|---|---|
| `Reconfigure` | stays started | `FLOWING` / `NONE` |
| `StreamMustYield` | stopped by Core | `STOPPED` / `PREEMPTED` |

**3. Core enforces that frames actually resume, and the guarantee lives there
rather than in the provider.**

Silence is the right report for a gap, but it is also how a permanent hang would
look. A reprovision that never restores the flow leaves the stream `FLOWING`
forever delivering nothing, and no watchdog, snapshot or gate would notice. That
is a worse failure than the preemption this tranche removes.

Leaving that to the provider makes the guarantee unverifiable: the code is in
Camera2, which cannot be built host-native, so it would rest on inspection plus
whatever a hardware run happened to exercise — and a run where the reprovision
works never touches it. In Core it is enforceable independent of any provider and
bindable by a deterministic check.

So: when Core acts on a `Reconfigure` verdict for a producing stream, it arms a
bounded expectation that frames resume for that stream, disarms it on the next
frame, and on expiry reports the stream as failed. The stopped fact carries a
provider error, which projects as `STOPPED` / `PROVIDER` — truthful, and already
distinct from both `USER` and `PREEMPTED`. No public enum changes.

**This is NOT a general frame-cadence watchdog.** It arms only after Core has
authorised a specific reprovision, and it asks one question: did the thing Core
just permitted actually happen. A watchdog over all streams at all times needs a
policy about what cadence means, which is `STARVED`, which is out of scope.

The bound is supplied by the provider, mirroring
`capture_admission_watchdog_timeout_ns()` and subject to the same rule stated on
that method: a value backed by measured worst-case latency, never a guess. Core
cannot know a given backend's reprovision cost, and a bound that is too short
turns a slow-but-working reprovision into a false failure — worse than waiting.

The provider obligation to report a failed restore stays as well. That is not
duplicated refusal of the kind removed from WinRT admission: there, a second
check would fire exactly when Core had arbitrated correctly. Here the two catch
different things — the provider knows when its own call failed, and Core catches
the case where the backend accepts the request and then delivers nothing.

### Scope

1. `seam_reprovision_permitted` in `imaging/api/acquisition_seam_claims.h`, with
   the preserving precondition stated. `seam_teardown_permitted` and
   `seam_teardown_permitted_by` do not change.
2. Camera2 performs a preserving reprovision where the requested still geometry
   is not in the realized output set and a stream is producing: halt the
   repeating request, reconfigure the session carrying the stream output AND the
   new still output, re-establish the repeating request. The stream keeps its
   identity and its own geometry.
3. Camera2's `acquisition_coexistence` answers `Reconfigure` for that case
   instead of `StreamMustYield`. `Unsupported` and the already-configured
   `Coexist` cases are unchanged.
4. Core's `Reconfigure` path becomes reachable for the first time. It already
   falls through without stopping anything, which is correct; it needs a test
   that says so rather than an accident that happens to hold.
5. **Core arms a bounded frame-resumption expectation** when it acts on a
   `Reconfigure` for a producing stream: disarmed by the next frame on that
   stream, and on expiry the stream is reported failed (`STOPPED` / `PROVIDER`).
   Armed only for a reprovision Core itself authorised — not a cadence watchdog
   over all streams.
6. A provider-supplied bound for that expectation, alongside
   `capture_admission_watchdog_timeout_ns()` and carrying the same rule in its
   doc comment: measured worst-case latency, never a guess. A generous default,
   because a bound that is too short converts a working reprovision into a false
   failure.
7. Provider obligations, both binding:
   - **Silence during the swap.** No `stream_error`, no `stream_stopped`, and no
     frames posted from the retired reader. Any of those turns a gap into an
     apparent failure.
   - **Report a restore that fails.** Where the provider knows its own call
     failed, it latches a stream error and posts `stream_stopped` rather than
     leaving Core's expectation to expire. Core's check is the backstop for the
     case it cannot know about — a backend that accepts the request and then
     delivers nothing.
8. Correct the "rebuild" uses introduced by the previous tranche.

### Out of scope

- `STARVED`. Core's expectation catches a reprovision whose frames never come
  back, so a permanent hang is reported. What remains indistinguishable is a
  reprovision that is merely SLOW: while it is within the bound, a caller
  watching frame cadence cannot tell "briefly reprovisioning" from "the device
  stalled". That distinction needs retained state that does not exist, and is a
  stated limitation of this work rather than a defect in it.
- Pre-existing "rebuild" wording elsewhere in the Camera2 provider.
- WinRT. Its constraint is real and its yield stays; it is the control case.
- Restoring a preempted stream after a capture settles. §6.2's "preempted to
  STOPPED" stands, and `yielded_stream_restorable` remains an advertisement with
  a compliance check and no consumer.
- Any use of Media Foundation.

### Acceptance criteria

1. `seam_reprovision_permitted` exists, a live stream permits it while still
   forbidding a plain reconfiguration, and an in-flight capture forbids both.
   Mutation-proved in `acquisition_seam_claims_verify`.
2. A provider that asks `seam_reprovision_permitted` and then fails to carry
   another claimant's output forward fails a `provider_compliance_verify` check.
3. Core's `Reconfigure` path is exercised by a check that fails if Core stops a
   stream on that verdict.
4. Scene 911 state 3 on S20+ camera 0: the capture is delivered at 1280x720 AND
   the stream reads `FLOWING`/`NONE` afterwards. Today it reads
   `STOPPED`/`PREEMPTED`; that is the change this tranche is for.
5. Scene 911 state 3 on the eMeet C970 still reads `STOPPED`/`PREEMPTED`. WinRT
   must not be dragged along by a Camera2 fix.
6. **A reprovision whose frames never resume is reported as a stream failure,
   and this is bound host-native.** A provider that answers `Reconfigure` and
   then delivers no further frames must produce `STOPPED` / `PROVIDER` within the
   provider's stated bound. Mutation-proved: removing the arming must fail the
   check. This is the criterion that had no gate when the guarantee sat in the
   provider, and it is the reason it moved to Core.
7. Core does not arm the expectation for `Coexist`, `StreamMustYield` or
   `Unsupported`, and a stream that resumes normally is never reported failed.
   The false-positive direction gets its own check; a watchdog that fires on
   healthy streams would be worse than none.
8. Existing gates green.

### Open gap: Core's arming is unobserved on hardware

Criteria 6 and 7 are met host-native, with the never-arm mutation failing both
checks. **Core arming the expectation has never been observed on a device**, and
three attempts to see it failed:

- Shortening the Camera2 bound to 50 ms, then to 1 ms, against a reprovision
  measured at ~350 ms. State 3 read `FLOWING`/`NONE` both times: the expiry did
  not fire.
- Core's own log lines cannot settle it. Core writes diagnostics with
  `std::fprintf` from the core thread, and that does not reach Android logcat.
  Proven by counting the provider-attached banner, which is emitted twice --
  once by Core directly and once by the Godot-side echo at
  `cambang_server.cpp:3036`: Windows captures 2, Android captures 1. Every one
  of Core's ~17 direct writes is invisible on Android. That is a pre-existing
  repo-wide condition, not something this tranche introduced.

So two possibilities remain unseparated: Core never arms on Android, or it arms
and the expiry never runs before the resuming frame disarms it. The second is
the less likely -- Core drains provider facts and checks expiry in the same tick
handler, and there is capture activity in that window -- but that is reasoning,
not evidence.

Neither backend can reach the arming path in a way that shows: WinRT answers
`StreamMustYield` and never reaches it at all, and Camera2's `Reconfigure` path
looks identical from outside whether Core armed or not, because the reprovision
is provider-side and happens regardless.

Two routes to close it, both needing a decision rather than a tweak:

- **Fault injection in Camera2**, reached by a `cambang/maintainer/...` project
  setting and `--cambang-...=` argument, the pattern `imaging/synthetic/config.h`
  already uses. Device-reachable, unlike `CAMBANG_INTERNAL_SMOKE`, which is a
  smoke-build concept and absent from GDE by design. The cost is a
  fault-injection path living in shipping platform code. It would also exercise
  Camera2's own loud-failure path, which has no coverage anywhere.
- **A bounded Core-to-host diagnostic drain**, generalising the single-slot
  banner echo. Core stays platform-free and the host emits. This is repo-wide
  work touching every existing Core write, and belongs in its own tranche.

Until one is taken, criteria 6 and 7 stand as host-native only. Do not record
them as hardware-validated.

### Validation expectations

Deterministic (required):

- `out/core_spine_smoke.exe`, `out/provider_compliance_verify.exe`,
  `out/restart_boundary_verify.exe`, `out/verify_case_runner.exe --run-all`,
  `out/core_thread_liveness_watchdog_verify.exe`,
  `out/outstanding_payload_ledger_verify.exe`,
  `out/capture_sequence_settlement_verify.exe`,
  `out/acquisition_seam_claims_verify.exe`, `out/phase3_snapshot_verify.exe`.
- Every new rule gets a mutation proof: removing it must fail the verifier.
- The frame-resumption expectation is exercised in `provider_compliance_verify`
  through a provider that answers `Reconfigure` and then stops delivering, and
  again through one that resumes normally. Both directions, because a watchdog
  is only as good as its false-positive behaviour.

Hardware (required before acceptance):

- Scene 911 on S20+ camera 0 and on the eMeet C970, operator-paced through all
  twelve gates. `TARGET` selects which. Criteria 4 and 5 are read from the
  `stream state after capture` line, not from the pass count.
- **The Windows platform-backed suite, carried forward from the previous
  tranche.** It was deliberately not run there: phase 2 changed shared Core code,
  but the arbitration behaviour was about to change again, and validating a
  configuration that is about to move is wasted. It belongs here, after the
  `Reconfigure` path settles. Ask before starting the multi-scene ADB sweep.

Report un-run surfaces plainly. Native-tool PASS does not prove the Godot scene.
