# Synthetic Timeline Scenarios (Architecture Supplement)

This supplement fixes the execution meaning and ownership boundary of CamBANG **scenarios** for authored synthetic timeline work.

It supplements:

- `docs/naming.md`
- `docs/provider_architecture.md`
- `docs/core_runtime_model.md`
- `docs/architecture/godot_boundary_contract.md`

It does not redefine those canonical documents; it resolves scenario-specific ambiguity for implementation tranches.

---

## 1. Purpose

This document exists to prevent terminology and authority drift while authored scenario support is introduced.

Historically, dev-node micro-scenarios mixed “scenario” wording with Godot-side procedural triggers. This supplement sets the canonical model for Tranche 1 onward:

- scenario semantics belong to SyntheticProvider timeline execution
- scenarios are low-level provider/core timeline replay, diagnostics, metrics,
  and fault-reproduction tooling rather than the primary public UX automation layer
- Godot hosts execution but does not define timeline meaning
- normal public UX automation should use the object-level Godot APIs
- maintainer verification cases remain a separate concept

---

## 2. Terminology

The following terms are distinct and must not be conflated:

- **scenario**: SyntheticProvider/provider-core timeline replay artifact for diagnostics, metrics, fault reproduction, and recorded/authored behavior playback
- **scenario library**: umbrella term for collections of scenarios
  - **built-in scenario library**: current C++-authored scenario collection
  - **external scenario library**: file-backed/user-provided scenario collections loaded through the scenario loader
  - **scenario loader**: serialized ingestion path for external scenario JSON into canonical SyntheticProvider timeline scenarios
- **verification case**: maintainer-authored smoke/CLI procedural validation input
- **verification case catalog**: maintainer tooling selector/list for verification cases (`verify_case_catalog`)
- **fixture**: static payload/data artifact, often consumed by tooling or UI/scene checks

A verification case may exercise a scenario. A fixture may be used by either. None of these terms are interchangeable.

---

## 3. Ownership and authority

Scenario semantics are owned by the **SyntheticProvider timeline subsystem**.

Authoritative scenario meaning includes:

- event ordering semantics
- event execution preconditions
- deterministic tie-breaking and scheduling
- interaction with stream lifecycle and frame emission paths

Godot dev glue (`CamBANGDevNode`-style helpers) is not a semantic authority. It may trigger and observe, but it must not redefine scenario meaning.

---

## 4. Time model

Scenario time is **synthetic time** in the provider virtual clock domain.

For `virtual_time` timing:

- time advances only when the host advances it
- no wall-clock progression is implied
- execution is deterministic with respect to host-provided `dt_ns`

For this first authored slice, host advancement is expected through the existing Godot tick → broker virtual-time pump path. The timeline subsystem consumes that synthetic time and executes due events.

---

## 5. Canonical scenario model (current slice)

The current supported canonical scenario forms are:

- built-in C++ authored scenario data
- external JSON text/file ingestion through the scenario loader into canonical
  `SyntheticProvider` timeline scenarios

In-scope representation characteristics:

- an authored list/sequence of scheduled timeline events
- explicit synthetic timestamp (`at_ns`) per event
- deterministic intra-timestamp ordering (stable sequence/tie-breaker)
- provider-owned execution against active stream/device state

This JSON ingestion path is maintainer/provider-core tooling. It does not by
itself define a finalized release UX scenario library, editor, recording
pipeline, or broad product authoring surface.

### 5.1 Single-scenario-model clarification

CamBANG continues to use **one scenario model**.

- scenarios remain primitive-capable
- existing primitive lifecycle vocabulary remains valid for diagnostics and replay
- this clarification does **not** introduce a second scenario-handling model

There is currently no alternate runtime scenario model that replaces primitive events.

---

## 6. Host integration boundary

Godot is a thin host/stepper/observer for scenario execution.

Scenario playback is not the primary public UX automation layer; ordinary
application/user flows should be expressed through the object-level Godot APIs.

Godot may:

- select a built-in scenario or load an external JSON scenario
- start / stop scenario execution
- pause / resume execution
- advance synthetic time
- observe outcomes through the normal runtime snapshot boundary

Godot must not:

- become the semantic owner of scenario execution
- reimplement timeline/event semantics in dev glue
- define alternate meaning for event ordering or timing

### 6.1 Execution arming compatibility rule (Timeline role)

To preserve existing verifier behavior while keeping explicit host controls:

- **config-seeded/provider-init-owned scenario path**
  - when `SyntheticProvider` Timeline role initializes with a non-empty
    config-seeded scenario event set, execution is armed as running/unpaused
    at initialize and `advance(dt_ns)` pumps immediately
  - purpose: preserve legacy verifier/direct-start compatibility without
    requiring host start controls

- **host-submitted scenario path**
  - when a scenario is supplied later via host control entry points, it is
    staged provider-side and remains not-running until explicit host start
  - Godot may accept this staging before the first observable runtime baseline
    when synthetic timeline mode is active; the staged data remains provider-owned
  - if Godot receives `start_scenario()` before that baseline, it may hold only
    that high-level playback intent and begin actual provider playback after
    `state_published(gen, 0, 0)` has been emitted
  - this deferred playback intent is not a general pre-baseline command queue:
    device/stream/rig/capture commands and `advance_timeline(...)` remain
    governed by the Godot runtime boundary
  - purpose: preserve thin-host explicit-control semantics for
    Godot/dev-hosted scenarios while keeping the baseline snapshot clean

