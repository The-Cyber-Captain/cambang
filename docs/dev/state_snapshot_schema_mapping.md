# State Snapshot Schema Mapping (v1)

Status: Dev note (authoring and review aid)

This note explains how to interpret the v1 snapshot contract layers without
mixing concerns.

## 1) Layer separation (what governs what)

### A. Documented contract (human-readable, normative behavior)
Primary docs:
- `docs/state_snapshot.md`
- `docs/architecture/godot_boundary_contract.md`
- `docs/architecture/publication_model.md`
- `docs/dev/snapshot_truth_rules.md`

This layer defines:
- Godot-facing observable behavior (`state_published`, tick-bounded coalescing,
  baseline publication, stop/start boundaries)
- field meanings and truth model
- lifecycle/mode vocabulary and interpretation

### B. Canonical machine-readable schema (wire shape/type contract)
Primary artifact:
- `schema/state_snapshot/v1/state_snapshot_schema.json`

This layer defines:
- exact JSON object/array shape
- required fields
- scalar types/ranges (`uint32`, `uint64`, `int32`)
- enum domains (wire-encoded integer values)
- closed-object policy (`additionalProperties: false`)
- version discrimination (`schema_version = 1`)

The schema is authoritative for *shape/type/enums* of the public payload.

### C. Semantic/runtime invariants (outside JSON Schema expressivity)
These are authoritative behavior/truth constraints that exceed schema scope,
for example:
- tick-bounded emission rate and coalescing
- generation/version/topology counter progression rules
- cross-record ownership and lifecycle interpretation
- “no fabricated truth” requirements

These are validated by runtime behavior, review, and verification tests/cases — not by
schema alone.

### D. Panel lightweight checks (UI/tool-side sanity checks)
Consumer-side checks should remain cheap and non-authoritative, e.g.:
- `schema_version == 1`
- required arrays exist and are arrays
- enum values are in expected domain
- obvious header monotonicity from last seen snapshot

These checks improve UX/diagnostics but do not replace schema validation or
runtime/verifier validation.

### E. Verifier validation (authoritative behavioral verification)
Higher-confidence validation (integration/verification-case verifier) should verify:
- boundary semantics (baseline publish, stop NIL window, restart generation)
- tick-bounded coalescing behavior
- semantic invariants that are cross-snapshot/cross-record

Verifier-level assertions are where semantic/runtime rules are enforced.

---

## 2) Invariant split: schema-level vs semantic-only

## Schema-level invariants (enforced by JSON Schema)

- Top-level object is closed; unknown top-level properties are rejected.
- `schema_version` is present and fixed to `1`.
- Header fields are present with integer type/range:
  - `gen`, `version`, `topology_version`, `timestamp_ns`, `imaging_spec_version`.
- `rigs`, `devices`, `acquisition_sessions`, `streams`, `native_objects`,
  `detached_root_ids` are present arrays.
- Every record object type is closed (`additionalProperties: false`) and has a
  frozen required field set.
- `AcquisitionSessionState` participates in the same closed-record required-field
  contract as other top-level state records.
- `NativeObjectRecord` requires `owner_acquisition_session_id` (uint64; `0` for
  none/unknown).
- Field scalar types are fixed (`uint32`/`uint64`/`int32`/`boolean`/`string`).
- Enum domains are fixed for wire-encoded integers:
  - lifecycle `phase`
  - rig/device/stream `mode`
  - stream `intent`
  - stream `stop_reason`

## Semantic-only invariants (not expressible or intentionally not encoded in schema)

- Tick-bounded publication:
  - at most one Godot-visible publish per tick,
  - coalescing of intermediate internal states.
- Counter progression semantics:
  - baseline publish per generation at `(version=0, topology_version=0)`,
  - contiguous `version` progression,
  - `topology_version` changes only alongside `version` and only on structural changes.
- Stop/start boundary behavior:
  - `get_state_snapshot() == NIL` after completed stop,
  - pre-baseline NIL window after restart,
  - no stale prior-generation exposure.
- Truth model constraints:
  - no fabricated values,
  - unknown remains zero/empty/NIL until runtime truth exists.
- Cross-record/cross-field logic:
  - top-level `acquisition_sessions[]` is authoritative for current/live
    `AcquisitionSession` truth,
  - historical teardown diagnosis remains available through retained
    `native_objects` history,
  - detached-root interpretation from owner/end-state relationships,
  - ownership/linkage consistency,
  - optional semantic rules like “at most one active repeating stream per device”.

---

## 3) Explicit non-scope: retained UI continuity state

Retained UI continuity state is **not part of the authoritative snapshot schema**.

Examples (non-authoritative, consumer-local state):
- panel expansion/collapse selections
- row pinning/sorting/filtering state
- client-side display aliases, focus, transient highlighting
- local “last seen” convenience caches used only for presentation continuity

Such UI continuity data must not be added to the authoritative snapshot payload
or inferred as part of schema truth.

---

## 4) Current v1 ambiguities / scaffolding realities intentionally permitted

The v1 schema intentionally allows some values/states that may be sparse or not
fully exercised in the current scaffolding slice.

1. Stream stop-reason domain includes `PREEMPTED` in the enum vocabulary,
   even though the current builder path in this slice primarily emits
   `NONE`/`USER`/`PROVIDER`.

2. Stream mode domain includes `STARVED` and `ERROR`, while current scaffolding
   projection may effectively emit only `STOPPED`/`FLOWING` from retained truth in
   this slice.

3. Lifecycle `phase` domain includes `TEARING_DOWN`, even if current retained
   projections may not populate that state across all record categories yet.

4. Rigs are structurally present in the contract and required at top level, but
   may be an empty array in current scaffolding where rig population is not yet
   fully implemented.

5. `type` in `NativeObjectRecord` is defined as `uint32` (CamBANG-owned
   vocabulary). The schema intentionally does not freeze to a closed enum in v1 to
   avoid over-constraining evolving provider-reported type usage in this slice.

6. Schema fixes shape/type/enums only; it intentionally permits runtime-legal zero
   values (`0`, empty arrays/strings where documented) for not-yet-known truth.

7. `AcquisitionSession` naming in v1 is shared across:
   - contract/state shape: `AcquisitionSessionState`,
   - id fields: `acquisition_session_id`,
   - native ownership linkage: `owner_acquisition_session_id`.

8. Current implementation scope note: the concrete implemented
   `AcquisitionSession` seam is stream-backed in `SyntheticProvider`; a
   still-only realization is not yet implemented.

These allowances are intentional to keep the v1 contract stable while runtime
coverage and retained truth fidelity continue to mature.
