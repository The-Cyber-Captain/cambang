# CamBANG Documentation Index

This directory contains both canonical architectural documentation and
development-stage notes.

To prevent drift and contradiction, documents are categorised explicitly.

---

## 1. Canonical architecture (source of truth)

These documents define CamBANG’s architectural model and design intent.
They are authoritative and must not be contradicted by other documents.

- `provider_architecture.md`
- `core_runtime_model.md`
- `arbitration_policy.md`
- `state_snapshot.md`
- `naming.md`
- `repo_structure.md`

Notes:

- Godot-facing snapshot publication uses a **tick-bounded observable truth**
  model (≤ 1 `state_published` emission per Godot tick when changed).
  The public contract is defined in `state_snapshot.md`.
- Validation layering (core invariant validation vs platform integration
  validation) is defined in `core_runtime_model.md`.
- Platform-specific validation documents must not redefine core invariants.

If any supplementary document appears to conflict with these, canonical
architecture documents take precedence.

Changes to canonical documents should be deliberate and reviewed carefully.

---

## 2. Architectural supplements

Located in:

```text
docs/architecture/
```

These documents clarify or extend specific aspects of the canonical model,
but do not redefine it.

They must:

- explicitly reference the canonical document(s) they supplement
- avoid duplicating or restating core architecture unnecessarily
- remain narrowly scoped

The lists in this index are intended to be complete for the current canonical
documentation set.
Any change that adds, removes, renames, or reclassifies a canonical or
supplement document must update this index in the same change.

Current supplements:

Architectural supplements provide focused explanations of subsystems,
edge cases, or conceptual models that support the canonical documents.

They may include diagrams, examples, and reasoning aids, but must not
redefine canonical rules.

| Document | Purpose |
|---|---|
| lifecycle_model.md | Explains lifecycle hierarchy and event flow across provider → core → Godot. |
| provider_state_machines.md | Defines provider/device/stream/frame-producer state machines and valid transitions. |
| provider_strand_model.md | Clarifies provider-strand delivery rules and event-class guarantees. |
| publication_model.md | Describes tick-bounded publication and Godot-visible snapshot behaviour. |
| publication_counter_examples.md | Provides worked examples illustrating `version` and `topology_version`. |
| frame_sinks.md | Describes frame sink types and responsibilities. |
| pixel_payload_and_result_contract.md | Defines the multi-representation payload/result contract for release-facing stream and capture paths. |
| synthetic_timeline_scenarios.md | Fixes scenario terminology and ownership boundaries for SyntheticProvider timeline work. |
| synthetic_picture_appearance_in_scenarios.md | Defines scenario-authored synthetic appearance/state boundaries for timeline replay. |
| pattern_module.md | Explains the deterministic pattern generator module used for testing and diagnostics. |
| godot_boundary_contract.md | Consolidates the externally visible Godot-facing runtime contract. |

### Reading guidance

For lifecycle behaviour, read:

1. provider_architecture.md (canonical rules)
2. lifecycle_model.md (hierarchy and event flow)
3. provider_state_machines.md (resource state transitions)

For snapshot publication and Godot-boundary behaviour, read:

1. state_snapshot.md (canonical snapshot structure and counter meaning)
2. godot_boundary_contract.md (canonical observable Godot-facing contract)
3. publication_model.md (tick-bounded publication mechanics and coalescing)
4. publication_counter_examples.md (counter interpretation examples)

For release-facing image/result architecture, read:

1. frame_sinks.md (internal sink boundary and current result-oriented split)
2. pixel_payload_and_result_contract.md (payload kinds, ownership, retention, and materialization)
3. naming.md (public result vocabulary and Godot-facing terminology)

### Roles of current supplements include:

- `lifecycle_model.md` explains hierarchy, lifecycle phases, and provider-strand event flow
- `provider_state_machines.md` provides a compact reference of provider lifecycle
  and resource-state axes
- `provider_strand_model.md` clarifies serialized provider → core delivery and non-droppable event classes
- `publication_model.md` explains internal-vs-observable publication and tick-bounded coalescing
- `publication_counter_examples.md` illustrates progression of `gen`, `version`,
  and `topology_version` across representative runtime situations
- `frame_sinks.md` explains the core frame-consumer boundary
- `pixel_payload_and_result_contract.md` defines the release-facing multi-representation payload/result contract, including retention, ownership, and materialization
- `godot_boundary_contract.md` consolidates the externally visible Godot-facing runtime contract
- `pattern_module.md` documents synthetic pixel rendering

---

## 3. Development / scaffolding notes

Located in:

```text
docs/dev/
```

These documents describe:

- build stages
- temporary scaffolding
- development-only utilities
- diagnostic tools
- platform-specific implementation notes

They are intentionally non-canonical and may evolve or be removed.

Current dev notes:

- `dev/build_and_scaffolding.md`
- `dev/frameview_stage.md`
- `dev/godot_boundary_verification_scenes.md`
- `dev/maintainer_tools.md`
- `dev/pixel_result_architecture_direction.md`
- `dev/provider_compliance_checklist.md`
- `dev/snapshot_truth_rules.md`
- `dev/synthetic_timeline_reconciliation_notes.md`
- `dev/upstream_discrepancies.md`
- `dev/windows_mf_visibility_phase.md`

These documents must clearly state when code is:

- development-only
- intended for replacement
- not representative of release architecture

### Additional provider-discipline note

Provider semantics are defined by the canonical architecture, the
reference providers, and the compliance verifier / harness.

Platform-backed providers are adapters to that contract. They must not
redefine lifecycle, defaulting, registry, snapshot, or timestamp
semantics to match a backend API.

This is especially important as additional platform-backed providers are
introduced.

---

## 4. Documentation discipline rules

1. canonical architecture documents are the single source of truth
2. supplements must reference, not replace, canonical docs
3. dev notes must explicitly mark temporary code paths
4. avoid duplicating architectural explanations across files
5. if in doubt, update a canonical document rather than creating a parallel explanation
6. maintain terminology discipline: use **verification case** for maintainer smoke/CLI authored validation inputs, and reserve **scenario** for user/Godot/SyntheticProvider timeline meaning

---

This structure helps keep CamBANG documentation precise, authoritative,
and resistant to drift as the project evolves.

## Recently Consolidated Topics

| Former Topic | Current Location |
|---|---|
| Provider strand detail | `architecture/provider_strand_model.md` |
| Snapshot truth guidance | `dev/snapshot_truth_rules.md` |
| Godot boundary verification scenes | `dev/godot_boundary_verification_scenes.md` |
| Pixel/result architecture direction | `dev/pixel_result_architecture_direction.md` |
| Payload/result contract | `architecture/pixel_payload_and_result_contract.md` |
