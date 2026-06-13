# Agent Context

This document gives repo-specific context for AI coding agents working on CamBANG. It supports the root `AGENTS.md` by recording durable project expectations that are too specific for the root file but too stable to repeat in every prompt.
For active tranche handoff tasks, `docs/dev/current_tranche.md` may provide additional current workstream context when the user explicitly requests it.
## Source authority

The checked-out repository is authoritative.

Do not infer current code state from previous chats, summaries, memories, or stale documentation when source inspection is possible. Inspect the relevant files before proposing or making changes.

When documentation and source disagree:

1. Treat source and tests as the first authority.
2. Treat documentation as possibly stale.
3. Report the mismatch instead of silently choosing one.

## Project shape

CamBANG is a deterministic imaging core with a Godot-facing API.

The system is organised around:

* a Core runtime responsible for lifecycle, ownership, publication, and result truth;
* providers that supply platform-backed or synthetic imaging behaviour;
* Godot-facing wrappers and scenes that expose and verify the public API;
* smoke, scenario, compliance, and runtime validation tools.

Core design priorities:

* truthful state publication;
* deterministic lifecycle and teardown;
* thread-safe ownership boundaries;
* simple user-facing API;
* provider/platform seams that can support Synthetic, Windows Media Foundation, Android Camera2, and future providers.

Avoid adding concepts, layers, public API names, diagnostics, or abstractions unless they clearly earn their keep.

## Public API caution

The Godot public API is considered locked unless a task explicitly says otherwise.

Do not rename or reshape public Godot methods, signals, constants, or expected scene behaviours merely to make an implementation easier.

Normal users should not need capture IDs, internal route descriptors, or debugging concepts. Keep advanced and developer tooling separate from the friendly object-level API.

## Snapshot and lifecycle expectations

Snapshot truth matters more than cosmetic simplicity.

Important expectations:

* published snapshots should reflect retained truth, not provider-local staging details;
* lifecycle phases must not be faked to satisfy tests;
* NIL, baseline, restart, and teardown behaviour must remain deterministic;
* destroyed, retained, and live objects must remain distinguishable when that distinction is meaningful;
* teardown should avoid blocking render-sensitive or Godot-sensitive paths.

When changing snapshot, result, or lifecycle logic, consider effects on:

* CoreRuntime publication;
* native object registry/state;
* provider compliance;
* Godot wrappers;
* status panel/editor visibility;
* scenario verification;
* restart-boundary verification.

## Provider boundaries

Provider seams are important.

SyntheticProvider is deterministic and cross-platform. Platform-backed providers should not be constrained by Synthetic-only implementation shortcuts unless the design explicitly justifies it.

Do not assume a SyntheticProvider optimisation represents Windows MF, Android Camera2, or future platform-backed behaviour.

When working near provider code, preserve the distinction between:

* provider-local staging data;
* retained Core-visible payload or backing truth;
* CPU payload availability;
* GPU/display backing availability;
* advertised capability;
* actual retained result access.

## Testing philosophy

Do not weaken tests, smoke tools, or Godot verification scenes merely to get PASS.

Tests may be updated when the design intentionally changes, but changes should preserve or increase the strength of verification. If a test failure reveals a real sequencing, ownership, lifecycle, or truth mismatch, report it rather than papering over it.

Manual local validation remains authoritative for:

* Windows-specific behaviour;
* Godot editor/runtime scenes;
* hardware-backed camera behaviour;
* GPU/display-view behaviour;
* platform-provider behaviour.

Cloud or sandbox validation is useful, but it does not prove those paths unless they were actually exercised.

## Build and generated files

Avoid editing generated outputs or build artefacts.

Common generated/build-output areas include:

* `out/`;
* object files and compiler outputs;
* generated compile databases unless the task explicitly concerns them;
* Godot binary artefacts;
* third-party build outputs.

Prefer source, docs, tests, and build scripts over generated files.

## Documentation expectations

Documentation should describe current design.

Avoid preserving named tombstones for deliberately removed concepts unless there is clear maintainer value. Do not keep references to things that no longer exist by design.

Keep documentation:

* source-grounded;
* concise;
* clear about stable design decisions;
* free of obsolete implementation detail;
* separated from temporary planning notes.

Agent-guidance maintenance:

* keep `docs/dev/current_tranche.md` current after accepted or committed tranches;
* use `current_tranche.md` for volatile active-workstream state, recent committed checkpoints, validation status, and near-term constraints;
* use `docs/dev/agent_context.md` only for durable cross-tranche expectations that should persist beyond the current workstream;
* do not duplicate canonical architecture in either guidance file.


## C++ expectations

Prefer clear, boring C++ over cleverness.

General expectations:

* preserve ownership clarity;
* avoid hidden cross-thread lifetime assumptions;
* keep error and status propagation explicit;
* avoid broad refactors mixed into narrow fixes;
* avoid speculative performance changes without evidence;
* keep public-facing names simple and internal names precise.

When changing synchronous Core or provider paths, do not assume synchronous work is accidental. Some synchronous boundaries exist to preserve snapshot truth, thread safety, or deterministic sequencing.

## Agent workflow

For audits:

* remain read-only unless explicitly asked to edit;
* cite files and functions inspected;
* separate confirmed findings from hypotheses;
* avoid proposing patches without a concrete issue.

For edits:

* keep scope narrow;
* inspect before editing;
* identify expected changed files for non-trivial work;
* avoid unrelated cleanup;
* report changed files and rationale;
* list validation commands;
* say plainly what was not run.

For Codex/Web agents:

* do not claim local Windows, Godot, hardware, or GPU validation unless actually performed;
* do not create commits unless explicitly asked;
* do not add persistent diagnostic knobs for temporary investigation;
* do not add environment variables as convenience workarounds unless explicitly approved.
