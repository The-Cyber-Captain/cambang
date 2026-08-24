# Capture Identity and Lifecycle

## 0. Status

**Implemented, bar two pre-existing gaps in §5.2 and §6. §9 is the ledger, and
it cites source for every claim.**

Per-section state, read from the code rather than from the tranche that claimed
it. §9.2 says how each mandate is met; §9.8 lists what remains thin.

| Section | State | Where |
|---|---|---|
| §2.1 separate id spaces | complete | `82fe1e7` |
| §2.2 durable `dc_`/`rc_` public ids | complete | uncommitted |
| §2.3 what a result carries | complete | uncommitted |
| §3 within-class arbitration | complete | `9084bfe` + uncommitted |
| §3.1 simultaneity admission-checked | complete | pre-existing + `c44e787` |
| §4.1 triggers return identity | complete | uncommitted |
| §4.2 signals | complete | uncommitted |
| §4.2 canonical wrappers | complete | uncommitted |
| §4.3 dispositions | complete | `c44e787` + uncommitted |
| §4.4 cohort closure and the simultaneity window | complete | `c44e787` |
| §4.5 outstanding-set query | complete | uncommitted |
| §5.1, §5.3 membership lifecycle and `DEVICE_LOST` | complete | `42c540a` |
| §5.2 membership versioning | partial — recorded in the cohort, not reachable by a caller | `42c540a` |
| §5.4 removal settles provider state | complete | uncommitted |
| §5.5 one rig per device | complete | `42c540a` + uncommitted |
| §6 accept, refuse, or version | partial — warm policy carries no version | pre-existing |
| §7 attribution by accounting | complete — every abandonment path aborts | uncommitted |
| §8 consequences | complete | uncommitted |

Rows marked "uncommitted" are in the working tree only and have no commit to
cite.

This document defines the target model for capture identity, capture
arbitration, capture completion reporting, and rig-membership lifecycle. Where
it and the source disagree, **source and tests remain the authority**; §9
records what is true today and how it still differs.

**§0 and §9 are updated by whatever change lands a part of this model, in the
same commit as the code.** Not by a work order that happens to remember to say
so — that instruction has died with two tranche files already, and in between
a comment asserting dead code survived the tranche that made it live. A status
section that drifts is worse than none, because it is read as current.

**§8 is met, which is the section that decides whether the rest was worth
doing.** Harnesses have deleted their hand-rolled completion detection rather
than rewritten it: scene 569 and the soak benchmark now gate on
`capture_finished`, and the per-device baselines, the `_device_last_capture_id`
helper and the `capture_id > baseline` comparisons are gone.

**Ids are durable.** §2.2 landed: `dc_<ulid>` / `rc_<ulid>`, monotonic within a
millisecond so a rig's members sort in the order they were minted. The internal
`uint64` is unchanged and still keys Core.

Two things remain partial, both pre-existing and neither introduced by this
work: §5.2's membership version is recorded but not reachable by a caller, and
§6's warm policy carries no version. §9.2 states both with source.

A note on how this document has been audited. An earlier version claimed every
mandate was met on the strength of a search for the word "must" — which missed
§2.3, §5.2 and §8, all of which state their requirement in a purpose clause
("so that…", "should be able to…") rather than an imperative. A requirement
here is not only a sentence containing "must", and a status section built that
way will keep reporting done.

**What was agreed, and what was not.** The completion verb (`finished`), the
single server-wide signal, the bound constants, and `create_rig` taking device
handles were decided by the maintainer in conversation. The per-object signal
payloads are implementation choices: §4.2 names no signal and defines no
payload. §9.1 lists what is actually bound.

**Hardware validation is thin, and should not be assumed from the commits.**
The arbitration rules (§3) are proven host-native and mutation-proved, but have
no hardware coverage: scene 870 stalls on the S20+ for reasons unrelated to
this work, and the one platform-backed run that completed cleanly (WinRT, after
the provider fix in `60d1533`) exercises capture and rig capture rather than
contention. §5.3's `DEVICE_LOST` has no hardware coverage at all — no existing
scene closes a device mid-capture. Nothing in §4.1 or §4.2 has run on hardware.

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
- **Server-wide** — `CamBANGServer` emits for every finished capture, carrying
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
matching completion signal (§4.2) knows what it is still waiting for, without polling
anything. That is the ordinary usage this model is designed around, and it is
deliberately sufficient on its own.

