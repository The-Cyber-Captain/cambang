# Capture Identity and Lifecycle

## 0. Status

**Partially implemented. Landing tranche by tranche; §9 is the ledger.**

| | Landed |
|---|---|
| §2.1 separate id spaces | `82fe1e7` |
| §4.3 dispositions, §4.4 cohort closure and the simultaneity window | `c44e787` |
| §3 within-class arbitration | `9084bfe` |
| §5 rig membership lifecycle | tranche 4, in progress |
| §2.2/§2.3/§4.1 durable public ids, result fields, trigger identity | not started |
| §4.2/§4.5 signals, canonical wrappers, outstanding set | not started |

This document defines the target model for capture identity, capture
arbitration, capture completion reporting, and rig-membership lifecycle. Where
it and the source disagree, **source and tests remain the authority**; §9
records what is true today and how it still differs. §9 is updated by each
tranche as it lands — a status section that drifts is worse than none, because
it is read as current.

The **public API is still nearly the pre-implementation one**: `trigger_capture()`
returns `Error`, there are no completion signals, and capture ids are
session-scoped integers. The single addition so far is
`CamBANGRig.add_member` / `remove_member`, taking `CamBANGDevice` handles.
Everything else landed is internal.

**Hardware validation is thin, and should not be assumed from the commits.**
The arbitration rules (§3) are proven host-native and mutation-proved, but
have no hardware coverage: scene 870 stalls on the S20+ for reasons unrelated
to this work, and the one platform-backed run that completed cleanly (WinRT,
after the provider fix in `60d1533`) exercises capture and rig capture rather
than contention. §5.3's `DEVICE_LOST` has no hardware coverage at all — no
existing scene closes a device mid-capture.

This document supersedes `arbitration_policy.md` §9's shared-counter rule,
which is done. It depends on, and must not contradict, `camera_fact_model.md`
§12 (acquisition timing and capture date-time).

---

## 1. Terminology

Three terms, used precisely. "Capture id" unqualified is not a term of art here
and should not appear in code, logs, or documentation.

- **Device Capture** — one device, one trigger, one terminal outcome.
  Identified by a **Device Capture Id**.
- **Rig Capture** — one rig, one trigger, composed of N Device Captures (its
  **members**). Identified by a **Rig Capture Id**.
- **Participation** — a member's place in a rig capture, expressed as
  (Rig Capture Id, member hardware id, member index).

A rig trigger is conceptually equivalent to pressing "capture" on every member
simultaneously — ideally within one engine tick. Members are therefore ordinary
Device Captures, not a separate mechanism.

---

## 2. Identity

### 2.1 Two spaces, not one counter

Device Captures and Rig Captures are different entity kinds and occupy
**separate id spaces**. A rig member's capture draws from the Device Capture
space like any other device capture.

### 2.2 Durable public ids

Capture results may be serialised, stored, and reloaded in a later session or
on another machine. A session-local monotonic counter collides on first reload
and is therefore unfit as a public identity.

- The **public** identity is an opaque, type-prefixed, lexicographically
  sortable string: `dc_<ulid>` for a Device Capture, `rc_<ulid>` for a Rig
  Capture.
- The **internal** identity remains a session-scoped `uint64`, unchanged, used
  for Core keying and hot-path lookups.
- The boundary mints the public id at trigger time and maps between the two.

The type prefix is load-bearing: a Rig Capture Id can never be silently
accepted where a Device Capture Id is expected.

### 2.3 What a result carries

Every `CamBANGCaptureResult` carries, with keys always present for shape
stability:

| Field | Meaning |
|---|---|
| `capture_origin` | `DEVICE` or `RIG`. **Branch on this**, never on an empty slot |
| `device_capture_id` | `dc_…` — always present |
| `rig_capture_id` | `rc_…` when origin is `RIG`; empty string when `DEVICE` |
| `rig_member_hardware_id` | member hardware id when origin is `RIG`; empty when `DEVICE` |
| `rig_member_index` | member index when origin is `RIG`; `-1` when `DEVICE` |

Participation identity is the **hardware id**; the index is a convenience for
the current session only. An index is meaningless against a reloaded result
(there is no rig object to index into, and membership order is not guaranteed
stable across runs).