This is a compatibility arming rule, not a semantic ownership split:
both paths remain SyntheticProvider-owned timeline execution.

### 6.2 `timeline_reconciliation` applicability and mode intent

`timeline_reconciliation` is a scoped synthetic timeline control, not a global
provider option.

It is valid only when all of the following are true:

- provider kind is synthetic
- synthetic role is timeline
- timing driver is `virtual_time`

Rules:

- omitted in that applicable mode defaults to `completion_gated`
- supplied outside that applicable mode is deterministic invalid-argument
  rejection

Mode intent:

- `completion_gated` is the standard/default mode for synthetic timeline +
  `virtual_time`; destructive progression waits for readiness truth before
  parent/resource removal
- for stream-destruction progression, readiness truth includes stream-stopped
  truth and stream-buffer-release truth
- `strict` remains a diagnostic/power-user mode; authored destructive intent is
  exercised more directly and may fail in-band when readiness truth has not yet
  arrived

---

## 7. Initial event vocabulary and expansion boundary

Current implemented executable slice includes:

- stream flow events: `StartStream`, `StopStream`, `EmitFrame`
- minimal realization/lifecycle events: `OpenDevice`, `CloseDevice`, `CreateStream`, `DestroyStream`
- stream-scoped appearance update event: `UpdateStreamPicture` (maps through provider `PictureConfig` semantics)

Implementation-status note (current verified path): stream appearance authoring
in the built-in scenario library is verified through explicit
`UpdateStreamPicture` events. In this slice, embedded picture payload on
`StartStream` is not documented as the canonical/effective stream-picture
application path.

This implemented slice is still a starting boundary, not the architectural ceiling.

Canonical scenario direction remains a self-contained authored/recorded timeline unit, and event vocabulary may still expand further as needed without moving semantic authority into host glue. Current executable support already includes lifecycle/realization events needed for provider-owned replay; future vocabulary expansion must preserve that ownership boundary.

### 7.x AcquisitionSession guardrail

`AcquisitionSession` is not a directly scenario-authored timeline primitive in
this vocabulary slice.

Scenario events do not include direct AcquisitionSession create/destroy commands.
Any AcquisitionSession realization remains provider runtime truth. Current
`SyntheticProvider` truth includes both stream-backed and capture-only
AcquisitionSession realization, but that realization is still provider-owned
rather than directly scenario-authored.

### 7.1 Verified primitive lifecycle status

Primitive lifecycle operations have been verified in a completion-aware native verification path (`provider_compliance_verify`) for the sequence:

1. `OpenDevice`
2. `CreateStream`
3. `StartStream`
4. `StopStream`
5. wait for authoritative stop completion
6. `DestroyStream`
7. wait for authoritative destroy completion
8. `CloseDevice`
9. wait for authoritative close completion

This means recent stuck/variable teardown observations in Godot scenario playback are **not** sufficient evidence that primitive lifecycle support is missing or fundamentally broken.

### 7.2 Clustered destructive-event boundary sensitivity (current issue)

Under current strict primitive playback, tightly adjacent destructive events such as:

- `StopStream`
- `DestroyStream`
- `CloseDevice`

may be dispatched in an advancement boundary context where later destructive operations are attempted before earlier completion is yet authoritative.

Under strict semantics, those later operations may then fail truthfully. This reflects strict primitive playback boundary sensitivity; it is not:

- snapshot fabrication
- panel/state invention
- provider-side auto-cascade

No new playback policy is implemented by this clarification.

Any future work on this issue, if adopted later, must stay within the single scenario model and may be considered in future for **reduction-facing** teardown handling (close/destroy facing), not implicit upward realization (`OpenDevice` / `CreateStream` / `StartStream`).

---

## 8. Scenario source compatibility requirement

Scenario architecture must remain compatible with all expected source families:

- developer/maintainer authored scenarios
- external JSON text/file inputs loaded through the scenario loader
- recorded behavior from real hardware-backed providers for later synthetic playback
- future GUI authoring tools built as separate layers

Current external JSON ingestion is an implemented loader path for SyntheticProvider
timeline scenarios. It is not a completed release scenario-library UX, recording
pipeline, or editor/product authoring surface.

---

## 9. Relationship to verification cases

Maintainer smoke/CLI verification cases are not scenarios.

Verification cases may:

- construct or select scenarios
- drive scenario execution deterministically
- assert runtime/publication invariants

But the scenario remains a SyntheticProvider timeline artifact, while the verification case remains a procedural maintainer validation flow.

---

## 10. Non-goals (explicit)

This tranche explicitly excludes:

- final release scenario-library product UX
- recording pipeline/format design
- editor/authoring UI tooling
- redesign of smoke verification-case architecture
- adversarial snapshot playback design
- unrelated core/provider runtime redesign

---

## 11. Immediate implementation implications

Next implementation tranche should:

1. move existing `CamBANGDevNode` micro-scenario behavior behind provider-owned timeline scenarios
2. keep Godot-side controls at host operations (select/start/stop/pause/resume/advance/observe)
3. preserve existing snapshot/publication boundary semantics while changing scenario ownership

This enables incremental replacement of dev-node scenario logic without changing core runtime authority or Godot observable contracts.
