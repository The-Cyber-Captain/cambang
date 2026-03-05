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

Example: - architecture/frame_sinks.md

         - architecture/pattern_module.md
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

Example: - dev/frameview_stage.md

These documents must clearly state when code is: - Development-only -
Intended for replacement - Not representative of release architecture

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