`device_instance_id` remains present but is session-scoped and carries no
meaning after a reload. It must not be used as durable identity.

---

## 3. Arbitration

Extends `arbitration_policy.md` §2 (rig > device capture > repeating stream)
into contention *within* the triggered-capture class, which that document does
not currently define.

| Situation | Rule |
|---|---|
| Second Device Capture on a device with one in flight | Deterministic denial, `ERR_BUSY` |
| Device Capture on a different device | Permitted; devices are independent |
| Rig Capture over a member's in-flight Device Capture | Rig preempts; the preempted capture terminalises as `PREEMPTED_BY_RIG` |
| Second Rig Capture while one is in flight | Deterministic denial, `ERR_BUSY` |

Triggering is an imperative action about *now*. Queuing a capture trigger for
later execution is never correct: it manufactures the latency this project
exists to avoid and breaks the caller's mental model. Denial is immediate and
carries a reason.

Preemption must never be silent — the preempted Device Capture reports its own
terminal disposition to its own subscriber.

### 3.1 Simultaneity is admission-checked

A rig result set asserts that its images are of one moment. A provider that
cannot execute all members concurrently would deliver a *staggered* set that
looks simultaneous.

Core therefore **refuses a Rig Capture it cannot execute simultaneously**
rather than delivering a staggered set. Simultaneity is a checked invariant,
not an aspiration.

**The capacity is ingested, not declared by the provider.** An earlier draft of
this section said providers declare their concurrent device-capture capacity.
They do not, and on the platforms this project targets they cannot: Camera2
NDK surfaces no runtime concurrency information. The authority is the
camera-concurrency truth ingested through
`CamBANGServer.ingest_camera_description(...)` before `start()`, held as
`allowed_camera_id_combinations`.

That gate exists and fails closed: a rig whose exact member combination has no
accepted truth is refused at creation and again at trigger, with
`ERR_UNCONFIGURED` — a permanent, caller-fixable configuration gap rather than
busy-ness. No `ICameraProvider` capacity method exists, and none should be
added; the sentence this replaces invited exactly that.

---

## 4. Completion and curation

### 4.1 Triggers return identity

```
CamBANGDevice.trigger_capture() -> { id: "dc_…", error }
CamBANGRig.trigger_capture()    -> { id: "rc_…", members: { "<hardware_id>": "dc_…", … }, error }
```

Returning the member map (not a bare list) makes the simultaneous-press model
explicit and gives the caller something to correlate against immediately.

This is consistent with `arbitration_policy.md` §9's stated rationale for
minting ids at the Godot boundary: so a synchronous trigger call can return the
assigned id without a core-thread round trip.

### 4.2 Signals

Completion is reported at two levels:

- **Per object** — a `CamBANGDevice` emits for the Device Captures it
  initiated; a `CamBANGRig` emits for its own Rig Captures. This is the
  ergonomic default: a component subscribes only to the rig it owns.
- **Server-wide** — `CamBANGServer` emits for every settled capture, carrying
  the id. This is the fan-in for cross-cutting consumers (status panels,
  logging, scene-wide orchestration).

**Wrapper objects must be canonical per id.** `get_rig(...)` and
`get_device_for_hardware_id(...)` must return the same wrapper instance for the
same id, so that a handle is a durable thing to subscribe to and identity
comparison behaves. Instancing a fresh wrapper per call makes per-object
signals unreliable by construction.

### 4.3 Dispositions

Per-member terminal disposition:

`DELIVERED` | `FAILED(error)` | `LATE_EXCLUDED` | `PREEMPTED_BY_RIG` |
`DEVICE_LOST` | `NEVER_ARRIVED`

Rig capture closed reason: `ALL_MEMBERS_TERMINAL` | `WINDOW_EXPIRED`.

A member's error code must reach the caller. A capture that timed out with a
known provider error must not be reduced to an absent array entry.

### 4.4 The cut-off is a simultaneity tolerance

A Rig Capture closes when every member is terminal, or when the simultaneity
window expires — whichever comes first. The window is a project-wide constant.