`CamBANGServer` must also expose that set directly — the ids it has minted and
not yet seen finish, per device and in total — for callers that would otherwise
maintain it themselves.

This adds no lifecycle concept and no new bookkeeping. Capture ids are minted
at the Godot boundary, and §4.2's completion signals arrive there; the boundary
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

Every entry below names the source it was read from. Where an entry and the
source disagree, the source wins and the entry is wrong.

### 9.1 The bound Godot surface, as it stands

Read from `src/godot/*.h` and the `_bind_methods()` bodies.

```
CamBANGDevice.trigger_capture()   -> Dictionary { id: "dc_…", error }
CamBANGRig.trigger_capture()      -> Dictionary { id: "rc_…",
                                                 members: { hw: "dc_…" }, error }
CamBANGRig.add_member(device)     -> Error
CamBANGRig.remove_member(device)  -> Error
CamBANGServer.create_rig(devices) -> CamBANGRig      // Array[CamBANGDevice]

CamBANGDevice.capture_finished(capture_id: String, disposition, error_code)
CamBANGRig.capture_finished(rig_capture_id: String, closed_reason)
CamBANGServer.capture_finished(capture_id: String, info)
    info = { capture_origin, device_instance_id, rig_id,
             disposition, closed_reason, error_code }

CamBANGServer.get_unfinished_captures() -> Dictionary
    { by_device:    { device_instance_id: [device_capture_id, ...] },
      rig_captures: [rig_capture_id, ...],
      total:        int }

CamBANGRig.get_member_outcomes()                          -> Array[Dictionary]
CamBANGServer.get_capture_member_outcomes_by_id(rig_id)   -> Array[Dictionary]
    { hardware_id, device_instance_id, device_capture_id,
      disposition, error_code }
```

The signal verb and the query share one word deliberately: the signal announces
a capture reaching a state, the query asks which have not reached it. Split
across two vocabularies, a caller can guess neither. The verb was chosen by the
maintainer.

Three signals: one per object, plus one server-wide covering both kinds. The
server-wide signal is deliberately not split per kind — an object cannot carry
two signals of one name, and letting that C++ constraint shape the caller's API
is backwards. §4.2 names no signal and specifies no payload.

**`CamBANGServer`'s signals, and its Device/Stream/Rig control surfaces, are
advanced tools.** Ordinary use goes through the handle a caller already holds;
device creation is the exception. Additions do not belong on this surface
unless they genuinely have no per-object home — Godot's own API is cluttered
enough without help.

This governs what may be **added**. It is not a mandate to remove what is
already there: taking an existing method away costs a migration and breaks
callers, so each removal needs its own justification on its own merits. Clutter
alone is not one.

The completion verb, collapsing the server signal to one, `create_rig` taking
device handles, and the constants in §9.3 were all decided by the maintainer in
conversation.

Canonical wrappers: `get_device_for_hardware_id`, `get_rig` and `create_rig`
return one cached instance per id. `get_device(instance_id)` is canonical for
its own key and deliberately not unified with the endpoint form — the reason is
recorded at `CamBANGServer::get_device` in `cambang_server.cpp`.

### 9.2 Mandates not met

- **§5.2 — "so a stored result set is self-describing about the membership
  that produced it".** The cohort records `rig_membership_version` correctly,
  but nothing at the boundary exposes it: not the snapshot, not
  `get_capture_identity()`, not `get_member_outcomes()`. The recording half is
  met; the self-describing half is not.
- **§6 — declarative configuration is versioned so the transition is
  observable.** Still-capture profile has `capture_profile_version` and rig
  membership has `rig_membership_version`. Warm policy, named in §6's own
  table, has no version anywhere in Core or the snapshot
  (`CoreDeviceRegistry::set_warm_hold_ms`). Pre-existing, not introduced here.

### 9.2b How the contested mandates are met

