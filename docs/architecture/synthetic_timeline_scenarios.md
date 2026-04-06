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
- Godot hosts execution but does not define timeline meaning
- maintainer verification cases remain a separate concept

---

## 2. Terminology

The following terms are distinct and must not be conflated:

- **scenario**: user-facing / Godot / SyntheticProvider timeline recording-storage-execution concept
- **scenario library**: umbrella term for collections of scenarios
  - **built-in scenario library**: current C++-authored scenario collection
  - **external scenario library**: future file-backed/user-provided scenario collections
  - **scenario loader**: future serialized ingestion path used to load external scenario libraries
- **verification case**: maintainer-authored smoke/CLI procedural validation input
- **verification case catalog**: maintainer tooling selector/list for verification cases (`verify_case_catalog`)
- **fixture**: static payload/data artifact, often consumed by tooling or UI/harness checks

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

## 5. Canonical scenario model (first slice)

The first supported canonical scenario form is **in-memory authored data**.

In-scope representation characteristics:

- an authored list/sequence of scheduled timeline events
- explicit synthetic timestamp (`at_ns`) per event
- deterministic intra-timestamp ordering (stable sequence/tie-breaker)
- provider-owned execution against active stream/device state

Tranche 1 persistence boundary:

- execution support is in-memory authored data only
- this is an implementation-slice constraint, not an architectural rejection of persisted/open forms

Out of scope for this tranche:

- file formats and schemas
- persistence/recording pipeline design

### 5.1 Single-scenario-model clarification

CamBANG continues to use **one scenario model**.

- scenarios remain primitive-capable
- existing primitive lifecycle vocabulary remains valid for diagnostics and replay
- this clarification does **not** introduce a second scenario-handling model

There is currently no alternate runtime scenario model that replaces primitive events.

---

## 6. Host integration boundary

Godot is a thin host/stepper/observer for scenario execution.

Godot may:

- select a scenario (from authored in-memory set)
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
  - purpose: preserve thin-host explicit-control semantics for
    Godot/dev-hosted scenarios

This is a compatibility arming rule, not a semantic ownership split:
both paths remain SyntheticProvider-owned timeline execution.

---

## 7. Initial event vocabulary and expansion boundary

Current implemented executable slice includes:

- stream flow events: `StartStream`, `StopStream`, `EmitFrame`
- minimal realization/lifecycle events: `OpenDevice`, `CloseDevice`, `CreateStream`, `DestroyStream`
- stream-scoped appearance update event: `UpdateStreamPicture` (maps through provider `PictureConfig` semantics)

This implemented slice is still a starting boundary, not the architectural ceiling.

Canonical scenario direction remains a self-contained authored/recorded timeline unit, and event vocabulary may still expand further as needed without moving semantic authority into host glue.
Tranche 1 executable vocabulary may begin with a small subset aligned with current timeline code shape:

- `StartStream`
- `StopStream`
- `EmitFrame`

That subset is an implementation starting point, not the architectural ceiling.

Canonical scenario direction remains a self-contained authored/recorded timeline unit. The event model is therefore expected to expand with minimal lifecycle/realization events needed to author and replay scenarios without hidden host-side semantic reconstruction (for example, device/stream realization and create/destroy-style families).

Tranche 1 intentionally does not freeze the complete long-term scenario event vocabulary.

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

Any future work on this issue, if adopted later, must stay within the single scenario model and is currently only under consideration for **reduction-facing** teardown handling (close/destroy facing), not implicit upward realization (`OpenDevice` / `CreateStream` / `StartStream`).

---

## 8. Scenario source compatibility requirement

Scenario architecture must remain compatible with all expected source families:

- developer/maintainer authored scenarios
- recorded behavior from real hardware-backed providers for later synthetic playback
- user/integrator hand-authored open serialized forms (for example JSON-like formats)
- future GUI authoring tools built as separate layers

This is a design-compatibility requirement. It is not a Tranche 1 decision about serialization format, recording pipeline, or editor UX.

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

- serialization format/schema decisions
- recording pipeline/format design
- editor/authoring UI tooling
- release UX design for scenario libraries (built-in/external)
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