The window is **not** an impatience threshold. A member arriving well outside
it is not part of the same moment, and excluding it (`LATE_EXCLUDED`) is
correct behaviour rather than a failure to wait. A rig capture of six devices
that closes with four delivered, one failed and one late-excluded is a complete
and truthful outcome, and reports the instant at which it closed.

**Clock constraint:** lateness is measured on Core's own clock from capture
admission. `camera_fact_model.md` §12.2 forbids using acquisition timing as
ordering or latency evidence, and §12.1 notes that Capture Date-Time is
deliberately *shared* across one rig capture. Acquisition marks from separate
devices may legitimately be identical and must never be used to decide
membership, lateness, ordering, or identity.

### 4.5 Outstanding work

A caller that records the id a trigger returns (§4.1) and clears it on the
matching settlement (§4.2) knows what it is still waiting for, without polling
anything. That is the ordinary usage this model is designed around, and it is
deliberately sufficient on its own.

`CamBANGServer` must also expose that set directly — the ids it has minted and
not yet seen settle, per device and in total — for callers that would otherwise
maintain it themselves.

This adds no lifecycle concept and no new bookkeeping. Capture ids are minted
at the Godot boundary, and §4.2's settlement signals arrive there; the boundary
therefore sees both ends already. Today it sees only one, because the minted id
is discarded rather than returned (§9), which is exactly what §4.1 fixes.

It is worth exposing rather than leaving to each caller because a device's busy
state is otherwise observable **only by being refused**, and every non-trivial
consumer written against this codebase has hand-rolled the same counter to
avoid that — including the soak scene, which tracks in-flight captures per
device purely so it can decline to ask.

**One caveat, and it is not about timing.** An empty outstanding set for a
device does not guarantee the next trigger is admitted: admission can refuse
for reasons unrelated to busy-ness — materialization backlog, orchestration
failure — and those surface as `ERR_BUSY` too. A caller still handles the
trigger's return. Narrowing what `ERR_BUSY` means, through additional states or
clearer reporting, is worthwhile and is tracked separately from this model.

---

## 5. Rig membership lifecycle

### 5.1 Snapshot at trigger

**Cohort membership is snapshotted when a Rig Capture is triggered. Rig
membership is forward-looking configuration.**

- Removing a device while a Rig Capture is in flight does not alter that
  capture's cohort. The in-flight capture completes with the device as a
  participant; the removal applies from the next trigger.
- Adding a device while a Rig Capture is in flight does not add it to that
  cohort. It participates from the next trigger.

Neither case requires `ERR_BUSY`, and neither can cross-contaminate an
in-flight result set.

### 5.2 Membership is versioned

Rig membership carries a `rig_membership_version`, bumped on change. Each Rig
Capture records the version it was admitted under, so a stored result set is
self-describing about the membership that produced it.

This mirrors the existing device capture-profile pattern, which is accepted
while live and versioned forward.

### 5.3 Device loss is not membership

If a device is disengaged, closed, or otherwise lost while one of its member
captures is in flight, that is a resource event, not a configuration change. It
terminalises that member as `DEVICE_LOST`. A member capture must always reach a
terminal disposition; it must never simply disappear from the cohort.

### 5.4 Removal must settle outstanding provider state

A device leaving a rig with abandoned or lost captures may still owe buffers to
its provider (see §7). Removal must settle that accounting, or it becomes a new
path by which a stale payload is later attributed to an unrelated capture —
including a standalone capture after the device has left the rig.

### 5.5 One rig per device

A device may belong to at most one rig. Cohorts therefore never share
participants, and multiple rigs require no arbitration against one another.
Scaling is bounded by provider capacity (§3.1), not by identity.

---

## 6. Accept, refuse, or version

A single rule governs where `ERR_BUSY` applies and where a request is accepted
and applied forward. Actions are about *now*; configuration is about *from now
on*.

| Kind | Examples | Rule |
|---|---|---|
| Imperative action | trigger a device or rig capture | Fail fast with a reason (`ERR_BUSY`). Never queue for later execution |
| Declarative configuration | still-capture profile, warm policy, rig membership | Accept while live, version it, apply forward. The version makes the transition observable |
| Foundational truth | camera description / concurrency truth | Refuse while the runtime is LIVE |

