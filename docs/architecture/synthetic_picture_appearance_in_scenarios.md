# Synthetic Picture Appearance in Scenarios (Architecture Supplement)

This supplement defines how scenario-authored synthetic visual/source choice is represented for SyntheticProvider timeline scenarios and replay.

It supplements:

- `docs/naming.md`
- `docs/provider_architecture.md`
- `docs/architecture/pattern_module.md`
- `docs/architecture/synthetic_timeline_scenarios.md`

It does not redefine canonical runtime or snapshot authority documents.

---

## 1. Purpose

This note resolves a boundary ambiguity:

- where synthetic visual/source choice belongs conceptually
- how scenarios should author that choice for deterministic replay
- how to retire host-side debug pattern mutation as the primary mechanism

---

## 2. Conceptual placement

Synthetic visual/pattern/source choice belongs to **picture appearance intent**, not structural capture-profile semantics.

- Capture Profile remains structural (width/height/format/fps and related structural capture properties).
- Synthetic visual distinction belongs to appearance state (preset/seed/overlay-style intent).

---

## 3. Scenario role

Scenarios may author synthetic picture appearance state for replayed/emitted frame production.

This is a legitimate scenario capability, including replay of recorded platform timelines where visually distinct synthetic source rendering is needed for human interpretation.

Terminology alignment:

- current C++ authored scenarios live in the **built-in scenario library**
- future file-backed collections belong to the **external scenario library**
- serialized ingestion for external libraries belongs to **scenario loader**

Godot/host layers may request scenario execution, but must not become the semantic owner of appearance-timeline meaning.

---

## 4. First-slice recommendation

Preferred first slice: **stream-scoped appearance state**.

- Scenario can set/update appearance state for a stream/frame-producing context.
- Subsequent emitted frames use that appearance state until the scenario changes it.
- Appearance change is an authored timeline action, not a host-side debug mutation.

This keeps the model small and deterministic while supporting meaningful visual source distinction.

Implementation-status note (current verified path): built-in scenario appearance
authoring is currently verified through explicit `UpdateStreamPicture` events
in the timeline. `StartStream` embedded-picture fields are not documented here
as the canonical/effective stream-picture application route.

Per-frame appearance overrides may be considered later, but are not the primary first-slice model.

---

## 5. Relationship to replay

Replay of recorded hardware-backed timelines may need multiple distinguishable synthetic visual sources over time.

Scenario-authored appearance state provides that without:

- changing structural capture-profile semantics
- requiring ad-hoc host debug pattern cycling
- coupling human-visible source distinction to non-canonical glue logic

---

## 6. Relationship to Pattern Module

Scenario layer should reference stable synthetic appearance vocabulary and parameters (preset token/enum plus appearance parameters), while rendering remains in SyntheticProvider + Pattern Module internals.

The Pattern Module remains the canonical deterministic pixel-generation engine.

Scenario layer chooses **what appearance intent applies when**; Pattern Module determines **how pixels are rendered**.

---

## 7. Relationship to PictureConfig

Recommendation: first slice should use **provider-owned scenario appearance state that maps directly to existing `PictureConfig` semantics** for synthetic rendering.

This avoids introducing a parallel appearance model while preserving a clear boundary:

- scenario timeline owns authored appearance changes over time
- provider owns application of those changes to stream render state
- Pattern Module renders deterministically from resulting appearance state

Scenario-authored appearance remains distinct from producer backing capability.

In particular:

- scenario chooses picture/source appearance intent through `PictureConfig`
- producer/provider determine which backing kinds are actually available in the current runtime
- scenario does not author CPU-backed vs GPU-backed realization policy

Future GPU-backed synthetic realization does not change this boundary.
Scenario continues to author appearance intent, while producer/provider own
backing capability and realization choice.
---

## 8. Non-goals

This tranche explicitly excludes:

- JSON/schema design for scenario appearance encoding
- GUI/editor tooling design
- full recording pipeline design
- broad release UX design
- unrelated runtime/provider architecture redesign

Capture-picture authoring behavior is intentionally left conservative here:
this note only freezes the currently verified stream-picture path above and
does not declare broader capture-picture authoring behavior as canonical yet.

---

## 9. Immediate implementation implications

Current Tranche 2.5 slice now includes:

1. provider-owned scenario-authored stream-scoped appearance updates, persistent until changed
2. canonical dev scenarios routing visual/source distinction through provider timeline appearance events
3. host-side pattern cycling no longer acting as the canonical scenario mechanism

Remaining immediate discipline:

- keep Godot thin (select/start/stop/observe only)
- keep host-side pattern cycling explicitly manual/debug-only
- continue avoiding serialization/editor/recording scope expansion in this layer
Next code tranche should:

1. add minimal scenario-authored appearance update support in provider timeline execution (stream-scoped, persistent-until-changed)
2. route dev scenario visual/source distinction through scenario-authored appearance updates
3. reduce reliance on host-side pattern cycling for scenario semantics
4. keep Godot thin: select/start/stop/observe; no appearance semantic reconstruction

This enables canonical scenario-driven visual distinction for replay and authored timelines without broadening into serialization/editor work.