- **§8, second consequence — harnesses delete their hand-rolled completion
  detection.** Scene 569 awaits `capture_finished` instead of polling
  `get_result()` and comparing capture ids.
  `870_to_image_soak_benchmark.gd` subscribes once to the server-wide signal
  and gates both its device and rig polls on it; `baseline_capture_id`,
  `_device_last_capture_id` and the per-device `last_capture_id` bookkeeping
  are all removed. Verified against the matching synthetic baseline
  (`20260812T204338394Z`, `config.provider = synthetic`): 54 rig captures, 324
  rig member samples and 384 device captures in both, with phase cadence
  matching to 0.1s.

- **§8, first consequence — a device-level accessor is no longer blind to
  rig-originated captures.** The rig trigger path now records each member's
  Device Capture Id against its device, exactly as a direct trigger does, so a
  device that has only ever been a rig member resolves through
  `CamBANGDevice::get_result()`'s fallback instead of returning null. Scene 73
  step 22 asserts each member device returns its own result from this rig
  capture; removing the recording reproduces the null exactly.

  `get_result()` still prefers a capture triggered through that handle over the
  device's latest. Inverting that order was tried and reverted: "latest on the
  device" lets a rig member's result satisfy a caller waiting on its own device
  capture, and `870_to_image_soak_benchmark.gd` polls precisely that way, so the
  inversion would have let a rig member's timing be recorded as a device
  capture's. §8 asks that the accessor not be blind, not that it always return
  the newest regardless of origin.

- **§4.3 — a member's error code must reach the caller.**
  `get_capture_result_set_by_id` still returns only results that exist, and a
  failed member still contributes none. `get_member_outcomes()` answers the
  question instead: one entry per member whether or not it produced an image,
  carrying its disposition and provider error code. Scene 73 asserts the
  entry count equals the member count and that each outcome names the same
  Device Capture Id the trigger returned.
- **§5.4 — removal must settle outstanding provider state.** Held by
  construction rather than by code on the removal path. §5.4 concerns a device
  leaving "with abandoned or lost captures"; every path that terminalises a
  capture without a provider terminal fact now issues `abort_capture` at the
  moment of abandonment — rig preemption (`core_runtime.cpp`, pre-existing),
  the admission watchdog, and `DEVICE_LOST`. A device therefore cannot carry an
  unsettled capture out of a rig, because none is left unsettled. Removal
  itself continues not to disturb an in-flight capture (§5.1).
- **§3 — preemption must never be silent.** The disposition is produced,
  queued, fanned out to the device wrapper, and nameable (§9.3). Scene 73 now
  subscribes to each member device's `capture_finished` and asserts it arrives
  carrying that member's own Device Capture Id, so the reporting path is proven
  to carry. The `PREEMPTED_BY_RIG` value specifically is mutation-proved in
  Core (tranche 3) rather than exercised through a scene.

### 9.3 Interpreting what the signals carry

`CamBANGServer` binds `DISPOSITION_DELIVERED`, `DISPOSITION_FAILED`,
`DISPOSITION_LATE_EXCLUDED`, `DISPOSITION_PREEMPTED_BY_RIG`,
`DISPOSITION_DEVICE_LOST`, `DISPOSITION_NEVER_ARRIVED`,
`COHORT_CLOSED_ALL_MEMBERS_TERMINAL`, `COHORT_CLOSED_WINDOW_EXPIRED` and
`RIG_CAPTURE_ID_BASE`. So `disposition` compares against `DISPOSITION_*`,
`closed_reason` against `COHORT_CLOSED_*`, and an id can be tested against
`RIG_CAPTURE_ID_BASE` to tell which space it belongs to.

`73_rig_capture_result_set_verification.gd` uses the bound constants and holds
no literals.

### 9.4 The disposition argument is narrower than its enum

`TerminalState` has seven values. Only four can reach `capture_finished`:
`DELIVERED`, `FAILED`, `PREEMPTED_BY_RIG`, `DEVICE_LOST` — the four sites that
assign `terminal_state` in `core_capture_assembly_registry.cpp`.
`LATE_EXCLUDED` and `NEVER_ARRIVED` are assigned only as cohort member outcomes
(`core_capture_cohort_registry.cpp:208`, `core_runtime.cpp:4133`), never as an
assembly terminal state. A caller matching on either would wait indefinitely.
All six are nonetheless bound (§9.3): they are the model's vocabulary, and the
two unreachable ones become caller-visible when §4.3's per-member reporting
lands.

### 9.5 The device signal fires more broadly than §4.2 describes

