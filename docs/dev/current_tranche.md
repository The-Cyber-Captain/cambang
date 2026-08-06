# Current tranche

## Capture-over-stream arbitration: provider reports, Core decides

### Problem

`arbitration_policy.md` §2 puts triggered capture above repeating streams and
says repeating streams are always preemptible by triggered capture; §6.2 says a
VIEWFINDER is denied or preempted to `STOPPED` while a triggered capture is in
flight. **Both platform providers do the opposite.** With a stream producing at
1280x720, a device capture at 640x480 is refused — measured on S20+ camera 0
(Camera2) and on an eMeet C970 (WinRT), same step, same result. Each refuses in
its own `validate_and_admit_submission_locked_`, before any seam claim is
consulted, so Core never learns a conflict existed and its preemption path —
frame-integration suppression, entered only *after* `prov->trigger_capture`
succeeds — never runs.

Two further symptoms of the same seam gap:

- `set_still_capture_profile` returns `OK` while a stream produces, without the
  provider ever being asked to prime at the new geometry. It is a *try*
  (`TrySetStillCaptureProfileStatus` has `OK` / `NotSupported` / `Busy`) and it
  reported success for something it had not attempted, so Core's retained
  profile and the seam's real shape diverge silently.
- The providers' distinct refusal codes both flatten to `ERR_UNAVAILABLE` at the
  Godot boundary, so the caller cannot recover the reason.

### Decision, taken

**The provider reports what its backend can do; Core decides who yields.** No
single rule Core could hardcode is right for both implemented backends, because
their constraints differ in kind:

- **camera2** — *declare up front.* `ACameraCaptureSession` fixes its output set
  at creation and rebuilding cancels the repeating request, but a session may
  hold a stream output and a still output at different geometries. Today
  `start_stream` provisions the still at the *stream's* geometry
  (`camera2_camera_provider.h`, "Outputs are fixed at session creation"), which
  is what makes a differing capture need a rebuild. Combination limits are
  answerable from `INFO_SUPPORTED_HARDWARE_LEVEL` and `StreamConfigurationMap`,
  both cached at open.
- **windows_winrt** — *one active format, shared.* A `MediaFrameSource` has a
  single active `MediaFrameFormat` and `SetFormatAsync` changes it for every
  reader on that source. On a shared-pin UVC device two geometries are not
  available at once, so a real yield is unavoidable.

The three unimplemented seams are taken at their documented face value and get a
declaration only, no seam work: `linux_v4l2` one active format per node with
`VIDIOC_S_FMT` refused while streaming; `apple_avfoundation` concurrent photo and
video outputs with `activeFormat` changes interrupting; `web_getusermedia`
varying by engine and therefore declared conservatively.

**Vocabulary.** One query, asked of a proposed concurrent *set* rather than of a
single profile — Camera2's constraint is a property of the whole output set, so
a per-profile question is unanswerable there. It is symmetric by construction: a
stream starting under a retained still profile is the same question as a profile
being retained under a live stream. Four verdicts:

- `Coexist` — both served, nothing disturbed.
- `Reconfigure` — both served, but reaching that state interrupts the stream.
- `StreamMustYield` — not concurrent; the lower-priority stream stops so the
  capture proceeds.
- `Unsupported` — not serviceable at these shapes in any order. A capability
  denial, which is **not** a priority decision and must not be reported as one.

Answerable from characteristics already cached at open, with no backend I/O —
brief §2 forbids I/O in a capability query on the core thread, and this is
consulted at profile-set, stream start and capture admission. Being a pure
function of static characteristics and the proposed set, it also satisfies §7.1
determinism.

Where a yield is required, the verdict must also carry whether the yielded
stream can be restored at its own profile afterwards. Core cannot honour §6.1's
"public stream objects remain valid" without knowing that.

### Scope

**Phase 1 — the seam and the truth. No behaviour change.**

1. The coexistence query on `ICameraProvider`, with the four verdicts and the
   restorability answer.
