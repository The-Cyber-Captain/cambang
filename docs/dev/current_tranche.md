# Current tranche

## Rig membership lifecycle: versioning, mutation, and device loss

Tranche 4 of the `capture_identity_and_lifecycle.md` implementation. Depends on
tranche 1 (`82fe1e7`, split id spaces), tranche 2 (`c44e787`, dispositions and
cohort closure) and tranche 3 (`9084bfe`, arbitration).

Additive at the Godot boundary — `CamBANGRig` gains methods — but changes no
existing signature, return type, constant or dictionary key. The public API
break remains tranche 5's.

### Problem

Rig membership is fixed at creation. `CamBANGRig` exposes exactly `get_id`,
`trigger_capture` and `get_result`; `CoreRigRegistry` holds
`member_hardware_ids` with no version and no mutation path beyond
`retain_member_hardware_ids`. Section 5 of the design is therefore entirely
unimplemented, and three of its rules are currently unenforced rather than
merely unexposed:

- **No `rig_membership_version` (§5.2).** A stored result set cannot describe
  the membership that produced it.
- **`DEVICE_LOST` has no producing path (§5.3).** Tranche 2 defined the
  disposition and deliberately left it unreachable. A device closed while one
  of its member captures is in flight currently leaves that member to the
  capture-admission watchdog, which reports `FAILED(ERR_TIMEOUT)` 30s later —
  a resource event misreported as a timeout.
- **One rig per device (§5.5) is not enforced.** Nothing in `create_rig` or
  `retain_member_hardware_ids` rejects a device that already belongs to another
  rig. §5.5 is load-bearing for tranche 3's per-rig arbitration scope: that
  denial is per-rig precisely *because* cohorts cannot share participants. The
  assumption is currently unchecked.

§5.1 (snapshot at trigger) is believed already satisfied — a cohort's
`expected_participants` are captured at admission from preflight — but that is
incidental, not asserted. This tranche pins it.

### Decision

**Membership is declarative configuration, versioned forward (§6).** Adding or
removing a member is accepted while live and applies from the next trigger. It
is never `ERR_BUSY`, and it never alters a cohort already in flight.

**Device loss is a resource event, not a configuration change (§5.3).** A
device closed, disengaged or lost while a member capture is in flight
terminalises that member `DEVICE_LOST` — promptly, not via the 30s watchdog. A
member capture must always reach a terminal disposition; it must never simply
disappear from a cohort.

**Removal settles what the device still owes (§5.4).** A device leaving a rig
with abandoned or lost captures may still owe buffers to its provider. Removal
must settle that accounting, or it becomes a new route by which a stale payload
is attributed to an unrelated capture — including a standalone capture taken
after the device has left the rig. Tranche 3 established the pattern for the
preemption case; this is the same obligation on a different trigger.

### Scope

1. `rig_membership_version` on `CoreRigRegistry::RigRecord`, bumped on a real
   membership change. Core-internal only.
