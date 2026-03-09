# CamBANG Documentation Index

This directory contains both canonical architectural documentation and
development-stage notes.

To prevent drift and contradiction, documents are categorised
explicitly.

------------------------------------------------------------------------

## 1. Canonical Architecture (Source of Truth)

These documents define CamBANG's architectural model and design intent.
They are authoritative and must not be contradicted by other documents.

-   provider_architecture.md
-   core_runtime_model.md
-   arbitration_policy.md
-   state_snapshot.md
-   naming.md
-   repo_structure.md

Note:

Godot-facing snapshot publication uses a **tick-bounded observable truth**
model (≤ 1 `state_published` emission per Godot tick when changed). This
contract is defined in `state_snapshot.md` and the boundary mechanics are
described in `core_runtime_model.md`.

Validation layering (core invariant validation vs platform integration
validation) is defined in `core_runtime_model.md`.

Platform-specific validation documents must not redefine core invariants.

If any supplementary document appears to conflict with these, the
canonical architecture documents take precedence.

Changes to canonical documents should be deliberate and reviewed
carefully.

------------------------------------------------------------------------

## 2. Architectural Supplements

Located in:

docs/architecture/

These documents clarify or extend specific aspects of the canonical
model, but do not redefine it.

They must:

-   Explicitly reference the canonical document(s) they supplement.
-   Avoid duplicating or restating core architecture unnecessarily.
-   Remain narrowly scoped.

Example:
- architecture/frame_sinks.md
- architecture/pattern_module.md
- architecture/godot_boundary_contract.md
------------------------------------------------------------------------

## 3. Development / Scaffolding Notes

Located in:

docs/dev/

These documents describe:

-   Build stages
-   Temporary scaffolding
-   Development-only utilities
-   Diagnostic tools

They are intentionally non-canonical and may evolve or be removed.

Example:

- dev/frameview_stage.md
- dev/godot_abuse_scenes.md
- dev/maintainer_tools.md
    - describes maintainer CLI validation tools
    - includes provider_compliance_verify
      (deterministic provider-contract verification using Stub and Synthetic)
    - includes restart_boundary_verify
      (deterministic verification of the CamBANGServer restart
      NIL-before-baseline boundary contract)
    - includes windows_mf_runtime_validate
      (opt-in Windows Media Foundation runtime validation against real hardware)

These documents must clearly state when code is: - Development-only -
Intended for replacement - Not representative of release architecture

### Additional provider-discipline note

Provider semantics are defined by the canonical architecture, the
reference providers, and the compliance verifier / harness.

Platform-backed providers are adapters to that contract. They must not
redefine lifecycle, defaulting, registry, snapshot, or timestamp
semantics to match a backend API.

This is especially important as additional platform-backed providers are
introduced (for example future `android_camera2` alongside
`windows_mediafoundation`).


------------------------------------------------------------------------

## Documentation Discipline Rules

1.  Canonical architecture documents are the single source of truth.
2.  Supplements must reference, not replace, canonical docs.
3.  Dev notes must explicitly mark temporary code paths.
4.  Avoid duplicating architectural explanations across files.
5.  If in doubt, update a canonical document rather than creating a
    parallel explanation.

------------------------------------------------------------------------

This structure ensures that CamBANG documentation remains precise,
authoritative, and resistant to drift as the project evolves.