§4.2 says a `CamBANGDevice` emits for the Device Captures **it initiated**.
`CamBANGServer::_emit_capture_completion_signals_` matches on
`device_instance_id` alone, with no origin filter, so a rig-member capture also
fires the member device's signal.

### 9.6 What a result carries (§2.3)

`CamBANGCaptureResult::get_capture_identity()` returns `capture_origin`,
`device_capture_id`, `rig_capture_id`, `rig_member_hardware_id`,
`rig_member_index` and `device_instance_id`, every key present in every case.
Origin is resolved by asking whether any cohort claims this Device Capture
(`CoreRuntime::rig_participation_for_device_capture`); no cohort means the
capture was device-triggered, so the absence is the answer rather than a
failed lookup.

Both branches are covered: scene 73 asserts a rig member's result names its
rig capture id, hardware id and member index and correlates them to the
trigger's member map; scene 569 asserts a device-triggered result reports
origin DEVICE with rig_capture_id 0, empty hardware id and index -1.

Ids are the durable `dc_`/`rc_` form (§2.2), so a stored result can be
identified in a later session. `get_capture_id()` is retired: §1 says the
unqualified term should not appear in code, and the internal `uint64` it
returned was never a caller's identity. Its dominant use was a staleness guard,
which §8 removes rather than rewrites.

### 9.7 Ids

Public capture ids are `dc_<ulid>` / `rc_<ulid>`, minted at the boundary by
`CapturePublicIdMinter` (`src/core/capture_public_id.{h,cpp}`) and mapped to the
internal `uint64` that still keys Core. The minter is monotonic within a
millisecond -- a rig's members are minted in one instant, and fresh entropy per
member would order them at random. Ordering across a backwards clock step is not
guaranteed; the high bits are wall clock, which is what makes ids from different
sessions comparable at all.

The prefix is enforced, not decorative: `device_capture_internal_id()` refuses an
`rc_` id rather than failing to find it, so a caller learns it passed the wrong
kind of id instead of "no such capture".

The maps are session-scoped and cleared on `stop()`. A durable id identifies a
STORED artifact; it does not resurrect one after the session that made it.

`get_capture_result_by_id(capture_id)` takes the id alone. It required a
`device_instance_id` alongside until the id spaces were split (§2.1) — after
that the argument disambiguated nothing, because a Device Capture Id belongs to
exactly one device, and the store's per-device nesting survives only for
internal callers that already hold the device. It was removed as a defect: a
parameter that outlives its reason teaches callers a constraint that is not
real. `CoreResultStore::get_capture_result(capture_id)` returns null rather
than guessing if that id ever names more than one device, so a failure of the
id split would surface instead of hiding.

### 9.8 Other open items

- **`last_capture_id` was removed from the published snapshot.** It lived on
  `RigState` and `AcquisitionSessionState` and was not state at all -- it was
  the residue of a past event, published so a consumer could diff it against
  the previous tick. That is the staleness-guard pattern §8 removes, and it was
  its only real consumer: retained-result calibration also read it, and now
  reads the boundary's own records instead
  (`latest_capture_id_by_device_instance_id_`,
  `latest_rig_capture_id_by_rig_id_`). Removed from the struct, the builder,
  the export, the v1 schema, the status panel, 20 fixtures and the scenes.
  Core's internal `last_capture_id` on the rig and acquisition-session records
  is untouched.

  The schema was edited in place rather than up-versioned, at the maintainer's
  direction: pre-Release, and there is no consumer to keep compatible.
- The word "settle" survives in `src/` only in unrelated senses: backing-plan
  settle delays, "the answer is settled" meaning decided, and §5.4's
  accounting sense of settling what is owed. Every identifier and comment
  naming *this* concept says "finished".

