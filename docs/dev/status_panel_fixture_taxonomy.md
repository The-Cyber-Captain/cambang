# Status Panel Fixture Taxonomy

Status: Current dev reference (StatusPanel fixture taxonomy and harness intent)

This note documents the fixture taxonomy and harness intent used by
`CamBANGStatusPanel` verification.

It distinguishes expected-invalid fixtures from accidental invalidity,
records intended validation stages, and documents current renderer/NPS
fixture metadata used by the harness.

## 1) Scope and goals

The harness must test both:
- strict authoritative snapshot contract handling, and
- retained/model/projection behavior used by panel continuity logic.

It must intentionally cover valid and invalid inputs while clearly
separating:
- **expected-invalid test data** (deliberate), from
- **accidental invalidity** (fixture rot or harness bug).

---

## 2) Fixture taxonomy

We define four top-level fixture classes.

## A. Valid authoritative snapshot fixtures

Purpose:
- prove happy-path compatibility with the canonical snapshot schema
- exercise semantic edge states that are still schema-valid

Examples:
- minimal baseline snapshot (`version=0`, empty arrays where allowed)
- fully populated mixed topology (rig/device/stream/native object coverage)
- schema-valid sparse truth (`0`/empty values where runtime truth may be unknown)
- enum edge values that are valid but not commonly emitted in current scaffolding

Expected behavior:
- schema validation passes
- projector (if enabled) runs
- panel output reaches deterministic “valid renderable state”

## B. Malformed snapshot fixtures

Purpose:
- verify hard rejection of invalid authoritative payloads

Examples:
- wrong `schema_version`
- missing required field
- unknown extra property (closed object violation)
- wrong scalar type/range (e.g., negative for uint field)
- invalid enum value

Expected behavior:
- fail during snapshot schema validation (or decode gate)
- projector does **not** run
- panel remains in rejected/error input state

## C. Valid retained/model fixtures

Purpose:
- verify panel-retained/model continuity logic independent of raw snapshot parsing

Examples:
- retained expansion/sort/filter metadata with valid projection references
- continuity caches that align with projected authoritative IDs
- legal transitions from previous model → next model

Expected behavior:
- retained/model schema checks (if present) pass
- projection merge/apply passes
- panel outcome matches expected continuity behavior

## D. Malformed retained/model/projection fixtures

Purpose:
- verify model/projection gate behavior when non-authoritative panel data is invalid

Examples:
- retained state references nonexistent IDs in projection
- malformed retained structure/type mismatch
- projection artifact inconsistent with authoritative snapshot-derived model
- illegal transition metadata (e.g., stale lineage assumptions)

Expected behavior:
- fail at retained/model/projection validation stage
- authoritative snapshot may still be valid
- outcome is deterministic fallback/recovery path (or explicit reject), per case

---

## 3) Current per-fixture metadata

Each current fixture is a single JSON file. Fixture intent and expected outcome
are encoded as top-level fields in that same file, with the authoritative input
under `payload` for snapshot fixtures.

```json
{
  "fixture_kind": "authoritative_snapshot",
  "provider_mode": "synthetic",
  "expected_validity": "invalid",
  "should_run_projector": false,
  "expected_panel_outcome": {
    "contract_gap_count": 1,
    "projection_gap_count": 0
  },
  "payload": {
    "schema_version": 1
  },
  "renderer_profile": "any",
  "nps_scope": "none"
}
```

## Required fixture fields in the current harness

- `fixture_kind` (string)
  - current harness paths include `authoritative_snapshot` and
    `continuity_no_snapshot`.
- `expected_validity` (string)
  - `valid` or `invalid`.
- `should_run_projector` (boolean)
  - explicit expectation to prevent accidental projector-stage drift.
- `expected_panel_outcome` (object)
  - rendered-row, badge, counter, and gap assertions consumed by the harness.
- `payload`
  - authoritative snapshot payload for `authoritative_snapshot` fixtures.
- `renderer_profile`
  - `any`, `gpu`, `compatibility`, or `ambiguous`.
- `nps_scope`
  - `none`, `structural`, `aggregate`, `gpu_leaf`, `compatibility_leaf`, or
    `anomaly`.

## Common optional fixture fields

- `provider_mode`
- `initial_expanded_row_ids`
- `panel_exports`
- `adversarial_projection`
- `authoritative_prelude_payload`
- `authoritative_observed_payloads`
- `continuity_prelude_payload`

---

## 4) Harness preflight sequence

The harness must run a deterministic preflight before executing panel assertions.

## Shared preflight (all fixtures)

1. Load fixture JSON.
2. Confirm required top-level fixture metadata is present and internally
   consistent for the fixture kind.
3. Normalize taxonomy fields such as `renderer_profile` and `nps_scope`.
4. For `authoritative_snapshot`, validate `payload` with the snapshot schema
   validator before deciding whether projection should run.

If any shared preflight step fails because the fixture metadata is malformed or
contradictory, classify as **harness/fixture-definition error** (not SUT
behavior).

