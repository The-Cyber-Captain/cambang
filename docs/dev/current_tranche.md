# Current tranche

## Capture arbitration: the per-device guard and rig preemption

Tranche 3 of the `capture_identity_and_lifecycle.md` implementation. Internal
only — no Godot-facing surface changes. Depends on tranche 1 (`82fe1e7`, split
id spaces) and tranche 2 (`c44e787`, dispositions and cohort closure).

### Problem

`arbitration_policy.md` §2 defines contention *between* rig capture, device
capture and streams. It says nothing about contention *within* the
triggered-capture class, and Core enforces nothing there:

- **There is no per-device in-flight guard.** `has_capture_in_flight_for_device()`
  exists and is consumed by exactly one caller — `snapshot_builder` projecting
  `CAPTURING`. Nothing refuses on it. Per-device serialisation happens
  incidentally, inside a platform provider, by blocking.
- **A rig capture cannot preempt a member's in-flight device capture.** §3 says
  it must, and that the preempted capture terminalises as `PREEMPTED_BY_RIG`.
  Tranche 2 defined that disposition and deliberately left it with no producing
  path. This tranche is that path.

The consequence today is that a refusal cannot be attributed. `ERR_BUSY` is
returned for genuine busy-ness, for orchestration failure, and for
materialization backlog alike — which has sent debugging the wrong way more
than once, and which end users hit on slow hardware.

### Decision

**Core arbitrates; providers keep redundant guards.** §6 is explicit that
arbitration is Core's responsibility, and equally that providers should carry
redundant guards for the invariants they depend on — the per-device
single-capture rule especially — so a later policy change fails loudly at the
seam rather than silently misattributing payloads. Both halves are in scope;
neither replaces the other.

**The four within-class rules (§3):**

| Situation | Rule |
|---|---|
| Second device capture on a device with one in flight | Deterministic denial |
| Device capture on a different device | Permitted; devices are independent |
| Rig capture over a member's in-flight device capture | Rig preempts; preempted capture terminalises `PREEMPTED_BY_RIG` |
| Second rig capture while one is in flight | Deterministic denial |

**Preemption is never silent.** The preempted device capture reports its own
terminal disposition to its own subscriber. It does not simply vanish, and its
result must not be attributed to the rig capture that displaced it.

**Triggering is imperative and about *now*.** A denial is immediate and carries
a reason. Queuing a trigger for later execution is never correct: it
manufactures the latency this project exists to avoid, looks accepted, behaves
unpredictably, and cannot be reasoned about from outside.

### Scope

1. Per-device in-flight guard in Core's device-capture admission, refusing a
   second capture on a device that already has one.
2. Second-rig-capture denial while a rig capture is in flight.
3. Rig preemption of a member's in-flight device capture, terminalising it
   `PREEMPTED_BY_RIG` — the producing path tranche 2 left open.
4. A preempted capture's outstanding provider payload accounting is settled, so
   a payload owed to the displaced capture cannot later be attributed to the
   rig capture that replaced it (§7).
5. Provider-side redundant guards for the per-device rule (§6).
6. Correct `arbitration_policy.md` §2's forward reference and
   `capture_identity_and_lifecycle.md` §3.1's "providers therefore declare
   their concurrent device-capture capacity" — see below.
7. **Update `capture_identity_and_lifecycle.md` §0 and §9 to match what has
   landed.** This is a standing obligation of every tranche in this branch from
   here on, not a one-off: §9 is the ledger of how the source still differs
   from the model, and a status section that drifts is worse than none because
   it is read as current. Tranche 3 also catches up the entries tranches 1 and
   2 left stale.

### Out of scope

- **Narrowing `ERR_BUSY`.** §4.5 notes that admission can refuse for reasons
  unrelated to busy-ness and that all of them surface as `ERR_BUSY` today.
  Making a refusal attributable is worth doing and is tracked separately; doing
  it here would mean changing a public error code mid-tranche, and tranche 5
  owns the public surface.
- Rig membership lifecycle and `DEVICE_LOST`'s producing path (tranche 4).
- Durable public ids, result fields, trigger returning identity (tranche 5).
- Completion signals, canonical wrappers, outstanding set (tranche 6).

### Settled: §3.1 needs no provider capacity method

§3.1 says "Providers therefore declare their concurrent device-capture
capacity." **They do not, and on this hardware they cannot.** Camera2 NDK
surfaces no runtime concurrency information, and the ingested camera-concurrency
truth (`camera_concurrency_adc.h`, `allowed_camera_id_combinations`) is the only
gate. That gate already exists and already fails closed: a rig combination with
no accepted truth is refused with `ERR_UNCONFIGURED` before admission.

So no `ICameraProvider` capacity method will be added. The doc sentence is
wrong as written and, left alone, invites exactly the method we have decided
against. Correcting it is scope item 6.

### Acceptance criteria

1. A second device capture on a device with one in flight is refused, and the
   first capture is unaffected — it still reaches its own terminal disposition
   with its own result.
2. A device capture on a *different* device is admitted while the first is in
   flight. Devices are independent, and a guard that over-refuses is worse than
   none.
3. A rig capture over a member's in-flight device capture preempts it. The
   preempted capture terminalises `PREEMPTED_BY_RIG`, and its subscriber can
   see that rather than silence.
4. A second rig capture while one is in flight is refused.
5. A payload owed to a preempted capture is never attributed to the rig capture
   that displaced it.
6. Every rule is mutation-proved: removing it must fail a check. The false-
   positive direction gets its own check for criteria 1 and 2 — a guard that
   refuses a legitimate capture would be a worse regression than the gap it
   closes.
7. Existing gates green, including the nine deterministic verifiers.

### Validation expectations

Deterministic (required):

- The nine verifiers.
- Both directions for the per-device guard: refusal when busy, admission when
  not. A guard proven only in the refusing direction is half-proven.
- Provider-side redundant guards exercised in `provider_compliance_verify`.

Godot (required):

- `.\godot_test_suite.ps1`.
- `73_rig_capture_result_set_verification.tscn`, windowed.
- `71_capture_session_matrix_v3.tscn`, which drives concurrent captures across
  two devices and is the scene most likely to expose an over-refusing guard.

Hardware (required before acceptance): **this tranche changes device
behaviour.** Per-device serialisation currently happens by blocking inside a
platform provider; moving the decision into Core changes what a real device
does under contention, and no host-native check can stand in for that. Scene 71
and scene 73 on the S20+ and on the eMeet C970.

**Build the GDE, not just the verifiers**, and build Android — the redundant
provider guard lands in `camera2_camera_provider.cpp`, which compiles only in
`scons gde platform=android arch=arm64`.