- **§5.5 is enforced on all three paths.** `create_rig` and `add_member`
  consult `rig_owning_member`; the scenario loader rejects a document placing
  one device in two rigs (`scenario_loader_validate.cpp`, "overlapping rig
  membership is not supported"); and `CoreRuntime::retain_rig_member_hardware_ids`
  now checks too. The loader's check is per-document and cannot see a rig the
  caller created earlier in the session, which is the gap the third one closes.
  `CoreRigRegistry::retain_member_hardware_ids` remains an unchecked primitive
  by design -- the rule lives in Core's command layer, not the registry.
- **The abandonment aborts are implemented but not covered by a test.** The
  watchdog and `DEVICE_LOST` paths now call `abort_capture` alongside rig
  preemption. Proving it host-native needs a provider that accepts a capture
  and then goes silent, with a short watchdog timeout; `StubProvider` is
  `final` and completes captures synchronously, so it cannot be subclassed for
  the purpose. Written against the pattern rig preemption already proves, and
  the comment at `sweep_admission_timeouts` records that returning a timed-out
  assembly without aborting it re-opens §7.

### 9.9 Scene status

Verdicts from `tests/cambang_gde/run-logs/`, 2026-08-24, Windows.

- Pass: 62, 63, 65, 66, 73, 74, 568, 569, 768. Scene 70 passes headless.
- **Scene 70 is interactive in windowed mode**, and does not self-verdict
  there: on success it enters inspection mode and waits for Esc, emitting its
  verdict only on quit. Run unattended with `-Windowed` it passes every step
  and then hits the runner timeout. Only headless self-quits.
- **`64_status_panel_runtime_smoke` and `67_status_panel_scenario_runtime` are
  manual scenes.** Neither driver script contains a `HarnessVerdict` or a
  `quit()`; they exist to be looked at and closed. A `missing_harness_verdict`
  from either is the launcher correctly describing a scene that never emits
  one, not a fault. Do not read them as regressions, and do not run them
  expecting a verdict.
- Scene 73 carries the completion coverage: the trigger names its capture in
  the rig id space with a member map; the outstanding set lists the rig and its
  members in flight and clears once they finish; the rig, server-wide and
  per-device `capture_finished` signals all arrive and correlate to the ids the
  trigger returned; and every member is accounted for by disposition. Scene 74
  asserts wrapper canonicity.
- `68_inner_evidence_reset_verify` fails at step 4, and
  `71_capture_session_matrix_v3` reaches the runner timeout with no verdict.
  Both fail identically against a build of `HEAD` without tranche 5, so neither
  is caused by that work. `run-logs/` holds no earlier entry for either, which
  is a gap in the record rather than evidence about their history.
- `911_acquisition_session_states` selects the platform-backed provider on this
  host, enumerates the real USB cameras, then requests the synthetic hardware
  id `"0"`.
- `1001_basic_quest_snap.tscn` is not migrated: it calls
  `create_rig(PackedStringArray(...))` and treats `trigger_capture()` as
  returning an `Error`. It carries uncommitted maintainer edits and was left
  untouched.
- The `PREEMPTED_BY_RIG` disposition has no scene coverage: it is
  mutation-proved in Core, but no scene contends a device capture against a
  rig trigger.
- **The status panel's display change is unverified.** Removing the
  `last_capture_id` row from `cambang_status_panel.gd` is display-only, and the
  field was already `"required": false` so the panel tolerated its absence by
  construction -- but that is reasoning, not evidence, and the only scenes that
  would exercise it are the manual ones above.

### 9.10 Resolved, kept so a reader coming from an older draft is not misled

- ~~One `capture_id` shared by device- and rig-triggered captures~~ — separate
  id spaces, numerically disjoint (`82fe1e7`).
- ~~Cohort state is `OPEN` or `FAILED` only~~ — cohorts close with a reason and
  per-member dispositions (`c44e787`).
- ~~No in-flight capture guard in Core~~ — Core denies a second capture on a
  device it has already admitted one for (`9084bfe`).
- ~~Per-device capture completion is misattributed for rig captures~~ — fixed
  before this work began; `captures_in_flight_` is keyed by
  `(capture_id, device_instance_id)`.
- ~~Rig membership is fixed at rig creation~~ — membership is versioned and
  mutable, applied from the next trigger (`42c540a`).
- ~~`DEVICE_LOST` has no producing path~~ — a device closed with a capture in
  flight terminalises it `DEVICE_LOST` (`42c540a`).
- ~~`trigger_capture()` returns `Error` only~~ — both return a Dictionary; the
  rig's carries the member map (§9.1).
- ~~`get_rig(...)` and `get_device_for_hardware_id(...)` instantiate a new
  wrapper per call~~ — canonical per id (§9.1).
- ~~`create_rig` takes hardware-id strings~~ — it takes device handles (§9.1).
