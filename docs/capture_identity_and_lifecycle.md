# Capture Identity and Lifecycle

## 0. Status

**Design intent. Not yet implemented.**

This document defines the target model for capture identity, capture
arbitration, capture completion reporting, and rig-membership lifecycle. Until
the implementation lands, **source and tests remain the authority for current
behaviour**; §9 records what is true today and how it differs.

This document supersedes `arbitration_policy.md` §9's shared-counter rule on
implementation. It depends on, and must not contradict, `camera_fact_model.md`
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

Providers therefore declare their concurrent device-capture capacity, and Core
**refuses a Rig Capture it cannot execute simultaneously** rather than
delivering a staggered set. Simultaneity is a checked invariant, not an
aspiration.

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

True at the time of writing, and contradicting the model above:

- `capture_id` is a single monotonic `uint64` shared by device- and
  rig-triggered captures (`arbitration_policy.md` §9). Rig and member ids are
  the same value.
- `CamBANGDevice.trigger_capture()` and `CamBANGRig.trigger_capture()` return
  `Error` only; the minted id is discarded at the boundary.
- There is no completion signal for either capture kind. `CamBANGRig` exposes
  `get_id`, `trigger_capture`, `get_result` and nothing else.
- `get_result()` returns a partial set that is indistinguishable from a final
  one, with no expected-member count and no per-member disposition.
- Cohort state is `OPEN` or `FAILED` only — there is no completion state, no
  cut-off, and no per-member disposition.
- There is no in-flight capture guard in Core; per-device serialisation happens
  incidentally, inside a platform provider, by blocking.
- Rig membership is fixed at rig creation; there is no add/remove API.
- `get_rig(...)` and `get_device_for_hardware_id(...)` instantiate a new wrapper
  on every call.