## Preflight for fixtures expected valid

1. Decode snapshot/retained/projection inputs (as applicable).
2. Run authoritative snapshot schema validation if snapshot present.
3. Run retained/projection schema checks if applicable.
4. Run semantic gate checks required for the fixture kind.
5. Assert no failure occurred prior to expected execution stage.
6. If `should_run_projector = true`, require projector execution and success.

Any failure before panel assertion is **unexpected** and should fail the test as
accidental invalidity.

## Preflight for fixtures expected invalid

1. Decode inputs as far as needed to validate the declared fixture kind.
2. Execute schema/compatibility gates in order until the first failure or fallback
   condition is observed.
3. Assert `should_run_projector` behavior is respected:
   - if false, projector must not run,
   - if true, failure/fallback must occur after projector entry.
4. Assert expected panel outcome/gap/error fields match the fixture metadata.

If fixture passes all gates unexpectedly, fail as **accidental validity drift**.
If it fails at a different stage, fail as **taxonomy mismatch**.

---

## 5) Distinguishing accidental invalidity vs expected-invalid data

Use explicit classification in test output:

- `EXPECTED_INVALID_CONFIRMED`
  - fixture marked invalid, failed at declared stage.
- `UNEXPECTED_INVALID`
  - fixture marked valid, but failed preflight/validation.
- `UNEXPECTED_VALID`
  - fixture marked invalid, but did not fail at declared stage.
- `INVALID_STAGE_MISMATCH`
  - fixture marked invalid, failed but at different stage.
- `HARNESS_CONFIG_ERROR`
  - fixture metadata malformed, missing required data, or contradictory fields.

This classification prevents silent acceptance of broken fixtures and keeps
negative tests intentional.

---

## 6) Current directory layout

StatusPanel harness fixtures currently live under the Godot test project at:

```text
tests/cambang_gde/fixtures/status_panel/
  fixture_<fixture_id>.json
  review_multistream/
    fixture_review_multistream_<fixture_id>.json
  review_orphan/
    fixture_review_orphan_<fixture_id>.json
```

The active harness consumes these fixture JSON files directly, for example:

```bash
godot4 --headless --path tests/cambang_gde --script res://scripts/status_panel_harness.gd -- fixtures/status_panel/fixture_valid_basic_authoritative.json
```

The taxonomy concepts above describe how fixtures should be classified and
reviewed; they do not imply a separate manifest-per-fixture directory layout in
the current repository. Keep fixture payloads small and purpose-specific; use
explicit fixture fields/tags for cross-cutting selection rather than duplicating
large payloads.

---

## 7) Practical policy recommendations

- Require every invalid fixture to declare explicit expected validity and panel outcome/gap expectations.
- Freeze fixture metadata under code review as strictly as test code.
- Add a CI summary table grouped by classification labels above.
- Treat fixture metadata/schema drift as a blocking failure, not warning-only.

---

## 8) Renderer/NPS taxonomy metadata (current fixture-level extension)

StatusPanel fixtures now carry two explicit classification fields:

- `renderer_profile` (enum):
  - `any`
  - `gpu`
  - `compatibility`
  - `ambiguous`
- `nps_scope` (enum):
  - `none`
  - `structural`
  - `aggregate`
  - `gpu_leaf`
  - `compatibility_leaf`
  - `anomaly`

### Meaning

- `renderer_profile=any` means expectations are intended to be renderer-invariant.
- `renderer_profile=gpu` means fixture expectations are specifically tied to GPU renderer behavior.
- `renderer_profile=compatibility` means fixture expectations are specifically tied to compatibility/non-GPU renderer behavior.
- `renderer_profile=ambiguous` means current expectations likely mix invariant and renderer-sensitive assumptions and need reconciliation.

### Native Payload Support (NPS) sensitivity rule

NPS leaf-level expectations are not globally invariant: GPU and compatibility renderers can legitimately produce different native-support leaf shapes and therefore different leaf-level rows/badges/counters/info details.

Policy consequence:

- Keep renderer-invariant structural assertions (e.g. parent grouping, existence of NPS grouping rows) separate from renderer-specific leaf assertions.
- Do not “fix” mismatches by making one fixture accept both leaf shapes.
- Prefer split fixtures and explicit renderer-profile targeting for leaf-sensitive expectations.

### Split naming convention (narrow rollout)

For a renderer-sensitive fixture family `<base>.json`, use:

- `<base>.json` (or `<base>_common.json`) for renderer-invariant structural/aggregate assertions
- `<base>_gpu.json` for GPU leaf assertions
- `<base>_compatibility.json` for compatibility leaf assertions

When only one renderer-specific payload is currently evidenced, keep the common fixture and the evidenced renderer fixture, and defer the other renderer-specific variant until real snapshot evidence is captured. A separate `_common` file is optional when `<base>.json` already serves as the canonical common fixture.

This keeps snapshot-contract testing, projection testing, and panel continuity
checks decoupled but composable.