2. Each cohort records the membership version it was admitted under, so a rig
   capture knows the membership it ran against (§5.2's "each Rig Capture
   records the version it was admitted under"). Also Core-internal.
3. Add/remove member on `CamBANGRig` (additive public methods), applying from
   the next trigger and never disturbing an in-flight cohort.
4. One-rig-per-device enforced at rig creation and at membership change.
5. `DEVICE_LOST` producing path: a device closed or disengaged with a member
   capture in flight terminalises that member promptly.
6. Removal settles the leaving device's outstanding provider payload
   accounting. **CLOSED AS SATISFIED BY CONSTRUCTION** — no code written, and
   deliberately so:
   - §5.1 says removal does not disturb an in-flight cohort; that capture
     completes. Removal therefore abandons nothing itself.
   - An orderly device close cannot abandon a capture either: a provider pins
     the device while one is in flight (`Camera2CameraProvider::close_device`
     returns `ERR_BUSY`).
   - The one path that genuinely abandons is rig preemption, and tranche 3
     already aborts there — Core's single `abort_capture` call.
   - Tranche 1's split id spaces mean a late payload carries its own Device
     Capture Id, so Core cannot attribute it to a later capture (§7's hazard).

   A removal-time sweep would abort captures already long settled: noise
   rather than settlement.

   **One real gap remains and is NOT this tranche's**: a capture failed by the
   30s admission watchdog is abandoned with no `abort_capture`. Recorded at
   `CoreCaptureAssemblyRegistry::sweep_admission_timeouts`, where it is
   created. It belongs with arbitration/lifecycle work, not membership.
7. §5.1 pinned by a check rather than left incidental.
8. **Update `capture_identity_and_lifecycle.md` §0 and §9 to match.** Standing
   obligation of every tranche in this branch: §9 is the ledger of how the
   source still differs from the model, and a status section that drifts is
   read as current.

### Out of scope

- **The state snapshot.** `rig_membership_version` does NOT go into the
  snapshot dictionary or `state_snapshot_schema.json`. §5.2 asks for two
  things: the rig record carries the version, and each Rig Capture records the
  version it was admitted under so a *stored result set* is self-describing.
  A result field is §2.3, which is tranche 5. Nothing in the design asks for a
  snapshot projection, and the snapshot dictionary is part of the locked
  Godot-facing surface (CLAUDE.md) — a tranche may not widen it just because
  the value happens to be nearby.

  An earlier draft of this scope item said "projected into the snapshot" and
  was implemented: the field was added to `RigState`, the builder, the export,
  and to the schema's `required` array under `additionalProperties: false`,
  which would have invalidated every stored snapshot lacking it. Reverted
  before it ran. Recorded here so the same reasoning is not repeated.
- Durable public ids, result fields, trigger returning identity (tranche 5).
- Completion signals, canonical wrappers, outstanding set (tranche 6).
- Narrowing `ERR_BUSY`.
- The WinRT equal-geometry `Coexist` defect recorded in
  `winrt_camera_provider.cpp`, and the scene-870 Android second-bundle stall
  recorded in `eeb85af`. Both predate or sit outside this work.

### Acceptance criteria

1. Removing a member while that rig has a capture in flight does not alter the
   in-flight cohort; the capture completes with the device as a participant,
   and the removal applies from the next trigger. Adding behaves symmetrically.
   Neither returns `ERR_BUSY`.
2. `rig_membership_version` bumps on change, and a cohort reports the version it
   was admitted under.
3. A device closed while one of its member captures is in flight terminalises
   that member `DEVICE_LOST`, **and specifically not** `FAILED(ERR_TIMEOUT)` —
   the check must distinguish the two, or it proves nothing beyond the watchdog
   still working.
4. A device already in one rig cannot join another, at creation or by add.
5. A payload owed by a removed device is never attributed to a later capture on
   that device.
6. Every rule mutation-proved. Criteria 1 and 4 get their false-positive
   direction too: a membership change that wrongly refuses, or an enforcement
   that rejects a legitimate rig, is worse than the gap it closes.
7. Existing gates green.

### Validation expectations

Deterministic (required):

- The nine verifiers.
- Both directions for criteria 1 and 4.
- `DEVICE_LOST` proven against a device closed mid-capture, distinguished from
  the watchdog path by disposition and by timing.

Godot (required):

- `.\godot_test_suite.ps1`.
- `73_rig_capture_result_set_verification.tscn`, windowed.

Hardware: **WinRT, not Android.** Scene 870 on the S20+ stalled in four of four
post-fix runs for reasons unrelated to this work, and is not a usable vehicle
until that is understood. WinRT with the C970 plus internal camera completed
cleanly post-fix (rig 66/12/0) and is the working platform-backed path.

Note what hardware can and cannot show here: membership mutation and
one-rig-per-device are Core rules that a host-native check proves better than a
scene does. `DEVICE_LOST` against a genuinely disengaged device is the part
worth seeing on hardware, and 870 does not exercise device close mid-capture —
so if that coverage is wanted it needs an explicit decision about how, not an
assumption that an existing scene covers it.

**Build the GDE, not just the verifiers**, and build Android — Core changes
reach `camera2_camera_provider.cpp`'s compile only there, even when Android is
not the validation vehicle.