2. `SyntheticProvider` answers `Coexist` for everything, and that is the
   reference statement of the permissive end of the range, not a stub. Stub
   provider likewise.
3. Both platform providers answer from cached characteristics.
4. The three unimplemented seams carry their declaration in source.
5. A `provider_compliance_verify` check binding **answer to behaviour**: a
   provider answering `Coexist` must not then refuse a capture during a stream;
   one answering `StreamMustYield` must see the stream stopped before the
   capture is admitted. Mutation-tested.

Nothing consults the verdict for decisions in this phase, so every existing gate
stays green and the phase is verifiable host-native.

**Phase 2 — Core acts on it.**

6. Core consults the verdict at profile-set, stream start and capture admission,
   and implements §6.2 stop-then-capture where the verdict demands a yield.
7. The stopped fact carries a preemption reason. A caller must be able to tell
   preemption from a failure and from its own stop; §6.1 requires truthful
   accounting and does not permit reporting a stop that did not happen.
8. Camera2 provisions the retained still geometry as its own output, so the case
   911 fails on should answer `Coexist` and never reach the yield path. A
   capture at a geometry no output was declared for remains a genuine
   `Reconfigure`.
9. WinRT implements the yield: preempt and restore.
10. Both providers' unilateral admission refusals come out. A provider may still
    refuse an operation that would corrupt its own state; it may not refuse one
    that merely conflicts by priority.
11. `set_still_capture_profile` answers truthfully, using the verdict.

### Out of scope

- Rig-capture repair. Complete; evidence is in git history.
- WinRT bracketing.
- Seam implementation for `linux_v4l2`, `apple_avfoundation`, `web_getusermedia`
  beyond the declaration in scope item 4.
- The outstanding-capture query designed in `capture_identity_and_lifecycle.md`
  §4.5.
- Any use of Media Foundation. `windows_winrt` means real WinRT APIs; surface a
  toolchain blocker rather than substituting a backend.
- `kMaxBracketMembers`. It is deliberate maintainer policy.

### Acceptance criteria

1. Scene 911 passes on **both** backends — S20+ camera 0 and the eMeet C970.
   Step 4 asserts the contract and currently fails on both; it is the reproducer
   this tranche exists to turn green, and no expectation in it may be relaxed to
   reach that.
2. A provider's coexistence answer and its behaviour agree, bound host-native
   and mutation-tested: a provider that answers `Coexist` and then refuses must
   fail the verifier.
3. A stream stopped by preemption is distinguishable at the boundary from a
   caller's stop and from a failure.
4. `set_still_capture_profile` reports a failure to apply when it cannot apply.
5. Camera2 answers `Coexist` for a retained still profile differing from a live
   stream, and reaches it without a session rebuild.
6. Existing gates green.

### Validation expectations

Deterministic (required):

- `out/core_spine_smoke.exe`, `out/provider_compliance_verify.exe`,
  `out/restart_boundary_verify.exe`, `out/verify_case_runner.exe --run-all`,
  `out/core_thread_liveness_watchdog_verify.exe`,
  `out/outstanding_payload_ledger_verify.exe`,
  `out/capture_sequence_settlement_verify.exe`,
  `out/acquisition_seam_claims_verify.exe` all green.
- Any new rule gets a mutation proof: removing it must fail the verifier.

Hardware (required before acceptance):

- Scene 911 on Android over ADB, S20+ camera 0, windowed. Android scenes cannot
  self-verdict, so classify from step evidence in logcat.
- Scene 911 on Windows against the eMeet C970, windowed. Set `TARGET` to match;
  it is currently `"windows"`.
- Windows platform-backed suite, windowed, since Core changes in phase 2 are
  shared code. Ask before the full multi-scene ADB sweep.
- State what the maintainer must watch or press **before** any interactive run
  starts.

Frame-time p99 needs no fresh matched-condition measurement unless phase 2
changes the frame path. If it does, the alternating base/HEAD/base protocol from
the previous tranche applies.

Report un-run surfaces plainly. Native-tool PASS does not prove the Godot scene.
