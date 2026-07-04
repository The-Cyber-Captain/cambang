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
- file-backed/user-provided collections belong to the **external scenario library**
- serialized ingestion for external scenario JSON belongs to **scenario loader**

Implemented external loading does not by itself define a finalized recording/editor UX or broad public scenario-file product surface.

Godot/host layers may request scenario execution, but must not become the semantic owner of appearance-timeline meaning.

---

## 4. Implemented scenario appearance scope

Scenario-authored synthetic appearance currently has two explicit event scopes:

- `UpdateStreamPicture` updates stream-scoped appearance state for subsequent
  emitted stream frames.
- `UpdateCapturePicture` updates capture-side appearance state for later
  API-driven still-capture output on the target device.

Both map through provider-owned `PictureConfig` semantics. Appearance state is
authored timeline intent; it is not structural capture-profile truth and does
not author CPU/GPU backing policy.

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

Scenario appearance state uses provider-owned `PictureConfig` semantics for synthetic rendering.

This preserves a clear boundary:

- scenario timeline owns authored appearance changes over time
- provider owns application of those changes to stream and capture render state
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

This tranche excludes:

- GUI/editor tooling design
- full recording pipeline design
- broad release UX design
- unrelated runtime/provider architecture redesign

---

## 9. Immediate implementation implications

Current scenario appearance behaviour includes:

1. provider-owned scenario-authored stream appearance updates, persistent until changed
2. provider-owned scenario-authored capture appearance updates, persistent until changed
3. canonical dev scenarios routing visual/source distinction through provider timeline appearance events
4. host-side pattern cycling remaining manual/debug-only rather than canonical scenario semantics

Remaining discipline:

- keep Godot thin: select, start, stop, and observe
- keep host-side pattern cycling explicitly manual/debug-only
- avoid broadening this layer into editor, recording, or release UX design

