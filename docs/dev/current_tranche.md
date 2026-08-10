# Current tranche

## Capture completion: dispositions, cohort closure, and the simultaneity window

Tranche 2 of the `capture_identity_and_lifecycle.md` implementation. Internal
only — no Godot-facing surface changes. Depends on tranche 1 (`82fe1e7`), which
split the Device Capture and Rig Capture id spaces.

### Problem

A rig capture has no notion of being finished. `CoreCaptureCohortRegistry`
carries `OPEN` and `FAILED` and nothing else, `CoreCaptureAssemblyRegistry`
carries `NONE`/`COMPLETED`/`FAILED` per device, and there is no cut-off. The
consequences are all visible today:

- `get_capture_result_set()` returns a partial set that is indistinguishable
  from a final one. A caller polling it cannot tell "two of three so far" from
  "two of three, and the third is never coming".
- A member that timed out with a known provider error becomes an *absent array
  entry*. The error code exists in the assembly record and never reaches the
  caller, so a failure and a still-pending member look identical.
- `RigState::active_capture_id`, `last_capture_id`, `captures_triggered`,
  `captures_completed` and `captures_failed` are never written and project a
  permanent 0 (found while closing tranche 1's scope item 7). A consumer cannot
  read 0 there as "no capture yet".

Every working scene hand-rolls its own completion detection because of this.
`70_result_retrieval_verification.gd` carries 48 in-flight references and
`870_to_image_soak_benchmark.gd` 17, all of it reconstructing state Core
already has and does not expose.

### Decision

**Per-member terminal disposition**, replacing the per-device
`COMPLETED`/`FAILED` binary:

`DELIVERED` | `FAILED(error)` | `LATE_EXCLUDED` | `PREEMPTED_BY_RIG` |
`DEVICE_LOST` | `NEVER_ARRIVED`

The full vocabulary is defined here so the shape is settled once, but two
values are **not reachable in this tranche and must not be faked**:
`PREEMPTED_BY_RIG` needs rig-preempts-member arbitration (tranche 3), and
`DEVICE_LOST` in its §5.3 sense needs membership lifecycle (tranche 4). A
disposition no code path can produce is honest; one produced by guessing is
not.

**Cohort closure.** `CohortState` gains a completed state, and a closed cohort
records `ALL_MEMBERS_TERMINAL` or `WINDOW_EXPIRED`.

**The window is a simultaneity tolerance, not an impatience threshold.** A rig
capture closes when every member is terminal or when the window expires,
whichever comes first. A member arriving well outside it is not part of the
same moment, and `LATE_EXCLUDED` is the correct outcome rather than a failure
to wait. A six-device capture closing with four delivered, one failed and one
late-excluded is a complete and truthful result.

**Clock constraint, and it is load-bearing.** Lateness is measured on Core's
own clock from capture admission. `camera_fact_model.md` §12.2 forbids using
acquisition timing as ordering or latency evidence, and §12.1 notes Capture
Date-Time is deliberately *shared* across one rig capture. Acquisition marks
from separate devices may legitimately be identical and must never decide
membership, lateness, ordering or identity.

`capture_sequence_settlement.h` already solves the per-device version of this
problem and solves it well, including the in-flight grace that a naive
"settle on sequence end" gets wrong. Lift that reasoning to cohort level rather
than reinventing it — in particular, decide explicitly what the cohort does
about a member whose payload is mid-delivery when the window expires.

### Scope

1. Per-member disposition on `CoreCaptureAssemblyRegistry`, replacing the
   `TerminalState` binary. Reachable values only.
2. `CohortState` completion + closed reason on `CoreCaptureCohortRegistry`.
3. The simultaneity window as a project-wide constant, driven off Core's clock
   from admission, with cohort closure swept on the core thread alongside the
   existing retention and admission-watchdog sweeps.
4. A member's error code reaches its result. A failed member is a *present*
   entry carrying its disposition and error, not an absent one.
5. `get_capture_result_set()` distinguishes open from closed internally.
6. Populate `RigState`'s capture counters and `last_capture_id` /
   `active_capture_id`, and delete the "never written, always 0" caveats from
   `state_snapshot.h` — they stop being true here. `cambang_server.cpp`'s rig
   loop becomes reachable for the first time.

### Out of scope

- Arbitration: per-device in-flight guard, rig-preempts-member (tranche 3).
- Rig membership lifecycle and versioning (tranche 4).
- Durable public ids, result fields, trigger returning identity (tranche 5).
- Completion signals, canonical wrappers, outstanding set (tranche 6). Core
  learns the truth here; exposing it is later.
- `STARVED` and any general frame-cadence policy.

### Open decision — the window value

§4.4 says "project-wide constant" and does not give a number. This needs the
maintainer's, not mine, and the two failure directions are asymmetric:

- Too tight turns a slow-but-working camera into `LATE_EXCLUDED`. A camera that
  goes quiet — no pilot frames, `ae_state=255`, payloads seconds late — is a
  known real condition on this hardware, and excluding it silently makes a rig
  return short in a way that looks exactly like a rig defect.
- Too loose lets a genuinely staggered set pass as simultaneous, which is the
  invariant the window exists to protect.

Note the window measures admission-to-settlement on Core's clock, not exposure
spread, so it is bounded below by payload delivery latency rather than by any
tolerance about "the same instant". Proposed starting value **2000 ms**, to be
confirmed or replaced before implementation.

### Acceptance criteria

1. A cohort with one delivered, one failed and one late member closes with
   three distinct dispositions and `WINDOW_EXPIRED`.
2. A cohort whose members all settle closes `ALL_MEMBERS_TERMINAL` without
   waiting out the window.
3. A member that failed with a provider error is present in the result set
   carrying that error. Mutation-proved: reverting it to an absent entry fails.
4. Lateness is decided on Core's clock. A check fails if acquisition marks
   influence membership, lateness or ordering — including the case where two
   members carry identical marks, which is legitimate.
5. `RigState` capture counters and last/active capture id are populated and
   agree with the cohort's own record.
6. `PREEMPTED_BY_RIG` and `DEVICE_LOST` have no producing path, and this is
   asserted rather than left ambiguous.
7. Existing gates green, including the nine deterministic verifiers.

### Validation expectations

Deterministic (required):

- The nine verifiers as listed in tranche 1's commit message.
- Every new rule gets a mutation proof.
- The window is exercised in both directions — a cohort that closes early on
  all-terminal, and one that closes on expiry — using the synthetic provider's
  virtual time rather than wall-clock sleeps.

Godot (required):

- `.\godot_test_suite.ps1`.
- `73_rig_capture_result_set_verification.tscn`, windowed. Tranche 1's
  experience is the reason this is not optional: scene 73 caught an id-space
  defect that all nine native gates missed, because their ids are hardcoded and
  never collided.

Hardware: not required. No provider or device behaviour changes. Note that
`LATE_EXCLUDED` against a genuinely slow device is only observable on hardware,
so its real-world calibration is not proven by this tranche.

**Build the GDE, not just the verifiers** — and build Android too. Tranche 1
nearly shipped an uncompiled edit because `camera2_camera_provider.cpp`
compiles only in `scons gde platform=android arch=arm64`.