A short queue with delayed application is the worst option for all three: it
looks accepted, behaves unpredictably, and cannot be reasoned about from
outside.

Arbitration is Core's responsibility. Providers should nonetheless carry
redundant guards for the invariants they depend on — the per-device
single-capture rule especially — so that a later policy change fails loudly at
the seam rather than silently misattributing payloads.

---

## 7. Relationship to the stale-payload repair

The per-device single-capture rule (§3) and removal settlement (§5.4) exist
partly because a provider that abandons a submitted capture may have payloads
still outstanding with the platform, which can be delivered into a subsequent
capture on that device.

Attribution of those outstanding payloads must be by accounting, never by
acquisition timing (§4.4). That repair is independent of this design and does
not wait on it.

---

## 8. Consequences

- Rig members become ordinary Device Captures, so a device-level result
  accessor is no longer blind to rig-originated captures.
- Harnesses that today hand-roll completion detection, staleness guards, and
  expected-member counts should be able to delete that code. If they cannot,
  this design has not taken the burden back and is incomplete.

---

## 9. Implementation-status guardrails (current)

Still true, and still contradicting the model above:

- `CamBANGDevice.trigger_capture()` and `CamBANGRig.trigger_capture()` return
  `Error` only; the minted id is discarded at the boundary. Capture ids remain
  session-scoped `uint64`; there are no durable `dc_`/`rc_` public ids.
- There is no completion signal for either capture kind. `CamBANGRig` exposes
  `get_id`, `trigger_capture`, `get_result` and nothing else.
- **A caller still cannot tell a partial result set from a final one.** Core
  now knows — cohorts close and record per-member dispositions — but none of
  that is exposed, so `get_result()` looks exactly as it did. This is the one
  entry that changed underneath without changing at the surface.
- `get_rig(...)` and `get_device_for_hardware_id(...)` instantiate a new wrapper
  on every call, so per-object signals would be unreliable by construction.
- `NEVER_ARRIVED` and `LATE_EXCLUDED` are produced only by cohort closure;
  every disposition now has a producing path except through the boundary,
  which exposes none of them.
- **§5.5 is enforced for callers but not for authored content.** `create_rig`
  and `add_member` reject a device already in another rig;
  `retain_member_hardware_ids` does not, and it carries synthetic
  scenario-staged rig topology. A scenario can still stage two rigs sharing a
  device.
- **`create_rig` still takes hardware-id strings** while `add_member` /
  `remove_member` take `CamBANGDevice` handles. The handle form is the better
  surface; changing `create_rig` alters a bound signature and belongs with the
  public-API work.
- A capture failed by the 30s admission watchdog is abandoned without
  `abort_capture`, so a provider with a shared delivery queue may still hold
  its payloads (§7). Recorded at
  `CoreCaptureAssemblyRegistry::sweep_admission_timeouts`.

Resolved, kept briefly so a reader coming from an older draft is not misled:

- ~~One `capture_id` shared by device- and rig-triggered captures~~ — separate
  id spaces, numerically disjoint (`82fe1e7`).
- ~~Cohort state is `OPEN` or `FAILED` only~~ — cohorts close with a reason and
  per-member dispositions (`c44e787`).
- ~~No in-flight capture guard in Core; serialisation happens incidentally
  inside a platform provider by blocking~~ — Core denies a second capture on a
  device it has already admitted one for, and providers keep redundant guards
  alongside (tranche 3).
- ~~Per-device capture completion is misattributed for rig captures~~ — fixed
  before this work began; `captures_in_flight_` is keyed by
  `(capture_id, device_instance_id)`.
- ~~Rig membership is fixed at rig creation; there is no add/remove API, and no
  `rig_membership_version`~~ — membership is versioned and mutable via
  `CamBANGRig.add_member` / `remove_member`, applied from the next trigger
  (tranche 4).
- ~~`DEVICE_LOST` has no producing path~~ — a device closed with a capture in
  flight terminalises it `DEVICE_LOST`, promptly and without an error code,
  distinct from the watchdog's `FAILED(ERR_TIMEOUT)` (tranche 4). Note that an
  orderly close is refused by a provider while a capture is in flight, so this
  fires for genuine loss rather than for tidy shutdown.
